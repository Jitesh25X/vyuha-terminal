#include "vyuha/core/core_loop.h"

#include <thread>

#include "quill/LogMacros.h"
#include "vyuha/util/log.h"

namespace vyuha {

CoreLoop::CoreLoop(const CoreConfig& cfg, const IClock& clock, md::IMdSink& sink)
    : cfg_(cfg),
      clock_(&clock),
      sink_(&sink),
      book_(cfg.max_instruments),
      trades_(cfg.max_instruments * 2),  // (instrument, feed): an instrument may stream on two feeds
      bars_1s_(md::BarInterval::k1s, cfg.max_instruments, cfg.hours, cfg.bar_grace),
      bars_1m_(md::BarInterval::k1m, cfg.max_instruments, cfg.hours, cfg.bar_grace) {}

bool CoreLoop::add_input(TickRing& ring) noexcept {
  if (n_inputs_ == kMaxInputs) {
    return false;
  }
  inputs_[n_inputs_++] = &ring;  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index): checked above
  return true;
}

std::size_t CoreLoop::poll() noexcept {
  std::size_t done = 0;
  std::size_t idle = 0;
  md::Tick tick;
  // Round-robin one tick per ring so a busy feed can't starve the others.
  while (done < cfg_.poll_budget && idle < n_inputs_) {
    TickRing* ring =
        inputs_[next_input_];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index): < n_inputs_
    next_input_ = next_input_ + 1 == n_inputs_ ? 0 : next_input_ + 1;
    if (ring->try_pop(tick)) {
      on_tick(tick);
      ++done;
      idle = 0;
    } else {
      ++idle;
    }
  }
  const Nanos now = clock_->now();
  if (now >= next_timer_) {
    on_timer(now);
    next_timer_ = now + cfg_.timer_period;
  }
  return done;
}

void CoreLoop::run(const std::stop_token& stop) {
  quill::Logger* log = log::engine();
  if (log != nullptr) {
    LOG_INFO(log, "core loop started: inputs={} max_instruments={}", n_inputs_, cfg_.max_instruments);
  }
  while (!stop.stop_requested()) {
    if (poll() == 0) {
      std::this_thread::yield();
    }
  }
  if (log != nullptr) {
    LOG_INFO(log, "core loop stopped: ticks={} invalid={} book_full={} bars_1m={}", stats_.ticks,
             stats_.invalid, stats_.book_full, bars_1m_.stats().bars);
  }
}

void CoreLoop::on_tick(md::Tick& tick) noexcept {
  if (tick.instrument_id == 0 || tick.ltp.units() < 0 || tick.bid.units() < 0 || tick.ask.units() < 0) {
    ++stats_.invalid;
    return;
  }
  tick.seq = ++seq_;
  ++stats_.ticks;
  trades_.stamp(tick);
  if (!book_.update(tick)) {
    ++stats_.book_full;
  }
  bars_1s_.on_tick(tick, *sink_);
  bars_1m_.on_tick(tick, *sink_);
  sink_->on_tick(tick);
}

void CoreLoop::on_timer(Nanos now) noexcept {
  bars_1s_.on_time(now, *sink_);
  bars_1m_.on_time(now, *sink_);
  const Nanos day = md::ist_day_start(now);
  if (day != day_) {
    if (day_ != 0) {
      // on_time above already closed yesterday's last bars (15:30 + grace is long past).
      bars_1s_.reset_day();
      bars_1m_.reset_day();
      ++stats_.day_rollovers;
    }
    day_ = day;
  }
}

}  // namespace vyuha
