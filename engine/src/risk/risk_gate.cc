#include "vyuha/risk/risk_gate.h"
#include "vyuha/risk/scope_book.h"
#include "vyuha/risk/wide.h"

#include <array>
#include <utility>

namespace vyuha::risk {

namespace {

constexpr std::array<std::string_view, kRuleCount> kCodeText{
    "R-KILL",        "R-LIVE-DISABLED", "R-ENTITLE",          "R-SESSION",
    "R-MKT-HOURS",   "R-INSTR-BAN",     "R-INSTR-RESTRICTED", "R-INSTR-EXPIRED",
    "R-ORDER-SHAPE", "R-FAT-FINGER",    "R-ORDER-VALUE",      "R-DUP",
    "R-OPS",         "R-OPEN-ORDERS",   "R-POSITION",         "R-EXPOSURE",
    "R-GREEKS",      "R-SHORT-OPT",     "R-LOSS-DAILY",       "R-DRAWDOWN",
    "R-MARGIN",      "R-DISCIPLINE",
};

constexpr bool in_chain(RuleCode code) noexcept {
  const auto n = std::to_underlying(code);
  return n >= 1 && n <= kRuleCount;
}

}  // namespace

std::string_view code_text(RuleCode code) noexcept {
  return in_chain(code) ? kCodeText.at(std::to_underlying(code) - 1U) : std::string_view("R-UNKNOWN");
}

std::string_view reason_text(const RuleResult& result) noexcept {
  if (result.code == RuleCode::kDup && result.alert) {
    return oms::kDupTableFull;
  }
  switch (result.reason) {
    case RuleReason::kTriggerSide:
      return kTriggerSideReason;
    case RuleReason::kTriggerBand:
      return kTriggerBandReason;
    case RuleReason::kDataMissing:
      switch (result.source) {
        case DataSource::kPositions:
          return "R-DATA-MISSING:positions";
        case DataSource::kPnl:
          return "R-DATA-MISSING:pnl";
        case DataSource::kMargin:
          return "R-DATA-MISSING:margin";
        case DataSource::kGreeks:
          return "R-DATA-MISSING:greeks";
        case DataSource::kNone:
          return "R-DATA-MISSING";
      }
      return "R-DATA-MISSING";
    case RuleReason::kRule:
      break;
  }
  return code_text(result.code);
}

std::expected<RiskGate, Error> RiskGate::create(std::span<const Rule> rules, const IClock& clock) noexcept {
  std::uint8_t last = 0;
  for (const Rule& rule : rules) {
    if (!in_chain(rule.code) || std::to_underlying(rule.code) <= last) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, "risk gate: rules out of §3 order"});
    }
    if (rule.eval == nullptr) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, "risk gate: a rule has no function"});
    }
    last = std::to_underlying(rule.code);
  }
  return RiskGate(rules, clock);
}

// D-29: per account and instrument, an order is opening if abs(net + same-side working + this order) is more
// than abs(net). A flip is opening as a whole order. With no positions loaded nothing is known, so every
// order counts as opening (D-27) and the rules that need positions refuse it.
bool RiskGate::is_opening(const GateContext& ctx, const oms::OrderDetails& order) noexcept {
  if (ctx.scopes == nullptr || !ctx.scopes->positions_loaded()) {
    return true;
  }
  const Position* p = ctx.scopes->position(LimitScope::kAccount, order.account_id, order.instrument_id);
  const Wide net = p != nullptr ? Wide{p->net_qty} : Wide{0};
  const bool buy = order.side == oms::Side::kBuy;
  const Wide working = p == nullptr ? Wide{0} : Wide{buy ? p->working_buy : p->working_sell};
  const Wide added = working + Wide{order.qty};
  const Wide projected = net + (buy ? added : -added);
  // Larger, or on the other side of flat: a flip is opening as a whole order, however small the new side.
  return wide_abs(projected) > wide_abs(net) || (net > 0 && projected < 0) || (net < 0 && projected > 0);
}

GateDecision RiskGate::evaluate(GateContext& ctx, const oms::OrderDetails& order) const noexcept {
  GateDecision out;
  const Nanos start = clock_->now();
  const Nanos started = clock_->monotonic();
  if (ctx.pause.observe(start, started)) {
    ctx.stale_before = started;  // D-36: the host was paused, so cached data that looks young is minutes old
  }
  out.stamp.ruleset_version = ctx.limits.version;
  out.stamp.ts = start;
  out.stamp.monotonic = started;
  const GateFacts facts{
      .now = start,
      .monotonic = started,
      .opening = is_opening(ctx, order),
      .instrument = ctx.instruments != nullptr ? ctx.instruments->find(order.instrument_id) : nullptr,
      .account = std::as_const(ctx).accounts.find(order.account_id)};

  if (order.is_paper.live() && !complete()) {
    out.rejection = RuleResult{.code = RuleCode::kLiveDisabled, .verdict = Verdict::kReject};
  }
  for (const Rule& rule : std::span(rules_).first(count_)) {
    if (out.rejection.has_value()) {
      break;
    }
    if (!applies(rule.code, facts.opening)) {
      continue;
    }
    const Outcome o = rule.eval(std::as_const(ctx), order, facts);  // pure: the check can't change ctx
    const RuleResult result{.code = rule.code,
                            .verdict = o.verdict,
                            .limit = o.limit,
                            .current = o.current,
                            .alert = o.alert,
                            .reason = o.reason,
                            .source = o.source};
    if (o.verdict == Verdict::kWarn) {
      out.warnings.at(out.warning_count++) = result;  // at most one per rule, so it always fits
    } else if (o.verdict != Verdict::kPass) {
      out.rejection = result;  // kReject, or a value no rule should return: fail closed
      out.rejection->verdict = Verdict::kReject;
    }
  }
  if (!out.rejection.has_value()) {
    for (const Rule& rule : std::span(rules_).first(count_)) {
      if (rule.on_approved != nullptr && applies(rule.code, facts.opening)) {
        rule.on_approved(ctx, order, facts);
      }
    }
  }
  out.stamp.eval_ns = clock_->monotonic() - started;
  if (!out.rejection.has_value()) {
    out.approved = ApprovedOrder(order, out.stamp);
  }
  return out;
}

}  // namespace vyuha::risk
