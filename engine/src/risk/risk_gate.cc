#include "vyuha/risk/risk_gate.h"

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
  return result.code == RuleCode::kDup && result.alert ? oms::kDupTableFull : code_text(result.code);
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

bool RiskGate::is_opening(const GateContext& /*ctx*/, const oms::OrderDetails& /*order*/) noexcept {
  return true;
}

GateDecision RiskGate::evaluate(GateContext& ctx, const oms::OrderDetails& order) const noexcept {
  GateDecision out;
  const Nanos start = clock_->now();
  out.stamp.ruleset_version = ctx.limits.version;
  out.stamp.ts = start;
  const GateFacts facts{
      .now = start,
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
    const RuleResult result{
        .code = rule.code, .verdict = o.verdict, .limit = o.limit, .current = o.current, .alert = o.alert};
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
  out.stamp.eval_ns = clock_->now() - start;
  if (!out.rejection.has_value()) {
    out.approved = ApprovedOrder(order, out.stamp);
  }
  return out;
}

}  // namespace vyuha::risk
