#include "vyuha/oms/pg_order_store.h"

#include <chrono>
#include <exception>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include <quill/LogMacros.h>
#include <pqxx/pqxx>

#include "vyuha/util/backoff.h"
#include "vyuha/util/log.h"

namespace vyuha::oms {

namespace {

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
  switch (v) {
    case Source::kManual:
      return "manual";
    case Source::kStrategy:
      return "strategy";
    case Source::kAiDraft:
      return "ai_draft";
    case Source::kSystem:
      return "system";
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

// The writer must not hang on a dead host: it holds up shutdown.
[[nodiscard]] std::string with_timeouts(const std::string& url) {
  if (url.find("connect_timeout=") != std::string::npos) {
    return url;
  }
  return url + (url.find('?') == std::string::npos ? "?" : "&") + "connect_timeout=5";
}

void write_event(pqxx::work& tx, const OrderEventRecord& e) {
  const std::string id = e.order_id.to_string();
  const Nanos micros = e.ts / kNanosPerMicro;
  if (!e.from.has_value()) {
    const OrderDetails& d = e.details;
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
                     d.is_paper,
                     d.created_at / kNanosPerMicro,
                     micros});
  } else {
    const bool rejected = e.to == OrderStatus::kRiskRejected || e.to == OrderStatus::kBrokerRejected;
    tx.exec(
        R"(UPDATE "order" SET status = $2::order_status, filled_qty = $3,
             broker_order_id = COALESCE($4, broker_order_id), reject_code = COALESCE($5, reject_code),
             updated_at = 'epoch'::timestamptz + $6::bigint * interval '1 microsecond'
           WHERE id = $1::uuid)",
        pqxx::params{id, std::string(to_string(e.to)), e.filled_qty, text(e.broker_order_id.view()),
                     rejected ? text(e.reason_code.view()) : std::nullopt, micros});
  }
  // Integers and exact decimal text, so building JSON by hand is safe. The broker order id is left out: it's
  // on the `order` row, and it's the broker's text.
  const std::string payload = e.fill_qty > 0
                                  ? std::format(R"({{"filled_qty":{},"fill_qty":{},"fill_price":"{}"}})",
                                                e.filled_qty, e.fill_qty, e.fill_price.to_string())
                                  : std::format(R"({{"filled_qty":{}}})", e.filled_qty);
  // A retry after an unknown commit finds the row already there, so nothing is written or alerted twice.
  const pqxx::result inserted = tx.exec(
      R"(INSERT INTO order_event (order_id, seq, from_status, to_status, reason_code, payload, ts)
         VALUES ($1::uuid, $2, $3::order_status, $4::order_status, $5, $6::jsonb,
           'epoch'::timestamptz + $7::bigint * interval '1 microsecond')
         ON CONFLICT (order_id, seq) DO NOTHING RETURNING id)",
      pqxx::params{id, e.seq,
                   e.from.has_value() ? std::optional{std::string(to_string(*e.from))} : std::nullopt,
                   std::string(to_string(e.to)), text(e.reason_code.view()), payload, micros});
  if (e.reason_code.view() == kReconMismatch && !inserted.empty()) {
    const std::string body =
        std::format(R"({{"order_id":"{}","seq":{},"status":"{}","filled_qty":{},"fill_qty":{}}})", id, e.seq,
                    to_string(e.to), e.filled_qty, e.fill_qty);
    tx.exec(
        R"(INSERT INTO alert_event (user_id, severity, title, body)
           SELECT id, 'critical', 'RECON_MISMATCH', $1::jsonb FROM app_user WHERE role = 'admin' AND is_active)",
        pqxx::params{body});
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

PgOrderEventWriter::PgOrderEventWriter(std::string url)
    : url_(std::move(url)),
      ring_(std::make_unique<SpscRing<OrderEventRecord, kRingSize>>()),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

PgOrderEventWriter::~PgOrderEventWriter() = default;

bool PgOrderEventWriter::try_push(const OrderEventRecord& event) noexcept {
  if (!ring_->try_push(event)) {
    return false;
  }
  ++pushed_;
  return true;
}

bool PgOrderEventWriter::flush(const std::vector<OrderEventRecord>& batch) {
  const auto log_failure = [&](std::string_view sqlstate) {
    if (quill::Logger* logger = log::engine(); logger != nullptr) {
      LOG_ERROR(logger, "order events: batch of {} not written, retrying (sqlstate {})", batch.size(),
                sqlstate);
    }
  };
  try {
    pqxx::connection conn(with_timeouts(url_));
    pqxx::work tx(conn);
    tx.exec("SET LOCAL statement_timeout = '5s'");
    for (const OrderEventRecord& e : batch) {
      write_event(tx, e);
    }
    tx.commit();
    return true;
  } catch (const pqxx::sql_error& e) {  // the code alone: messages can echo row values
    log_failure(
        e.sqlstate());  // a data error (22, 23) won't clear by itself, and nothing is sent until it does
  } catch (const std::exception&) {  // the message may echo connection details
    log_failure("none: connection");
  }
  return false;
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
        return;
      }
      std::this_thread::sleep_for(kIdle);
      continue;
    }
    if (flush(batch)) {
      written_.fetch_add(batch.size(), std::memory_order_acq_rel);
      healthy_.store(true, std::memory_order_release);
      backoff.reset();
      batch.clear();
      continue;
    }
    healthy_.store(false, std::memory_order_release);
    if (stop.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      if (!give_up.has_value()) {
        give_up = now + kShutdownRetry;
      } else if (now >= *give_up) {
        if (quill::Logger* logger = log::engine(); logger != nullptr) {
          LOG_ERROR(logger, "order events: {} LOST at shutdown; restart reconciles with the broker",
                    batch.size() + ring_->size_approx());
        }
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(backoff.next()));
  }
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
           WHERE o.status NOT IN ('RISK_REJECTED', 'FILLED', 'CANCELLED', 'BROKER_REJECTED')
           ORDER BY o.created_at, o.id)");
    out.reserve(rows.size());
    for (const pqxx::row_ref row : rows) {
      bool ok = true;
      RecoveredOrder r;
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
      d.is_paper = row[20].as<bool>();
      d.created_at = row[21].as<std::int64_t>() * kNanosPerMicro;
      r.status = *status;
      r.seq = *seq;
      r.filled_qty = row[10].as<std::int32_t>();
      out.push_back(std::move(r));
    }
  } catch (const std::exception&) {  // the message may echo connection details
    return std::unexpected(Error{ErrorCode::kOrderRecovery, "oms: cannot load open orders from Postgres"});
  }
  return out;
}

}  // namespace vyuha::oms
