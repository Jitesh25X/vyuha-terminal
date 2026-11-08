// Invariant 1 at compile time: the ways to reach a broker without the RiskGate must not compile.
// ctest compiles this file once with no case defined (the control, which must compile), then once per case
// with VYUHA_CASE_<name> (which must fail). A case's failure can then only be its own line.

#include <bit>
#include <string_view>
#include <utility>

#include "vyuha/oms/order_router.h"
#include "vyuha/risk/approved_order.h"
#include "vyuha/risk/risk_gate.h"

namespace probe {

using vyuha::oms::IOrderSender;
using vyuha::oms::Order;
using vyuha::oms::OrderDetails;
using vyuha::oms::OrderRouter;
using vyuha::risk::ApprovedOrder;
using vyuha::risk::GateStamp;
using vyuha::risk::RiskGate;
using vyuha::risk::Rule;

struct Bytes {
  OrderDetails d;
  GateStamp s;
  bool spent;
};

#if defined(VYUHA_CASE_DERIVE)
// A subclass that would expose the constructor.
class Forged : public ApprovedOrder {};
#endif

void probe(IOrderSender& sender, OrderRouter& router, const Order& order, const OrderDetails& details,
           const GateStamp& stamp, const ApprovedOrder& approved) {
  static_cast<void>(sender.send(approved, std::string_view{}));  // the one way in
  static_cast<void>(router);
  static_cast<void>(order);
  static_cast<void>(details);
  static_cast<void>(stamp);
#if defined(VYUHA_CASE_CONSTRUCT)
  const ApprovedOrder forged(details, stamp);
  static_cast<void>(forged);
#elif defined(VYUHA_CASE_BRACE)
  const ApprovedOrder forged{details, stamp};
  static_cast<void>(forged);
#elif defined(VYUHA_CASE_DEFAULT)
  const ApprovedOrder forged;
  static_cast<void>(forged);
#elif defined(VYUHA_CASE_COPY)
  const ApprovedOrder copy = approved;
  static_cast<void>(copy);
#elif defined(VYUHA_CASE_SEND_ORDER)
  static_cast<void>(sender.send(order, std::string_view{}));
#elif defined(VYUHA_CASE_SEND_DETAILS)
  static_cast<void>(sender.send(details, std::string_view{}));
#elif defined(VYUHA_CASE_SUBMIT_DETAILS)
  static_cast<void>(router.submit(0, details));
#elif defined(VYUHA_CASE_SUBMIT_WITHOUT_APPROVAL)
  static_cast<void>(router.submit(0));
#elif defined(VYUHA_CASE_BITCAST)
  const Bytes raw{details, stamp, false};
  const auto forged = std::bit_cast<ApprovedOrder>(raw);  // invariant-allow: compile-fail fixture
  static_cast<void>(forged);
#elif defined(VYUHA_CASE_PAPER_MISSING)
  const OrderDetails forgot{.qty = 1};  // no is_paper: it has no default (M2.2b)
  static_cast<void>(forgot);
#elif defined(VYUHA_CASE_PAPER_FROM_BOOL)
  const OrderDetails implicit{.is_paper = true};  // a bare bool isn't a PaperFlag
  static_cast<void>(implicit);
#elif defined(VYUHA_CASE_TEMP_CLOCK)
  static_cast<void>(RiskGate::create(std::span<const Rule>{}, vyuha::SimClock{}));
#endif
}

}  // namespace probe
