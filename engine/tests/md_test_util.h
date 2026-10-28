#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/core/price.h"
#include "vyuha/md/session.h"
#include "vyuha/md/tick.h"

namespace vyuha::testing {

// UTC ns for an IST wall time (IST = UTC+05:30, no DST).
inline Nanos ist(int y, unsigned mo, unsigned d, int h, int mi, int s = 0, std::int64_t ms = 0) {
  using namespace std::chrono;
  const sys_days day{year{y} / month{mo} / ::std::chrono::day{d}};
  const auto local = day.time_since_epoch() + hours{h} + minutes{mi} + seconds{s} + milliseconds{ms};
  return duration_cast<nanoseconds>(local).count() - md::kIstOffset;
}

inline md::Tick trade(md::InstrumentId id, Nanos ts, std::int64_t rupees, std::uint64_t cum_volume,
                      std::uint64_t oi = 0) {
  md::Tick t;
  t.instrument_id = id;
  t.exchange_ts = ts;
  t.recv_ts = ts;
  t.ltp = Price::from_units(rupees * Price::kUnitsPerRupee);
  t.volume = cum_volume;
  t.oi = oi;
  return t;
}

struct RecordingSink final : md::IMdSink {
  std::vector<md::Tick> ticks;
  std::vector<md::Bar> bars;
  void on_tick(const md::Tick& t) noexcept override { ticks.push_back(t); }
  void on_bar(const md::Bar& b) noexcept override { bars.push_back(b); }
};

// Counts nothing and stores nothing: for allocation-free measurements.
struct NullSink final : md::IMdSink {
  std::uint64_t ticks{0};
  std::uint64_t bars{0};
  void on_tick(const md::Tick& /*t*/) noexcept override { ++ticks; }
  void on_bar(const md::Bar& /*b*/) noexcept override { ++bars; }
};

}  // namespace vyuha::testing
