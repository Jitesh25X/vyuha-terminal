#include "vyuha/md/trade_clock.h"

namespace vyuha::md {

void TradeClock::stamp(Tick& tick) noexcept {
  if (tick.instrument_id >= (InstrumentId{1} << kFeedShift)) {
    return;  // can't pack the key; ids never get this large (bigserial)
  }
  const InstrumentId key = (static_cast<InstrumentId>(tick.feed_id) << kFeedShift) | tick.instrument_id;
  Entry* e = map_.find_or_insert(key);
  if (e == nullptr) {
    ++stats_.full;
    return;
  }
  if (tick.last_trade_ts != 0) {
    e->last_trade = tick.last_trade_ts;  // the broker said when
  } else if (tick.volume != 0 && e->seen && tick.volume > e->volume) {
    e->last_trade = tick.exchange_ts;
    ++stats_.inferred;
  }
  if (tick.volume != 0) {
    e->volume = tick.volume;  // also re-bases on a drop (new session, reconnect)
    e->seen = true;
  }
  tick.last_trade_ts = e->last_trade;
}

}  // namespace vyuha::md
