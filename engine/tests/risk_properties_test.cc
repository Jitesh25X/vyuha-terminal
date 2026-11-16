// Properties of the whole 22-rule chain (RISK_ENGINE §3, M2.4b), over random books and random orders, on
// shares, futures, calls and puts, paper and live: (a) a position-reducing order is never refused by rules 14
// to 22 except by R-SHORT-OPT-UNCOVER, and an opening one always is, (b) with every limit at its most
// permissive and every source loaded, every well-formed order passes, (c) the gate is deterministic given its
// context, and a refused order leaves no state behind, (d) an order that reduces a long option or a future is
// refused as uncovering exactly when it leaves more short call units without cover, judged by a matching
// worked out in the test.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>  // rapidcheck's headers use std::exception_ptr without including it
#include <limits>
#include <optional>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>

#include "desk_fixture.h"
#include "scope_desk.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

struct Kind {
  std::int64_t id;
  std::int64_t lot;
  const char* name;
};
// A share, a future, a call and a put: the lots differ, so the arithmetic in lots is tested too.
constexpr std::array<Kind, 4> kKinds{
    {{kEquity, 1, "equity"}, {kFuture, 1, "future"}, {kOption, kLot, "call"}, {kPutOption, kLot, "put"}}};

// `net` lots held on the instrument by the account and trader, with `working` lots more on the order's side.
void hold(ScopeDesk& d, const Kind& kind, int net, bool buy, int working) {
  d.mark(kind.id, rupees(100));
  if (net != 0) {
    d.fill(net > 0 ? Side::kBuy : Side::kSell, std::abs(net) * kind.lot, rupees(100), kind.id);
  }
  if (working > 0) {
    EXPECT_TRUE(d.sb.add_working(d.who(), kind.id, buy ? Side::kBuy : Side::kSell, working * kind.lot));
  }
}

// Every limit that rules 14 to 22 read at its tightest: any opening order breaks one of them. With `keep_14`
// the open-orders limit stays loose, so rules 15 to 22 have to do the refusing.
void tighten(ScopeDesk& d, bool keep_14) {
  for (const LimitKey key :
       {LimitKey::kMaxOpenOrders, LimitKey::kMaxPositionLots, LimitKey::kMaxPositionNotional,
        LimitKey::kMaxGrossExposure, LimitKey::kMaxNetDeltaInr, LimitKey::kMaxNetVegaInr,
        LimitKey::kMaxDailyLoss, LimitKey::kMaxDrawdownFromPeak, LimitKey::kMaxShortOptionLots}) {
    if (keep_14 && key == LimitKey::kMaxOpenOrders) {
      continue;
    }
    d.clear_limits(key);
    d.limit(key, 0);
  }
  d.clear_limits(LimitKey::kMaxOrderQtyLots);
  d.limit(LimitKey::kMaxOrderQtyLots, 1'000 * kFromScale);
  d.refresh_margin(rupees(1).units());  // next to nothing free
  for (const auto mode : {oms::kPaperOrder, oms::kLiveOrder}) {
    EXPECT_TRUE(d.discipline_table->lock(kUser, mode, md::ist_day_start(d.clock.now())));
  }
}

// Independent of the gate: does the order leave the account's position no larger, and on the same side?
bool reduces(int net, bool buy, int working, int qty) {
  const int added = working + qty;
  const int projected = net + (buy ? added : -added);
  const bool flipped = (net > 0 && projected < 0) || (net < 0 && projected > 0);
  return !flipped && std::abs(projected) <= std::abs(net);
}

OrderDetails order_for(ScopeDesk& d, const Kind& kind, bool live, bool buy, int lots) {
  OrderDetails o = live ? d.live_order() : d.order();
  o.instrument_id = kind.id;
  o.side = buy ? Side::kBuy : Side::kSell;
  o.qty = static_cast<std::int32_t>(lots * kind.lot);
  return o;
}

// Options are where rules 18 and 21 and the lot arithmetic bite, so they are drawn twice as often.
const Kind& pick_kind() {
  const int i = *rc::gen::weightedElement<int>({{1, 0}, {1, 1}, {2, 2}, {2, 3}});
  return kKinds.at(static_cast<std::size_t>(i));
}

TEST(RiskProperties, AReducingOrderIsNeverRefusedByRules14To22AndAnOpeningOneAlwaysIs) {
  std::int64_t uncovered_refusals = 0;
  const bool ok = rc::check([&uncovered_refusals] {
    const Kind& kind = pick_kind();
    const int net = *rc::gen::inRange<int>(-5, 6);
    const int working = *rc::gen::inRange<int>(0, 6);
    const bool buy = *rc::gen::arbitrary<bool>();
    const int lots = *rc::gen::inRange<int>(1, 6);
    const bool live = *rc::gen::arbitrary<bool>();
    const bool keep_14 = *rc::gen::arbitrary<bool>();
    ScopeDesk d;
    tighten(d, keep_14);
    d.fresh_greeks_all();
    hold(d, kind, net, buy, working);
    RC_ASSERT(d.sb.healthy());
    const GateDecision out = d.run_all(order_for(d, kind, live, buy, lots));
    const bool is_reducing = reduces(net, buy, working, lots);
    RC_CLASSIFY(is_reducing, "reducing");
    RC_CLASSIFY(!is_reducing, "opening");
    RC_CLASSIFY(!is_reducing && keep_14, "opening, rule 14 loose");
    RC_CLASSIFY(live, "live");
    RC_CLASSIFY(kind.id == kEquity, "equity");
    RC_CLASSIFY(kind.id == kFuture, "future");
    RC_CLASSIFY(kind.id == kOption, "call");
    RC_CLASSIFY(kind.id == kPutOption, "put");
    if (is_reducing) {
      // Rule 18 alone sees a reducing order, and refuses only one that takes cover from a short (D-39 (10)).
      // One instrument is held here, so there is no short for it to leave bare.
      const bool uncovering = out.rejection.has_value() && out.rejection->reason == RuleReason::kUncover;
      RC_CLASSIFY(uncovering, "reducing, refused as uncovering");
      uncovered_refusals += uncovering ? 1 : 0;
      RC_ASSERT(!out.rejection.has_value() || uncovering);
      RC_ASSERT(!uncovering);
      RC_ASSERT(out.approved.has_value());
    } else {
      RC_ASSERT(out.rejection.has_value());
      RC_ASSERT(out.rejection->code >= RuleCode::kOpenOrders);  // and never by rules 1 to 13
      if (keep_14) {
        RC_ASSERT(out.rejection->code > RuleCode::kOpenOrders);  // so one of 15 to 22 did it
      }
    }
  });
  EXPECT_TRUE(ok);
  EXPECT_EQ(uncovered_refusals, 0);
}

// Opens every limit as far as it goes, and loads and freshens every source.
void open_up(ScopeDesk& d) {
  for (const LimitKey key : {LimitKey::kMaxOrderValue, LimitKey::kMaxOrderQtyLots, LimitKey::kFatFingerPct,
                             LimitKey::kFatFingerPctOpt, LimitKey::kMaxOpenOrders, LimitKey::kMaxPositionLots,
                             LimitKey::kMaxPositionNotional, LimitKey::kMaxGrossExposure,
                             LimitKey::kMaxNetDeltaInr, LimitKey::kMaxNetVegaInr, LimitKey::kMaxDailyLoss,
                             LimitKey::kMaxDrawdownFromPeak, LimitKey::kMaxShortOptionLots}) {
    d.clear_limits(key);
    d.limit(key, kMax);
  }
  for (const auto& [key, value] : {std::pair{LimitKey::kDupWindowMs, std::int64_t{0}},
                                   std::pair{LimitKey::kMarginBufferPct, std::int64_t{0}},
                                   std::pair{LimitKey::kNakedShortOptions, std::int64_t{1} * kFromScale},
                                   std::pair{LimitKey::kOpsCap, std::int64_t{10} * kFromScale},
                                   std::pair{LimitKey::kAllowMarketOrders, std::int64_t{1} * kFromScale}}) {
    d.clear_limits(key);
    d.limit(key, value);
  }
  d.refresh_margin(kMax / 4);
  d.fresh_greeks_all();
}

TEST(RiskProperties, WithEveryLimitAtItsMostPermissiveEveryWellFormedOrderPasses) {
  const bool ok = rc::check([] {
    const Kind& kind = pick_kind();
    const int net = *rc::gen::inRange<int>(-5, 6);
    const bool buy = *rc::gen::arbitrary<bool>();
    const int lots = *rc::gen::inRange<int>(1, 6);
    const bool market = *rc::gen::arbitrary<bool>();
    const bool live = *rc::gen::arbitrary<bool>();
    ScopeDesk d;
    open_up(d);
    hold(d, kKinds.front(), net, buy, 0);  // some shares held, whatever the order is for
    OrderDetails o = order_for(d, kind, live, buy, lots);
    if (market) {
      o.order_type = OrderType::kMarket;
      o.price.reset();
    }
    const GateDecision out = d.run_all(o);
    RC_CLASSIFY(kind.id == kEquity, "equity");
    RC_CLASSIFY(kind.id == kFuture, "future");
    RC_CLASSIFY(kind.id == kOption, "call");
    RC_CLASSIFY(kind.id == kPutOption, "put");
    RC_CLASSIFY(live, "live");
    RC_CLASSIFY(market, "market");
    RC_CLASSIFY(live && kind.lot > 1 && !buy, "live short option");
    RC_ASSERT(out.approved.has_value());
  });
  EXPECT_TRUE(ok);
}

TEST(RiskProperties, TheGateIsDeterministicAndARefusedOrderLeavesNoStateBehind) {
  const bool ok = rc::check([] {
    const Kind& kind = pick_kind();
    const int net = *rc::gen::inRange<int>(-5, 6);
    const int working = *rc::gen::inRange<int>(0, 6);
    const bool buy = *rc::gen::arbitrary<bool>();
    const int lots = *rc::gen::inRange<int>(1, 6);
    const bool live = *rc::gen::arbitrary<bool>();
    const bool tight = *rc::gen::arbitrary<bool>();
    const bool keep_14 = *rc::gen::arbitrary<bool>();
    const bool margin_tight = *rc::gen::arbitrary<bool>();
    const auto scenario = [&](ScopeDesk& d) {
      if (tight) {
        tighten(d, keep_14);
      }
      if (margin_tight) {
        d.refresh_margin(rupees(500).units());
      }
      d.fresh_greeks_all();
      hold(d, kind, net, buy, working);
      return order_for(d, kind, live, buy, lots);
    };
    ScopeDesk first;
    ScopeDesk second;
    const OrderDetails a = scenario(first);
    const OrderDetails b = scenario(second);
    const GateDecision x = first.run_all(a);
    const GateDecision y = second.run_all(b);
    RC_CLASSIFY(x.approved.has_value(), "approved");
    RC_CLASSIFY(x.rejection.has_value(), "refused");
    RC_CLASSIFY(x.rejection.has_value() && x.rejection->code >= RuleCode::kOpenOrders, "refused by 14 to 22");
    RC_CLASSIFY(x.rejection.has_value() && x.rejection->code == RuleCode::kShortOpt, "refused by 18");
    RC_CLASSIFY(x.rejection.has_value() && x.rejection->code == RuleCode::kMargin, "refused by 21");
    RC_ASSERT(x.approved.has_value() == y.approved.has_value());
    RC_ASSERT(x.rejection.has_value() == y.rejection.has_value());
    RC_ASSERT(x.warning_count == y.warning_count);
    RC_ASSERT(x.stamp == y.stamp);
    if (x.rejection.has_value()) {
      RC_ASSERT(x.rejection->code == y.rejection->code);
      RC_ASSERT(reason_text(*x.rejection) == reason_text(*y.rejection));
      RC_ASSERT(x.rejection->limit == y.rejection->limit);
      RC_ASSERT(x.rejection->current == y.rejection->current);
      // A refusal uses no OPS token, no duplicate entry and no reserved margin, so the same order again gives
      // the same answer.
      const ScopeDesk pristine;
      RC_ASSERT(first.ctx.dup.count == 0);
      RC_ASSERT(first.margin_cache->available(kAccount)->reserved == 0);
      RC_ASSERT(first.ctx.accounts.find(kAccount)->ops == pristine.ctx.accounts.find(kAccount)->ops);
      const GateDecision again = first.run_all(a);
      RC_ASSERT(again.rejection.has_value());
      RC_ASSERT(again.rejection->code == x.rejection->code);
      RC_ASSERT(again.rejection->limit == x.rejection->limit);
    }
  });
  EXPECT_TRUE(ok);
}

// ---- (d) uncovering, against an independent matching ----

constexpr std::int64_t kFutAtNear = 400;  // expires with kOption and kCall75
constexpr std::int64_t kPut75 = 401;      // a put like kPutOption with a lot of 75

// One instrument of the account's book on the underlying, as the test sees it.
struct Holding {
  std::int64_t id{0};
  bool option{false};
  std::int64_t lot{1};
  Nanos expiry{0};
  std::int64_t net{0};  // units
  std::int64_t working_buy{0};
  std::int64_t working_sell{0};
};

// Short units of one option kind (calls, or puts when `put`) that no long of that kind of their own expiry,
// and no future of the covering side (long for calls, short for puts) expiring at or after theirs, can be
// matched with: the shorts less the largest matching, found by augmenting paths on a flow network (source,
// one node per option holding with shorts, one per supply, sink). It shares nothing with the book's formula.
// What a future gives as cover: a long covers calls, a short covers puts, less the working orders that would
// close it.
std::int64_t future_gives(const Holding& h, bool put) {
  return std::max<std::int64_t>(put ? -h.net - h.working_buy : h.net - h.working_sell, 0);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a small flow network, read top to bottom
std::int64_t uncovered_units(const std::vector<Holding>& book, bool put) {
  const std::size_t n = book.size();
  // Nodes: 0 source, 1..n shorts of holding i, n+1..2n supply from holding i, 2n+1 sink.
  const std::size_t nodes = (2 * n) + 2;
  const std::size_t sink = nodes - 1;
  std::vector<std::vector<std::int64_t>> cap(nodes, std::vector<std::int64_t>(nodes, 0));
  constexpr std::int64_t kInf = 1'000'000'000;
  std::int64_t shorts = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Holding& h = book[i];
    if (h.option) {
      const std::int64_t s =
          std::max<std::int64_t>(-h.net, 0) + h.working_sell;  // a working sell needs cover
      shorts += s;
      cap[0][1 + i] = s;
      cap[n + 1 + i][sink] = std::max<std::int64_t>(h.net, 0);  // a long covers, even one that is being sold
    } else {
      cap[n + 1 + i][sink] = future_gives(h, put);
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      if (!book[i].option) {
        continue;
      }
      const bool own_expiry = book[j].option && book[j].expiry == book[i].expiry;
      const bool later_future = !book[j].option && book[j].expiry >= book[i].expiry;
      if (own_expiry || later_future) {
        cap[1 + i][n + 1 + j] = kInf;
      }
    }
  }
  std::int64_t flow = 0;
  for (;;) {
    std::vector<std::int64_t> from(nodes, -1);
    from[0] = 0;
    std::vector<std::size_t> queue{0};
    for (std::size_t head = 0; head < queue.size() && from[sink] < 0; ++head) {
      for (std::size_t v = 0; v < nodes; ++v) {
        if (from[v] < 0 && cap[queue[head]][v] > 0) {
          from[v] = static_cast<std::int64_t>(queue[head]);
          queue.push_back(v);
        }
      }
    }
    if (from[sink] < 0) {
      break;
    }
    std::int64_t push = kInf;
    for (std::size_t v = sink; v != 0; v = static_cast<std::size_t>(from[v])) {
      push = std::min(push, cap[static_cast<std::size_t>(from[v])][v]);
    }
    for (std::size_t v = sink; v != 0; v = static_cast<std::size_t>(from[v])) {
      cap[static_cast<std::size_t>(from[v])][v] -= push;
      cap[v][static_cast<std::size_t>(from[v])] += push;
    }
    flow += push;
  }
  return shorts - flow;
}

// NOLINTBEGIN(readability-function-cognitive-complexity): one property, and its generator is the point
void uncover_property(bool put) {
  std::int64_t refused_count = 0;
  std::int64_t passed_count = 0;
  const bool ok = rc::check([&] {
    ScopeDesk d;
    InstrumentRef at_near = *d.instruments.find(kHedgeFut25);
    at_near.expires_at = d.instruments.find(kOption)->expires_at;
    RC_ASSERT(d.instruments.put(kFutAtNear, at_near));  // banded like the fixture's other derivatives
    d.tick(kFutAtNear, rupees(100));
    InstrumentRef put75 = *d.instruments.find(kPutOption);
    put75.lot_size = 75;
    RC_ASSERT(d.instruments.put(kPut75, put75));
    d.tick(kPut75, rupees(100));
    // Options of one kind at two expiries and two lot sizes, futures at three expiries and two lot sizes.
    std::vector<Holding> book;
    const std::array<std::int64_t, 3> options = put ? std::array<std::int64_t, 3>{kPutOption, kPut75, kFarPut}
                                                    : std::array<std::int64_t, 3>{kOption, kCall75, kCallFar};
    for (const std::int64_t id : {options[0], options[1], options[2], kFutAtNear, kHedgeFut, kHedgeFutNear}) {
      const InstrumentRef& ref = *d.instruments.find(id);
      book.push_back(
          {.id = id, .option = ref.segment == Segment::kOpt, .lot = ref.lot_size, .expiry = ref.expires_at});
    }
    for (Holding& h : book) {
      const int lots = *rc::gen::inRange<int>(-3, 4);
      const int work_lots = *rc::gen::inRange<int>(0, 3);
      const bool work_buy = *rc::gen::arbitrary<bool>();
      h.net = lots * h.lot;
      (work_buy ? h.working_buy : h.working_sell) = work_lots * h.lot;
      d.mark(h.id, rupees(100));
      if (h.net != 0) {
        d.fill(h.net > 0 ? Side::kBuy : Side::kSell, std::abs(h.net), rupees(100), h.id);
      }
      if (h.working_buy > 0) {
        RC_ASSERT(d.sb.add_working(d.who(), h.id, Side::kBuy, h.working_buy));
      }
      if (h.working_sell > 0) {
        RC_ASSERT(d.sb.add_working(d.who(), h.id, Side::kSell, h.working_sell));
      }
    }
    // An order that reduces a long option of the kind, or any order on a future (a flip included).
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < book.size(); ++i) {
      const Holding& h = book[i];
      if (!h.option || h.net - h.working_sell >= h.lot) {
        candidates.push_back(i);
      }
    }
    RC_PRE(!candidates.empty());
    const std::size_t pick = candidates.at(*rc::gen::inRange<std::size_t>(0, candidates.size()));
    const Holding& target = book[pick];
    // A long option is sold in whole lots up to what its working sells leave, so the order reduces and
    // doesn't flip; a future is traded either way in any amount.
    const auto max_lots =
        static_cast<int>(target.option ? (target.net - target.working_sell) / target.lot : 3);
    const int order_lots = *rc::gen::inRange<int>(1, max_lots + 1);
    const bool buy = target.option ? false : *rc::gen::arbitrary<bool>();
    const std::int64_t qty = order_lots * target.lot;
    const std::int64_t before = uncovered_units(book, put);
    std::vector<Holding> after_book = book;
    Holding& after = after_book[pick];
    if (target.option) {
      after.net -= qty;  // a long option sold
    } else if (buy == put) {
      // The order is on the closing side of the cover the future gives (a sell of a long for calls, a buy of
      // a short for puts) and takes away up to what it gives, which already leaves out the working orders.
      const std::int64_t taken = std::min(future_gives(after, put), qty);
      after.net += put ? taken : -taken;
    }
    const std::int64_t later = uncovered_units(after_book, put);
    OrderDetails o = d.order();
    o.instrument_id = target.id;
    o.side = buy ? Side::kBuy : Side::kSell;
    o.qty = static_cast<std::int32_t>(qty);
    const GateDecision out = d.run_all(o);
    const bool uncovering = out.rejection.has_value() && out.rejection->reason == RuleReason::kUncover;
    RC_CLASSIFY(later > before, "leaves more bare");
    RC_CLASSIFY(later <= before && before > 0, "no worse, though a short was bare already");
    RC_CLASSIFY(!target.option, "a future");
    RC_ASSERT(uncovering == (later > before));
    if (uncovering) {
      RC_ASSERT(out.rejection->code == RuleCode::kShortOpt);
      RC_ASSERT(reason_text(*out.rejection) == "R-SHORT-OPT-UNCOVER");
      // A refusal leaves no state behind: the same order again is refused the same way.
      RC_ASSERT(d.ctx.dup.count == 0);
      const GateDecision again = d.run_all(o);
      RC_ASSERT(again.rejection.has_value() && again.rejection->reason == RuleReason::kUncover);
      ++refused_count;
    } else {
      RC_ASSERT(out.approved.has_value());
      ++passed_count;
    }
  });
  EXPECT_TRUE(ok);
  EXPECT_GT(refused_count, 0);
  EXPECT_GT(passed_count, 0);
}

TEST(RiskProperties,
     AnOrderThatReducesALongCallOrAFutureIsRefusedAsUncoveringExactlyWhenItLeavesMoreCallsBare) {
  uncover_property(false);
}

TEST(RiskProperties,
     AnOrderThatReducesALongPutOrAFutureIsRefusedAsUncoveringExactlyWhenItLeavesMorePutsBare) {
  uncover_property(true);
}

// NOLINTEND(readability-function-cognitive-complexity)
}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
