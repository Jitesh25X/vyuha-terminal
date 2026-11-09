#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "vyuha/oms/oms.h"

namespace vyuha::risk {

struct GateContext;

// The kill switch's block state, which R-KILL reads, and the only code that can mark an order kill-generated
// (D-23). There is one per GateContext, and nothing else can make or copy one: a mark from a switch of your
// own would otherwise pass. M2.7 adds actions, auto triggers and the gRPC API. Core thread only.
class KillSwitch final {
 public:
  ~KillSwitch() = default;
  enum class Scope : std::uint8_t { kDesk, kAccount, kTrader, kStrategy };
  static constexpr std::size_t kMaxPerScope = 64;

  // False when the scope's list is full. The whole desk is blocked instead, because a kill that can't be
  // recorded must still stop orders. id is ignored for kDesk.
  bool block(Scope scope, const oms::Uuid& id = {}) noexcept {
    if (scope == Scope::kDesk) {
      desk_ = true;
      return true;
    }
    List& list = list_for(scope);
    if (contains(list, id)) {
      return true;
    }
    if (list.count == kMaxPerScope) {
      desk_ = true;
      return false;
    }
    list.ids[list.count++] = id;  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index): checked above
    return true;
  }

  void release(Scope scope, const oms::Uuid& id = {}) noexcept {
    if (scope == Scope::kDesk) {
      desk_ = false;
      return;
    }
    List& list = list_for(scope);
    const auto used = std::span(list.ids).first(list.count);
    const auto it = std::ranges::find(used, id);
    if (it != used.end()) {
      *it = used.back();
      --list.count;
    }
  }

  [[nodiscard]] bool blocks(const oms::OrderDetails& order) const noexcept {
    return desk_ || contains(accounts_, order.account_id) || contains(traders_, order.user_id) ||
           (order.strategy_id.has_value() && contains(strategies_, *order.strategy_id));
  }

  // The order, marked kill-generated so that R-KILL lets it through. Empty unless a kill covers it, so the
  // mark can't be had while nothing is being killed.
  [[nodiscard]] std::optional<oms::OrderDetails> kill_generated(
      const oms::OrderDetails& order) const noexcept {
    if (!blocks(order)) {
      return std::nullopt;
    }
    oms::OrderDetails marked = order;
    marked.source = oms::Source::kKillFlatten;
    return marked;
  }

 private:
  friend struct GateContext;
  KillSwitch() noexcept = default;
  KillSwitch(const KillSwitch&) noexcept = default;
  KillSwitch(KillSwitch&&) noexcept = default;
  KillSwitch& operator=(const KillSwitch&) noexcept = default;
  KillSwitch& operator=(KillSwitch&&) noexcept = default;

  struct List {
    std::array<oms::Uuid, kMaxPerScope> ids{};
    std::size_t count{0};
  };
  [[nodiscard]] List& list_for(Scope scope) noexcept {
    if (scope == Scope::kAccount) {
      return accounts_;
    }
    return scope == Scope::kTrader ? traders_ : strategies_;
  }
  [[nodiscard]] static bool contains(const List& list, const oms::Uuid& id) noexcept {
    const auto used = std::span(list.ids).first(list.count);
    return std::ranges::find(used, id) != used.end();
  }

  bool desk_{false};
  List accounts_;
  List traders_;
  List strategies_;
};

}  // namespace vyuha::risk
