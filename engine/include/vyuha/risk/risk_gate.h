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
#include "vyuha/risk/gate_context.h"
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
// 1–13 run for every order; those whose Applies column says "opening" or "live" decide that themselves. So
// does R-SHORT-OPT, the one rule of 14–22 that also sees reducing orders: selling the hedge of a short option
// reduces a position and leaves the short naked (D-39 (10)). It refuses only that.
[[nodiscard]] constexpr bool applies(RuleCode code, bool opening) noexcept {
  return opening || code < RuleCode::kOpenOrders || code == RuleCode::kShortOpt;
}

enum class Verdict : std::uint8_t {
  kPass,
  kWarn,    // a soft limit: the order passes, and the warning goes to the ticket and the log
  kReject,  // a hard limit: the chain stops here
};

// Why a rule refused, when the rule's own code isn't specific enough.
enum class RuleReason : std::uint8_t {
  kRule,
  kTriggerSide,
  kTriggerBand,
  kDataMissing,
  kNoCover,
  kUncover,
  kFreezeUnknown,   // R-ORDER-SHAPE: an F&O contract with no freeze quantity, and more than one lot (D-51)
  kFreezeExceeded,  // R-ORDER-SHAPE: an F&O order above the largest size its freeze quantity allows (D-54)
};

// What a rule needed and didn't have (D-27). The refusal's reason names it: R-DATA-MISSING:positions.
enum class DataSource : std::uint8_t { kNone, kPositions, kPnl, kMargin, kGreeks, kDiscipline, kMark };

// What a rule returns. limit and current are in the rule's own unit (price units, lots, a count); messages
// are rendered later, off the core thread.
struct Outcome {
  Verdict verdict{Verdict::kPass};
  std::int64_t limit{0};
  std::int64_t current{0};
  bool alert{false};  // the desk must hear about this rejection (R-DUP's table being full)
  RuleReason reason{RuleReason::kRule};
  DataSource source{DataSource::kNone};  // with kDataMissing
};

// An outcome with the code of the rule that gave it. The gate fills in the code, so a rule can't misreport.
struct RuleResult {
  RuleCode code{RuleCode::kKill};
  Verdict verdict{Verdict::kPass};
  std::int64_t limit{0};
  std::int64_t current{0};
  bool alert{false};
  RuleReason reason{RuleReason::kRule};
  DataSource source{DataSource::kNone};
};

inline constexpr std::string_view kTriggerSideReason = "R-TRIGGER-SIDE";
inline constexpr std::string_view kTriggerBandReason = "R-TRIGGER-BAND";
inline constexpr std::string_view kFreezeUnknownReason = "R-FREEZE-UNKNOWN";
inline constexpr std::string_view kFreezeExceededReason = "R-FREEZE-EXCEEDED";

// The order's reason code when it is refused. It's the rule's code, except for a rejection with a reason of
// its own: R-DUP's full table, which needs an alert, R-FAT-FINGER's stop triggers, and the others in
// RuleReason.
[[nodiscard]] std::string_view reason_text(const RuleResult& result) noexcept;

// Worked out once per evaluation. instrument and account are null when the context doesn't know them.
struct GateFacts {
  Nanos now{0};
  Nanos monotonic{0};  // for every duration (D-25, D-26): now is for session hours and absolute times only
  bool opening{true};
  const InstrumentRef* instrument{nullptr};
  const AccountState* account{nullptr};
  // RiskGate::check on a freeze-quantity parent, which is sliced next: R-FREEZE-EXCEEDED does not apply to it
  // (D-54). Never set by evaluate.
  bool sliced_parent{false};
};

// A rule: a plain function, called through a fixed table with no virtual dispatch. It must be noexcept, must
// not allocate, and must depend only on its arguments, so the gate is pure given the context.
using RuleFn = Outcome (*)(const GateContext&, const oms::OrderDetails&, const GateFacts&) noexcept;

// A stateful rule's second phase (R-DUP, R-OPS): records the approved order, such as taking an OPS token.
// Called only once the whole chain has passed, so a rejected order never uses up state (owner, 2026-11-08).
using OnApprovedFn = void (*)(GateContext&, const oms::OrderDetails&, const GateFacts&) noexcept;

struct Rule {
  RuleCode code{RuleCode::kKill};
  RuleFn eval{nullptr};
  OnApprovedFn on_approved{nullptr};  // only a stateful rule has one
};

struct GateDecision {
  std::optional<ApprovedOrder> approved;  // empty: rejected
  std::optional<RuleResult> rejection;    // the hard failure that stopped the chain
  std::array<RuleResult, kRuleCount> warnings{};
  std::size_t warning_count{0};
  GateStamp stamp;
  // Set when the order would take the account's position through flat (a flip, which is opening as a whole
  // order, D-38): the part of it that only closes, after the working orders on its side. Sent as an order of
  // its own that part is reducing, so the ticket can offer "close 10 + open 4" when a flip is refused. 0 for
  // any other order, and for a flip whose working orders already close the position.
  std::int64_t flip_close_qty{0};

  [[nodiscard]] std::span<const RuleResult> warning_list() const noexcept {
    return std::span(warnings).first(warning_count);
  }
};

// What a check-only evaluation found (D-49). It has no ApprovedOrder and no way to make one: a passed check
// sends nothing.
struct GateCheck {
  std::optional<RuleResult> rejection{};  // NOLINT(readability-redundant-member-init)
  std::array<RuleResult, kRuleCount> warnings{};
  std::size_t warning_count{0};
  GateStamp stamp{};               // NOLINT(readability-redundant-member-init)
  std::int64_t flip_close_qty{0};  // as in GateDecision

  [[nodiscard]] bool passed() const noexcept { return !rejection.has_value(); }
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

  // Runs every rule's check, and the first hard failure stops it. The checks are pure given ctx and order,
  // apart from the stamp's timing. Only when the whole chain has passed does each stateful rule's on_approved
  // run, in chain order; that is the only change the gate makes to ctx. A live order (is_paper false) is
  // refused with R-LIVE-DISABLED unless the table holds every rule of the chain: a gate missing rules must
  // not approve real orders.
  [[nodiscard]] GateDecision evaluate(GateContext& ctx, const oms::OrderDetails& order) const noexcept;

  // The check on a freeze-quantity parent's full size before any child is sent (D-49): every rule in chain
  // order except R-OPS, with the same applicability, incomplete-gate refusal and pause observation as
  // evaluate. It takes nothing (no on_approved runs: no OPS token, duplicate entry or reserved margin) and
  // approves nothing: each child still goes through evaluate. The only change to ctx is what seeing a pause
  // does (D-36). One rule's reason differs too: R-FREEZE-EXCEEDED is not applied (D-54).
  [[nodiscard]] GateCheck check(GateContext& ctx, const oms::OrderDetails& order) const noexcept;

  // Every rule of §3 is in the table.
  [[nodiscard]] bool complete() const noexcept { return count_ == kRuleCount; }

 private:
  RiskGate(std::span<const Rule> rules, const IClock& clock) noexcept : count_(rules.size()), clock_(&clock) {
    std::ranges::copy(rules, rules_.begin());
  }

  // Whether the order adds to the absolute position, and for a flip how much of it only closes. Positions
  // arrive with M2.6; until then every order counts as opening, so every rule runs (fail closed). Revisit at
  // M2.6, with M2.2b's modify rule.
  struct Direction {
    bool opening{true};
    std::int64_t flip_close_qty{0};
  };
  [[nodiscard]] static Direction direction(const GateContext& ctx, const oms::OrderDetails& order) noexcept;

  // Everything evaluate and check share, so the two cannot drift: the pause observation, the facts and the
  // chain. `parent_check` leaves out R-OPS and tells the rules (GateFacts::sliced_parent). Fills `out` (a
  // GateDecision or a GateCheck, in place: copying one costs tens of ns on the hot path) except
  // stamp.eval_ns, and returns the facts. Defined and used only in risk_gate.cc.
  template <class Out>
  [[nodiscard]] GateFacts run_chain(GateContext& ctx, const oms::OrderDetails& order, bool parent_check,
                                    Out& out) const noexcept;

  std::array<Rule, kRuleCount> rules_{};
  std::size_t count_{0};
  const IClock* clock_;
};

}  // namespace vyuha::risk
