#include "vyuha/oms/pg_order_store.h"

#include <chrono>
#include <exception>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <quill/LogMacros.h>
#include <pqxx/pqxx>

#include "vyuha/util/backoff.h"
#include "vyuha/util/log.h"

namespace vyuha::oms {

namespace {

// A fixed title per known expiry reason, never built from data (risk review M2.2b).
[[nodiscard]] std::string_view expiry_alert_title(std::string_view reason) noexcept {
  if (reason == kExpiredInQueue) {
    return "ORDER_EXPIRED_IN_QUEUE";
  }
  if (reason == kExpiredOnRestart) {
    return "ORDER_EXPIRED_ON_RESTART";
  }
  return "ORDER_EXPIRED";
}

[[nodiscard]] std::string_view name(Side v) noexcept { return v == Side::kBuy ? "BUY" : "SELL"; }
[[nodiscard]] std::string_view name(OrderType v) noexcept {
  switch (v) {
    case OrderType::kLimit:
      return "LIMIT";
    case OrderType::kMarket:
      return "MARKET";
    case OrderType::kSl:
      return "SL";
    case OrderType::kSlM:
      return "SL-M";
  }
  return "";
}
[[nodiscard]] std::string_view name(Product v) noexcept {
  switch (v) {
    case Product::kCnc:
      return "CNC";
    case Product::kMis:
      return "MIS";
    case Product::kNrml:
      return "NRML";
  }
  return "";
}
[[nodiscard]] std::string_view name(Validity v) noexcept { return v == Validity::kDay ? "DAY" : "IOC"; }
[[nodiscard]] std::string_view name(Source v) noexcept {
  switch (v.kind()) {
    case Source::Kind::kManual:
      return "manual";
    case Source::Kind::kStrategy:
      return "strategy";
    case Source::Kind::kAiDraft:
      return "ai_draft";
    case Source::Kind::kSystem:
      return "system";
    case Source::Kind::kKillFlatten:
      return "kill_flatten";
  }
  return "";
}

[[nodiscard]] std::optional<Side> parse_side(std::string_view s) noexcept {
  for (const Side v : {Side::kBuy, Side::kSell}) {
    if (name(v) == s) {
      return v;
    }
  }
  return std::nullopt;
}
[[nodiscard]] std::optional<OrderType> parse_type(std::string_view s) noexcept {
  for (const OrderType v : {OrderType::kLimit, OrderType::kMarket, OrderType::kSl, OrderType::kSlM}) {
    if (name(v) == s) {
      return v;
    }
  }
  return std::nullopt;
}
[[nodiscard]] std::optional<Product> parse_product(std::string_view s) noexcept {
  for (const Product v : {Product::kCnc, Product::kMis, Product::kNrml}) {
    if (name(v) == s) {
      return v;
    }
  }
  return std::nullopt;
}
[[nodiscard]] std::optional<Validity> parse_validity(std::string_view s) noexcept {
  for (const Validity v : {Validity::kDay, Validity::kIoc}) {
    if (name(v) == s) {
      return v;
    }
  }
  return std::nullopt;
}
[[nodiscard]] std::optional<Source> parse_source(std::string_view s) noexcept {
  // A stored kill order comes back as `system`: only the kill-switch code can make the exempt value (D-23),
  // so a row can't grant the exemption. A restored order is never re-gated, and Postgres keeps the true
  // source.
  if (s == "kill_flatten") {
    return Source::kSystem;
  }
  for (const Source v : {Source::kManual, Source::kStrategy, Source::kAiDraft, Source::kSystem}) {
    if (name(v) == s) {
      return v;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> text(const std::optional<Uuid>& id) {
  return id.has_value() ? std::optional{id->to_string()} : std::nullopt;
}
[[nodiscard]] std::optional<std::string> text(const std::optional<Price>& p) {
  return p.has_value() ? std::optional{p->to_string()} : std::nullopt;
}
[[nodiscard]] std::optional<std::string> text(std::string_view s) {
  return s.empty() ? std::nullopt : std::optional{std::string(s)};
}
[[nodiscard]] std::optional<std::string> millipct_text(const std::optional<std::int32_t>& v) {
  return v.has_value() ? std::optional{std::format("{}.{:03}", *v / 1000, *v % 1000)}
                       : std::nullopt;  // never negative: Oms
}

constexpr Nanos kNanosPerMicro = 1000;

constexpr const char* kUniqueViolation = "23505";
constexpr const char* kStatementTimeout = "SET LOCAL statement_timeout = '5s'";

// Classes 22 and 23: this row will never go in, however often it's retried.
[[nodiscard]] bool is_data_error(std::string_view sqlstate) noexcept {
  return sqlstate.starts_with("22") || sqlstate.starts_with("23");
}

void log_retry(std::size_t events, std::string_view sqlstate) {
  if (quill::Logger* logger = log::engine(); logger != nullptr) {
    LOG_ERROR(logger, "order events: batch of {} not written, retrying (sqlstate {})", events, sqlstate);
  }
}

// The writer must not hang on a dead host: it holds up shutdown.
[[nodiscard]] std::string with_timeouts(const std::string& url) {
  if (url.find("connect_timeout=") != std::string::npos) {
    return url;
  }
  return url + (url.find('?') == std::string::npos ? "?" : "&") + "connect_timeout=5";
}

// A stale approval (owner, 2026-11-08) ends the order: audited and alerted in the status's transaction, so a
// dropped order is never silent. Only on the transition, so a later event can't repeat it; any other event is
// left alone.
void record_approval_stale(pqxx::work& tx, const std::string& id, const OrderEventRecord& e) {
  if (e.to != OrderStatus::kRiskRejected || e.from == e.to || e.reason_code.view() != kApprovalStale) {
    return;
  }
  tx.exec(
      R"(INSERT INTO audit_event (actor_kind, action, entity, entity_id, payload)
         SELECT 'engine', 'order.approval_stale', 'order', o.id::text,
                jsonb_build_object('reason_code', $2::text, 'seq', $3::int, 'approval_age_ns', $4::bigint,
                                   'client_order_id', o.client_order_id, 'account_id', o.account_id,
                                   'instrument_id', o.instrument_id, 'side', o.side, 'qty', o.qty,
                                   'is_paper', o.is_paper)
         FROM "order" o WHERE o.id = $1::uuid)",
      pqxx::params{id, std::string(e.reason_code.view()), e.seq, e.approval_age});
  tx.exec(
      R"(INSERT INTO alert_event (user_id, severity, title, body)
         SELECT u.id, 'warn', 'APPROVAL_STALE',
                jsonb_build_object('order_id', o.id, 'client_order_id', o.client_order_id,
                                   'account_id', o.account_id, 'approval_age_ns', $2::bigint,
                                   'is_paper', o.is_paper)
         FROM "order" o CROSS JOIN app_user u
         WHERE o.id = $1::uuid AND u.role = 'admin' AND u.is_active)",
      pqxx::params{id, e.approval_age});
}

// R-DUP couldn't check for duplicates because its table was full (D-24), so the order was refused. Audited
// and alerted in the status's transaction, as a stale approval is.
void record_dup_table_full(pqxx::work& tx, const std::string& id, const OrderEventRecord& e) {
  if (e.to != OrderStatus::kRiskRejected || e.from == e.to || e.reason_code.view() != kDupTableFull) {
    return;
  }
  tx.exec(
      R"(INSERT INTO audit_event (actor_kind, action, entity, entity_id, payload)
         SELECT 'engine', 'order.dup_table_full', 'order', o.id::text,
                jsonb_build_object('reason_code', $2::text, 'seq', $3::int,
                                   'client_order_id', o.client_order_id, 'account_id', o.account_id,
                                   'instrument_id', o.instrument_id, 'is_paper', o.is_paper)
         FROM "order" o WHERE o.id = $1::uuid)",
      pqxx::params{id, std::string(e.reason_code.view()), e.seq});
  tx.exec(
      R"(INSERT INTO alert_event (user_id, severity, title, body)
         SELECT u.id, 'critical', 'DUP_TABLE_FULL',
                jsonb_build_object('order_id', o.id, 'client_order_id', o.client_order_id,
                                   'account_id', o.account_id, 'is_paper', o.is_paper)
         FROM "order" o CROSS JOIN app_user u
         WHERE o.id = $1::uuid AND u.role = 'admin' AND u.is_active)",
      pqxx::params{id});
}

// Integers and exact decimal text, so building JSON by hand is safe. The broker order id is left out: it's
// on the `order` row, and it's the broker's text. So is the trade id, which Postgres encodes in write_event.
[[nodiscard]] std::string event_payload(const OrderEventRecord& e) {
  std::string payload = e.fill_qty > 0 ? std::format(R"({{"filled_qty":{},"fill_qty":{},"fill_price":"{}")",
                                                     e.filled_qty, e.fill_qty, e.fill_price.to_string())
                                       : std::format(R"({{"filled_qty":{})", e.filled_qty);
  if (e.modify.qty != 0) {
    payload += std::format(R"(,"modified_qty":{})", e.modify.qty);
  }
  if (e.modify.price.has_value()) {
    payload += std::format(R"(,"modified_price":"{}")", e.modify.price->to_string());
  }
  if (e.modify.trigger.has_value()) {
    payload += std::format(R"(,"modified_trigger":"{}")", e.modify.trigger->to_string());
  }
  if (e.gate.has_value()) {
    payload += std::format(R"(,"gate":{{"ruleset_version":{},"eval_ns":{},"ts_ns":{}}})",
                           e.gate->ruleset_version, e.gate->eval_ns, e.gate->ts);
  }
  payload += '}';
  return payload;
}

void write_event(pqxx::work& tx, const OrderEventRecord& e) {
  const std::string id = e.order_id.to_string();
  const Nanos micros = e.ts / kNanosPerMicro;
  if (!e.from.has_value()) {
    if (!e.details.has_value()) {  // the OMS always sets it: a data error, so this order alone is refused
      throw pqxx::sql_error("order event: first event without its order", "", "22023");
    }
    const OrderDetails& d = *e.details;
    tx.exec(
        R"(INSERT INTO "order" (id, parent_id, client_order_id, account_id, user_id, strategy_id, source,
             instrument_id, side, qty, order_type, product, validity, price, trigger_price, market_protection_pct,
             status, tag, is_paper, created_at, updated_at)
           VALUES ($1::uuid, $2::uuid, $3, $4::uuid, $5::uuid, $6::uuid, $7::order_source, $8, $9::order_side, $10,
             $11::order_type, $12::product_type, $13::validity_type, $14::numeric, $15::numeric, $16::numeric,
             $17::order_status, $18, $19,
             'epoch'::timestamptz + $20::bigint * interval '1 microsecond',
             'epoch'::timestamptz + $21::bigint * interval '1 microsecond')
           ON CONFLICT (id) DO NOTHING)",
        pqxx::params{id,
                     text(d.parent_id),
                     std::string(d.client_order_id.view()),
                     d.account_id.to_string(),
                     d.user_id.to_string(),
                     text(d.strategy_id),
                     std::string(name(d.source)),
                     d.instrument_id,
                     std::string(name(d.side)),
                     d.qty,
                     std::string(name(d.order_type)),
                     std::string(name(d.product)),
                     std::string(name(d.validity)),
                     text(d.price),
                     text(d.trigger_price),
                     millipct_text(d.market_protection_millipct),
                     std::string(to_string(e.to)),
                     std::string(d.tag.view()),
                     d.is_paper.paper(),
                     d.created_at / kNanosPerMicro,
                     micros});
  }
  const std::string payload = event_payload(e);
  // A retry after an unknown commit finds the row already there, so nothing is written or alerted twice.
  const pqxx::result inserted = tx.exec(
      R"(INSERT INTO order_event (order_id, seq, from_status, to_status, reason_code, payload, ts)
         VALUES ($1::uuid, $2, $3::order_status, $4::order_status, $5,
           CASE WHEN $8::text IS NULL THEN $6::jsonb
                ELSE $6::jsonb || jsonb_build_object('broker_trade_id', $8::text) END,
           'epoch'::timestamptz + $7::bigint * interval '1 microsecond')
         ON CONFLICT (order_id, seq) DO NOTHING RETURNING id)",
      pqxx::params{id, e.seq,
                   e.from.has_value() ? std::optional{std::string(to_string(*e.from))} : std::nullopt,
                   std::string(to_string(e.to)), text(e.reason_code.view()), payload, micros,
                   text(e.broker_trade_id.view())});
  if (inserted.empty()) {
    // That seq is taken. The same event again is a retry after an unknown commit, and already done. Anything
    // else must not touch the row: it would make a finished order look live, and disagree with its events.
    const bool retry = tx.exec(R"(SELECT to_status::text = $3 AND
                                    ts = 'epoch'::timestamptz + $4::bigint * interval '1 microsecond'
                                  FROM order_event WHERE order_id = $1::uuid AND seq = $2)",
                               pqxx::params{id, e.seq, std::string(to_string(e.to)), micros})
                           .one_row()[0]
                           .as<bool>();
    if (!retry) {
      throw pqxx::sql_error("order event: seq already taken", "", kUniqueViolation);  // quarantined, alone
    }
    return;
  }
  if (e.from.has_value()) {
    // Only the event that ended the order sets its code: a late fill's RECON_MISMATCH doesn't replace it.
    const bool rejected = (e.to == OrderStatus::kRiskRejected || e.to == OrderStatus::kBrokerRejected ||
                           e.to == OrderStatus::kExpired) &&
                          e.from != e.to;
    tx.exec(
        R"(UPDATE "order" SET status = $2::order_status, filled_qty = $3,
             broker_order_id = COALESCE($4, broker_order_id), reject_code = COALESCE($5, reject_code),
             updated_at = 'epoch'::timestamptz + $6::bigint * interval '1 microsecond',
             qty = COALESCE($7::int, qty), price = COALESCE($8::numeric, price),
             trigger_price = COALESCE($9::numeric, trigger_price)
           WHERE id = $1::uuid)",
        pqxx::params{id, std::string(to_string(e.to)), e.filled_qty, text(e.broker_order_id.view()),
                     rejected ? text(e.reason_code.view()) : std::nullopt, micros,
                     e.modify.row_qty != 0 ? std::optional{e.modify.row_qty} : std::nullopt,
                     e.modify.applied ? text(e.modify.price) : std::nullopt,
                     e.modify.applied ? text(e.modify.trigger) : std::nullopt});
  }
  if (e.reason_code.view() == kReconMismatch) {
    const std::string body =
        std::format(R"({{"order_id":"{}","seq":{},"status":"{}","filled_qty":{},"fill_qty":{}}})", id, e.seq,
                    to_string(e.to), e.filled_qty, e.fill_qty);
    tx.exec(
        R"(INSERT INTO alert_event (user_id, severity, title, body)
           SELECT id, 'critical', 'RECON_MISMATCH', $1::jsonb FROM app_user WHERE role = 'admin' AND is_active)",
        pqxx::params{body});
  }
  record_approval_stale(tx, id, e);
  record_dup_table_full(tx, id, e);
  // The restart rule (ADR-025) and the queue age limit (ADR-027): one transaction with the status, so an
  // expiry is never unaudited or unalerted. The alert title is fixed per reason
  // (expiry_alert_title): ORDER_EXPIRED_ON_RESTART, ORDER_EXPIRED_IN_QUEUE, else ORDER_EXPIRED. The
  // transition, not the status: a late fill on an expired order is also recorded with to = EXPIRED.
  if (e.to == OrderStatus::kExpired && e.from != e.to) {
    tx.exec(
        R"(INSERT INTO audit_event (actor_kind, action, entity, entity_id, payload)
           SELECT 'engine', 'order.expire', 'order', o.id::text,
                  jsonb_build_object('reason_code', $2::text, 'seq', $3::int,
                                     'client_order_id', o.client_order_id, 'account_id', o.account_id,
                                     'instrument_id', o.instrument_id, 'side', o.side, 'qty', o.qty,
                                     'is_paper', o.is_paper)
           FROM "order" o WHERE o.id = $1::uuid)",
        pqxx::params{id, std::string(e.reason_code.view()), e.seq});
    if (e.reason_code.view() == kCancelledInQueue) {
      return;  // someone asked for this: audited, no alert
    }
    tx.exec(
        R"(INSERT INTO alert_event (user_id, severity, title, body)
           SELECT u.id, 'critical', $2::text,
                  jsonb_build_object('order_id', o.id, 'client_order_id', o.client_order_id,
                                     'account_id', o.account_id, 'instrument_id', o.instrument_id,
                                     'side', o.side, 'qty', o.qty, 'is_paper', o.is_paper)
           FROM "order" o CROSS JOIN app_user u
           WHERE o.id = $1::uuid AND u.role = 'admin' AND u.is_active)",
        pqxx::params{id, std::string(expiry_alert_title(e.reason_code.view()))});
  }
}

template <class T>
[[nodiscard]] std::optional<T> optional_field(const pqxx::row_ref& row, int column) {
  return row[column].is_null() ? std::nullopt : std::optional<T>{row[column].as<T>()};
}

constexpr Error kUnreadable{ErrorCode::kOrderRecovery, "oms: an open order in Postgres can't be read"};

[[nodiscard]] std::optional<Price> parse_price(const pqxx::row_ref& row, int column, bool& ok) {
  const auto raw = optional_field<std::string>(row, column);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  const auto price = Price::parse(*raw);
  ok = ok && price.has_value();
  return price.has_value() ? std::optional{*price} : std::nullopt;
}

[[nodiscard]] std::optional<Uuid> parse_uuid(const pqxx::row_ref& row, int column, bool& ok) {
  const auto raw = optional_field<std::string>(row, column);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  const auto id = Uuid::parse(*raw);
  ok = ok && id.has_value();
  return id;
}

[[nodiscard]] std::optional<std::int32_t> parse_millipct(const pqxx::row_ref& row, int column, bool& ok) {
  const auto raw = optional_field<std::string>(row, column);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  const auto units = Price::parse(*raw);  // numeric text with at most 4 decimals
  if (!units.has_value()) {
    ok = false;
    return std::nullopt;
  }
  return static_cast<std::int32_t>(units->units() / 10);
}

}  // namespace

PgOrderEventWriter::PgOrderEventWriter(std::string url, IOutageSignal* outage_signal)
    : url_(std::move(url)),
      outage_signal_(outage_signal),
      ring_(std::make_unique<SpscRing<OrderEventRecord, kRingSize>>()),
      refused_(std::make_unique<SpscRing<Uuid, kRefusedRingSize>>()),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

PgOrderEventWriter::~PgOrderEventWriter() = default;

bool PgOrderEventWriter::try_push(const OrderEventRecord& event) noexcept {
  if (!ring_->try_push(event)) {
    return false;
  }
  ++pushed_;
  return true;
}

bool PgOrderEventWriter::flush(const std::vector<OrderEventRecord>& batch, const std::stop_token& stop) {
  try {
    pqxx::connection conn(with_timeouts(url_));
    pqxx::work tx(conn);
    tx.exec(kStatementTimeout);
    for (const OrderEventRecord& e : batch) {
      if (!quarantined_.contains(e.order_id.to_string())) {
        write_event(tx, e);
      }
    }
    tx.commit();
    return true;
  } catch (const pqxx::sql_error& e) {  // the code alone: messages can echo row values
    if (is_data_error(e.sqlstate())) {
      return flush_each(batch, stop);
    }
    log_retry(batch.size(), e.sqlstate());
  } catch (const std::exception&) {  // the message may echo connection details
    log_retry(batch.size(), "none: connection");
  }
  return false;
}

bool PgOrderEventWriter::flush_each(const std::vector<OrderEventRecord>& batch, const std::stop_token& stop) {
  const auto write_one = [this](const OrderEventRecord& e) {
    pqxx::connection conn(with_timeouts(url_));
    pqxx::work tx(conn);
    tx.exec(kStatementTimeout);
    write_event(tx, e);
    tx.commit();
  };
  try {
    for (const OrderEventRecord& e : batch) {
      if (quarantined_.contains(e.order_id.to_string())) {
        continue;
      }
      try {
        write_one(e);
      } catch (const pqxx::sql_error& first) {
        if (!is_data_error(first.sqlstate())) {
          throw;
        }
        const std::string sqlstate(first.sqlstate());
        // A live order keeps its history: the broker's id is the one part that can clash
        // (order_broker_id_uniq).
        OrderEventRecord without_id = e;
        without_id.broker_order_id = {};
        try {
          if (!e.from.has_value() || e.broker_order_id.empty()) {
            throw;
          }
          write_one(without_id);
        } catch (const pqxx::sql_error& second) {
          if (!is_data_error(second.sqlstate())) {
            throw;
          }
          quarantine(e, sqlstate, stop);
        }
      }
    }
    return true;
  } catch (const pqxx::sql_error& e) {
    log_retry(batch.size(), e.sqlstate());
  } catch (const std::exception&) {
    log_retry(batch.size(), "none: connection");
  }
  return false;
}

// One order fails, and only that one: it never goes out, admins hear about it, and the queue moves on.
void PgOrderEventWriter::quarantine(const OrderEventRecord& event, std::string_view sqlstate,
                                    const std::stop_token& stop) {
  const std::string id = event.order_id.to_string();
  quarantined_.insert(id);
  if (quill::Logger* logger = log::engine(); logger != nullptr) {
    LOG_ERROR(logger, "ORDER_EVENT_REFUSED order={} seq={} sqlstate={}", id, event.seq, sqlstate);
  }
  try {
    pqxx::connection conn(with_timeouts(url_));
    pqxx::work tx(conn);
    tx.exec(
        R"(INSERT INTO alert_event (user_id, severity, title, body)
           SELECT id, 'critical', 'ORDER_EVENT_REFUSED',
                  jsonb_build_object('order_id', $1::text, 'seq', $2::int, 'sqlstate', $3::text,
                                     'client_order_id', $4::text)
           FROM app_user WHERE role = 'admin' AND is_active)",
        pqxx::params{
            id, event.seq, std::string(sqlstate),
            event.details.has_value() ? std::string(event.details->client_order_id.view()) : std::string()});
    tx.commit();
  } catch (const std::exception&) {  // the log line above stands; the order is refused either way
    if (quill::Logger* logger = log::engine(); logger != nullptr) {
      LOG_ERROR(logger, "ORDER_EVENT_REFUSED alert not written for order={}", id);
    }
  }
  // Before durable() moves past this event, so the router can't take it for persisted. Shutting down, nobody
  // reads.
  while (!refused_->try_push(event.order_id) && !stop.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void PgOrderEventWriter::run(const std::stop_token& stop) {
  std::vector<OrderEventRecord> batch;
  batch.reserve(kBatchSize);
  Backoff backoff(BackoffPolicy{.initial = 100'000'000, .cap = 5 * kNanosPerSecond});
  constexpr auto kIdle = std::chrono::milliseconds(1);
  std::optional<std::chrono::steady_clock::time_point> give_up;
  for (;;) {
    OrderEventRecord event;
    while (batch.size() < kBatchSize && ring_->try_pop(event)) {
      batch.push_back(event);
    }
    if (batch.empty()) {
      if (stop.stop_requested()) {
        log_unreported_outage();
        return;
      }
      report_outage_if_due();  // a report that failed after recovery is retried here, not only on the next
                               // batch
      std::this_thread::sleep_for(kIdle);
      continue;
    }
    if (flush(batch, stop)) {
      written_.fetch_add(batch.size(), std::memory_order_acq_rel);
      healthy_.store(true, std::memory_order_release);
      backoff.reset();
      batch.clear();
      signal_outage_ended();
      report_outage_if_due();
      continue;
    }
    healthy_.store(false, std::memory_order_release);
    signal_outage_started();
    if (stop.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      if (!give_up.has_value()) {
        give_up = now + kShutdownRetry;
      } else if (now >= *give_up) {
        if (quill::Logger* logger = log::engine(); logger != nullptr) {
          LOG_ERROR(logger, "order events: {} LOST at shutdown; restart reconciles with the broker",
                    batch.size() + ring_->size_approx());
        }
        log_unreported_outage();
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(backoff.next()));
  }
}

void PgOrderEventWriter::note_outage_refusal(OutageRefusal kind) noexcept {
  const auto i = static_cast<std::size_t>(kind);
  if (i < outage_refusals_.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): checked
    outage_refusals_[i].fetch_add(1, std::memory_order_relaxed);
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): Quill logging
void PgOrderEventWriter::signal_outage_started() noexcept {
  if (!outage_since_.has_value()) {
    outage_since_ = std::chrono::steady_clock::now();
  }
  if (outage_signalled_) {
    return;
  }
  outage_signalled_ = true;
  if (quill::Logger* logger = log::engine(); logger != nullptr) {
    LOG_ERROR(logger, "ORDER_LOG_OUTAGE: the order log is unavailable; new orders are blocked");
  }
  if (outage_signal_ != nullptr) {
    outage_signal_->outage_started();
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): Quill logging
void PgOrderEventWriter::signal_outage_ended() noexcept {
  if (!outage_signalled_) {
    return;
  }
  outage_signalled_ = false;
  if (quill::Logger* logger = log::engine(); logger != nullptr) {
    LOG_ERROR(logger, "ORDER_LOG_OUTAGE: the order log is back; new orders are accepted again");
  }
  if (outage_signal_ != nullptr) {
    outage_signal_->outage_ended();
  }
}

void PgOrderEventWriter::report_outage_if_due() noexcept {
  if (!outage_since_.has_value() || !healthy_.load(std::memory_order_acquire)) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < next_report_try_) {
    return;
  }
  if (!report_outage()) {
    next_report_try_ = now + kOutageReportRetry;
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): Quill logging
void PgOrderEventWriter::log_unreported_outage() noexcept {
  if (!outage_since_.has_value()) {
    return;
  }
  if (quill::Logger* logger = log::engine(); logger != nullptr) {
    LOG_ERROR(logger,
              "ORDER_LOG_OUTAGE not written before stop: refused_new_orders={} refused_qty_raises={} "
              "refused_price_modifies={}",
              outage_refusals_[0].load(std::memory_order_relaxed),
              outage_refusals_[1].load(std::memory_order_relaxed),
              outage_refusals_[2].load(std::memory_order_relaxed));
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): every exception is caught
bool PgOrderEventWriter::report_outage() noexcept {
  std::array<std::uint64_t, kOutageRefusalKinds> counts{};
  for (std::size_t i = 0; i < counts.size(); ++i) {
    counts.at(i) = outage_refusals_.at(i).load(std::memory_order_relaxed);
  }
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - outage_since_.value_or(std::chrono::steady_clock::now()))
          .count();
  try {
    pqxx::connection conn(with_timeouts(url_));
    pqxx::work tx(conn);
    tx.exec(kStatementTimeout);
    tx.exec(
        R"(INSERT INTO alert_event (user_id, severity, title, body)
           SELECT id, 'critical', 'ORDER_LOG_OUTAGE',
                  jsonb_build_object('duration_ms', $1::bigint, 'refused_new_orders', $2::bigint,
                                     'refused_qty_raises', $3::bigint, 'refused_price_modifies', $4::bigint)
           FROM app_user WHERE role = 'admin' AND is_active)",
        pqxx::params{static_cast<std::int64_t>(ms), static_cast<std::int64_t>(counts[0]),
                     static_cast<std::int64_t>(counts[1]), static_cast<std::int64_t>(counts[2])});
    tx.commit();
  } catch (const std::exception&) {
    if (quill::Logger* logger = log::engine(); logger != nullptr) {
      LOG_ERROR(logger, "ORDER_LOG_OUTAGE alert not written yet; retrying");
    }
    return false;
  }
  for (std::size_t i = 0; i < counts.size(); ++i) {  // refusals since the read go in the next report
    outage_refusals_.at(i).fetch_sub(counts.at(i), std::memory_order_relaxed);
  }
  outage_since_.reset();
  return true;
}

std::expected<std::vector<RecoveredOrder>, Error> load_open_orders(const std::string& url) {
  std::vector<RecoveredOrder> out;
  try {
    pqxx::connection conn(with_timeouts(url));
    pqxx::read_transaction tx(conn);
    const pqxx::result rows = tx.exec(
        R"(SELECT o.id::text, o.parent_id::text, o.client_order_id, o.account_id::text, o.user_id::text,
             o.strategy_id::text, o.source::text, o.instrument_id, o.side::text, o.qty, o.filled_qty,
             o.order_type::text, o.product::text, o.validity::text, o.price::text, o.trigger_price::text,
             o.market_protection_pct::text, o.status::text, o.broker_order_id, o.tag, o.is_paper,
             (extract(epoch FROM o.created_at) * 1000000)::bigint, e.seq, e.to_status::text
           FROM "order" o
           LEFT JOIN LATERAL (SELECT seq, to_status FROM order_event WHERE order_id = o.id
                              ORDER BY seq DESC LIMIT 1) e ON true
           WHERE o.status NOT IN ('RISK_REJECTED', 'FILLED', 'CANCELLED', 'BROKER_REJECTED', 'EXPIRED')
           ORDER BY o.created_at, o.id)");
    out.reserve(rows.size());
    for (const pqxx::row_ref row : rows) {
      bool ok = true;
      // is_paper first: an OrderDetails can't exist without it.
      RecoveredOrder r{.details = OrderDetails{.is_paper = PaperFlag{row[20].as<bool>()}}};
      OrderDetails& d = r.details;
      const auto id = parse_uuid(row, 0, ok);
      const auto account = parse_uuid(row, 3, ok);
      const auto user = parse_uuid(row, 4, ok);
      d.parent_id = parse_uuid(row, 1, ok);
      d.strategy_id = parse_uuid(row, 5, ok);
      const auto source = parse_source(row[6].as<std::string>());
      const auto side = parse_side(row[8].as<std::string>());
      const auto type = parse_type(row[11].as<std::string>());
      const auto product = parse_product(row[12].as<std::string>());
      const auto validity = parse_validity(row[13].as<std::string>());
      d.price = parse_price(row, 14, ok);
      d.trigger_price = parse_price(row, 15, ok);
      d.market_protection_millipct = parse_millipct(row, 16, ok);
      const auto status = parse_status(row[17].as<std::string>());
      const auto last_status = optional_field<std::string>(row, 23);
      const auto seq = optional_field<std::int32_t>(row, 22);
      r.broker_order_id = optional_field<std::string>(row, 18).value_or("");
      if (!ok || !id || !account || !user || !source || !side || !type || !product || !validity || !status ||
          !seq || !last_status || *last_status != row[17].as<std::string>() ||
          !d.client_order_id.assign(row[2].as<std::string>()) || !d.tag.assign(row[19].as<std::string>())) {
        return std::unexpected(kUnreadable);
      }
      d.id = *id;
      d.account_id = *account;
      d.user_id = *user;
      d.source = *source;
      d.instrument_id = row[7].as<std::int64_t>();
      d.side = *side;
      d.qty = row[9].as<std::int32_t>();
      d.order_type = *type;
      d.product = *product;
      d.validity = *validity;
      d.created_at = row[21].as<std::int64_t>() * kNanosPerMicro;
      r.status = *status;
      r.seq = *seq;
      r.filled_qty = row[10].as<std::int32_t>();
      out.push_back(std::move(r));
    }
    // Trades already counted, so a replay after this restart is still a duplicate.
    std::unordered_map<std::string, std::size_t> by_id;
    by_id.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
      by_id.emplace(out[i].details.id.to_string(), i);
    }
    const pqxx::result trades = tx.exec(
        R"(SELECT e.order_id::text, e.payload->>'broker_trade_id'
           FROM order_event e JOIN "order" o ON o.id = e.order_id
           WHERE e.payload->>'broker_trade_id' IS NOT NULL
             AND o.status NOT IN ('RISK_REJECTED', 'FILLED', 'CANCELLED', 'BROKER_REJECTED', 'EXPIRED')
           ORDER BY e.id)");
    for (const pqxx::row_ref row : trades) {
      if (const auto it = by_id.find(row[0].as<std::string>()); it != by_id.end()) {
        out[it->second].trade_ids.push_back(row[1].as<std::string>());
      }
    }
  } catch (const std::exception&) {  // the message may echo connection details
    return std::unexpected(Error{ErrorCode::kOrderRecovery, "oms: cannot load open orders from Postgres"});
  }
  return out;
}

}  // namespace vyuha::oms
