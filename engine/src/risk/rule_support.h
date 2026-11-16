#pragma once

#include <cstdint>
#include <optional>

#include "vyuha/risk/gate_context.h"
#include "vyuha/risk/risk_gate.h"

// What the rule files share. Internal: nothing outside src/risk includes it.
namespace vyuha::risk::detail {

constexpr Outcome reject(std::int64_t limit = 0, std::int64_t current = 0) noexcept {
  return {.verdict = Verdict::kReject, .limit = limit, .current = current};
}
constexpr Outcome warn(std::int64_t limit, std::int64_t current) noexcept {
  return {.verdict = Verdict::kWarn, .limit = limit, .current = current};
}
// A refusal that names what is missing: R-DATA-MISSING:<source> (D-27).
constexpr Outcome data_missing(DataSource source) noexcept {
  Outcome out = reject();
  out.reason = RuleReason::kDataMissing;
  out.source = source;
  return out;
}

// The LTP if there is one and it arrived within kLtpMaxAge and after the last pause.
[[nodiscard]] std::optional<Price> fresh_ltp(const GateContext& ctx, std::int64_t instrument_id,
                                             Nanos monotonic) noexcept;

}  // namespace vyuha::risk::detail
