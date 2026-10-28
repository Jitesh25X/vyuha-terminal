#include "vyuha/md/bar_builder.h"

#include <algorithm>

namespace vyuha::md {

void BarBuilder::on_tick(const Tick& tick, IMdSink& sink) noexcept {
  State* s = map_.find_or_insert(tick.instrument_id);
  if (s == nullptr) {
    ++stats_.capacity_dropped;
    return;
  }
  const Nanos ts = tick.exchange_ts;
  const Nanos iv = interval_nanos(interval_);
  if (s->active && ts >= s->bar.ts + iv) {
    finalize(*s, sink);  // the forming bar ended before this tick, in session or not
  }
  if (!in_session(ts, hours_)) {
    ++stats_.out_of_session;
    s->cum_volume = std::max(s->cum_volume, tick.volume);
    return;
  }
  const Nanos open = session_open(ts, hours_);
  const Nanos bucket = open + (((ts - open) / iv) * iv);
  if (s->active && bucket < s->bar.ts) {
    ++stats_.late_ticks;
    return;
  }
  if (tick.ltp.units() > 0) {
    if (!s->active) {
      s->bar = Bar{.instrument_id = tick.instrument_id,
                   .interval = interval_,
                   .ts = bucket,
                   .open = tick.ltp,
                   .high = tick.ltp,
                   .low = tick.ltp,
                   .close = tick.ltp,
                   .volume = 0,
                   .oi = tick.oi,
                   .final = false};
      s->active = true;
    } else {
      s->bar.high = std::max(s->bar.high, tick.ltp);
      s->bar.low = std::min(s->bar.low, tick.ltp);
      s->bar.close = tick.ltp;
      s->bar.oi = tick.oi;
    }
  }
  s->cum_volume = std::max(s->cum_volume, tick.volume);  // a lower value is a broker glitch: ignore
}

void BarBuilder::on_time(Nanos now, IMdSink& sink) noexcept {
  const Nanos iv = interval_nanos(interval_);
  map_.for_each([&](InstrumentId /*id*/, State& s) {
    if (s.active && now >= s.bar.ts + iv + grace_) {
      finalize(s, sink);
    }
  });
}

void BarBuilder::reset_day() noexcept {
  map_.for_each([](InstrumentId /*id*/, State& s) { s = State{}; });
}

const Bar* BarBuilder::forming(InstrumentId id) const noexcept {
  const State* s = map_.find(id);
  return s != nullptr && s->active ? &s->bar : nullptr;
}

void BarBuilder::finalize(State& s, IMdSink& sink) noexcept {
  s.bar.volume = s.cum_volume >= s.prev_bar_volume ? s.cum_volume - s.prev_bar_volume : 0;
  s.bar.final = true;
  s.prev_bar_volume = s.cum_volume;
  s.active = false;
  ++stats_.bars;
  sink.on_bar(s.bar);
}

}  // namespace vyuha::md
