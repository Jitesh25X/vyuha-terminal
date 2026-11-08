#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "vyuha/core/clock.h"
#include "vyuha/core/error.h"
#include "vyuha/oms/oms.h"
#include "vyuha/risk/approved_order.h"
#include "vyuha/risk/gate_stamp.h"

namespace vyuha::risk {

// The rule chain, in evaluation order (RISK_ENGINE §3). The numbers match the table's #, and they are
// stable: they are also the order a rule table must follow.
enum class RuleCode : std::uint8_t {
  kKill = 1,
  kLiveDisabled,
  kEntitle,
  kSession,
  kMktHours,
  kInstrBan,
  kInstrRestricted,
  kInstrExpired,
  kOrderShape,
  kFatFinger,
  kOrderValue,
  kDup,
  kOps,
  kOpenOrders,
  kPosition,
  kExposure,
  kGreeks,
  kShortOpt,
  kLossDaily,
  kDrawdown,
  kMargin,
  kDiscipline,
};
inline constexpr std::size_t kRuleCount = 22;

// "R-KILL" and so on; "R-UNKNOWN" for a value outside the chain.
[[nodiscard]] std::string_view code_text(RuleCode code) noexcept;

// Rules 14–22 check only opening orders, so limits never stop a trader getting flat (RISK_ENGINE §3). Rules
// 1–13 run for every order; those whose Applies column says "opening" or "live" decide that themselves.
[[nodiscard]] constexpr bool applies(RuleCode code, bool opening) noexcept {
  return opening || code < RuleCode::kOpenOrders;
}

enum class Verdict : std::uint8_t {
  kPass,
  kWarn,    // a soft limit: the order passes, and the warning goes to the ticket and the log
  kReject,  // a hard limit: the chain stops here
};

// What a rule returns. limit and current are in the rule's own unit (price units, lots, a count); messages
// are rendered later, off the core thread.
struct Outcome {
  Verdict verdict{Verdict::kPass};
  std::int64_t limit{0};
  std::int64_t current{0};
};

// An outcome with the code of the rule that gave it. The gate fills in the code, so a rule can't misreport.
struct RuleResult {
  RuleCode code{RuleCode::kKill};
  Verdict verdict{Verdict::kPass};
  std::int64_t limit{0};
  std::int64_t current{0};
};

// Limit values arrive with the rules that read them (M2.3, M2.4). Versioned; hot-reloaded by ReloadLimits.
struct LimitSet {
  std::uint64_t version{0};
};

// Engine-local state the rules read. The core thread owns it and updates it incrementally on fills, marks
// and limit reloads; a rule never iterates all positions. Rules add the fields they need (M2.3, M2.4).
struct GateContext {
  LimitSet limits;
};

// Worked out once per evaluation.
struct GateFacts {
  Nanos now{0};
  bool opening{true};
};

// A rule: a plain function, called through a fixed table with no virtual dispatch. It must be noexcept, must
// not allocate, and must depend only on its arguments, so the gate is pure given the context.
using RuleFn = Outcome (*)(const GateContext&, const oms::OrderDetails&, const GateFacts&) noexcept;

struct Rule {
  RuleCode code{RuleCode::kKill};
  RuleFn eval{nullptr};
};

struct GateDecision {
  std::optional<ApprovedOrder> approved;  // empty: rejected
  std::optional<RuleResult> rejection;    // the hard failure that stopped the chain
  std::array<RuleResult, kRuleCount> warnings{};
  std::size_t warning_count{0};
  GateStamp stamp;

  [[nodiscard]] std::span<const RuleResult> warning_list() const noexcept {
    return std::span(warnings).first(warning_count);
  }
};

// The pre-trade gate. Runs on the core thread: no allocation, no locks, no exceptions.
class RiskGate {
 public:
  // `rules` must follow the §3 order, with each code at most once and every function set. The gate keeps its
  // own copy of the table; the clock is borrowed and must outlive it.
  [[nodiscard]] static std::expected<RiskGate, Error> create(std::span<const Rule> rules,
                                                             const IClock& clock) noexcept;
  static std::expected<RiskGate, Error> create(std::span<const Rule> rules, const IClock&& clock) = delete;

  // Runs the chain; the first hard failure stops it. Pure given ctx and order, apart from the stamp's
  // timing. A live order (is_paper false) is refused with R-LIVE-DISABLED unless the table holds every rule
  // of the chain: a gate missing rules must not approve real orders.
  [[nodiscard]] GateDecision evaluate(const GateContext& ctx, const oms::OrderDetails& order) const noexcept;

  // Every rule of §3 is in the table.
  [[nodiscard]] bool complete() const noexcept { return count_ == kRuleCount; }

 private:
  RiskGate(std::span<const Rule> rules, const IClock& clock) noexcept : count_(rules.size()), clock_(&clock) {
    std::ranges::copy(rules, rules_.begin());
  }

  // Whether the order adds to the absolute position. Positions arrive with M2.6; until then every order
  // counts as opening, so every rule runs (fail closed). Revisit at M2.6, with M2.2b's modify rule.
  [[nodiscard]] static bool is_opening(const GateContext& ctx, const oms::OrderDetails& order) noexcept;

  std::array<Rule, kRuleCount> rules_{};
  std::size_t count_{0};
  const IClock* clock_;
};

}  // namespace vyuha::risk
