#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "vyuha/core/error.h"
#include "vyuha/oms/order_router.h"
#include "vyuha/risk/gate_context.h"
#include "vyuha/risk/risk_gate.h"

namespace vyuha::risk {

// An LTP older than this can't be priced against (D-22). The same 3 s the UI uses to dim a price.
inline constexpr Nanos kLtpMaxAge = 3 * kNanosPerSecond;

// Rules 1 to 13 of RISK_ENGINE §3, in chain order. M2.4 adds 14 to 22; until then a gate built from this
// table is incomplete, so it refuses every live order.
[[nodiscard]] std::span<const Rule> pre_trade_rules() noexcept;

// Rules 14 to 17, 19 and 20, judged against a scope's own totals (M2.4a). 18, 21 and 22 come in M2.4b. Until
// all 22 are in one gate, it refuses every live order.
[[nodiscard]] std::span<const Rule> scope_rules() noexcept;

// RISK_GREEKS_MAX_AGE_MS: whole milliseconds, 1 to 300,000; empty is the default 15,000 (D-28).
[[nodiscard]] std::expected<Nanos, Error> parse_greeks_max_age_ms(std::string_view text) noexcept;

// Invariant 3: live trading needs the setting to be exactly "true" and the environment to be prod or staging.
[[nodiscard]] bool live_trading_allowed(std::string_view enabled, std::string_view env) noexcept;

// An account's orders-per-second cap, per exchange: the account's own and the desk and account limits,
// whichever is strictest, and never above 10. 0 if the account isn't known. Both R-OPS and the send queue use
// it.
[[nodiscard]] std::uint8_t effective_ops_cap(const GateContext& ctx, const oms::Uuid& account) noexcept;

// The send queue's view of the gate's data. Borrows ctx, which must outlive it.
class GateSendLimits final : public oms::ISendLimits {
 public:
  explicit GateSendLimits(const GateContext& ctx) noexcept : ctx_(&ctx) {}
  explicit GateSendLimits(const GateContext&& ctx) = delete;
  [[nodiscard]] std::uint8_t ops_cap(const oms::Uuid& account) const noexcept override {
    return effective_ops_cap(*ctx_, account);
  }
  [[nodiscard]] std::uint8_t ops_venue(std::int64_t instrument_id) const noexcept override;

 private:
  const GateContext* ctx_;
};

}  // namespace vyuha::risk
