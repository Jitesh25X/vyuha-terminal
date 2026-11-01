#include "vyuha/md/last_value.h"

namespace vyuha::md {

void LastValueBook::stamp(Entry& e, Tick& tick) noexcept {
  FeedTrade* f = nullptr;
  for (FeedTrade& s : e.feeds) {
    if (s.used && s.feed_id == tick.feed_id) {
      f = &s;
      break;
    }
    if (!s.used && f == nullptr) {
      f = &s;  // first free slot, claimed below unless the feed is found further on
    }
  }
  if (f == nullptr) {
    ++stats_.feeds_full;
    return;
  }
  if (!f->used) {
    f->used = true;
    f->feed_id = tick.feed_id;
  }
  if (tick.last_trade_ts != 0) {
    f->last_trade = tick.last_trade_ts;  // the broker said when
  } else if (tick.volume != 0 && f->volume != 0 && tick.volume > f->volume) {
    f->last_trade = tick.exchange_ts;
    ++stats_.inferred;
  }
  if (tick.volume != 0) {
    f->volume = tick.volume;  // also re-bases on a drop (new session, reconnect)
  }
  tick.last_trade_ts = f->last_trade;
}

bool LastValueBook::update(Tick& tick) noexcept {
  Entry* e = map_.find_or_insert(tick.instrument_id);
  if (e == nullptr) {
    return false;
  }
  stamp(*e, tick);
  e->tick = tick;
  e->set = true;
  return true;
}

const Tick* LastValueBook::find(InstrumentId id) const noexcept {
  const Entry* e = map_.find(id);
  return e != nullptr && e->set ? &e->tick : nullptr;
}

}  // namespace vyuha::md
