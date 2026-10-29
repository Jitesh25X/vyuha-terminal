#include "vyuha/bus/md_encode.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vyuha::bus {

namespace {

constexpr std::uint32_t kVarint = 0;
constexpr std::uint32_t kFixed64 = 1;
constexpr std::uint32_t kLen = 2;

// Bounds-checked writer; once anything doesn't fit, `ok` stays false and nothing more is written.
class Writer {
 public:
  explicit Writer(std::span<std::uint8_t> out) noexcept : out_(out) {}

  [[nodiscard]] std::size_t size() const noexcept { return ok_ ? pos_ : 0; }
  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  void varint(std::uint64_t v) noexcept {
    while (v >= 0x80U) {
      byte(static_cast<std::uint8_t>(v | 0x80U));
      v >>= 7U;
    }
    byte(static_cast<std::uint8_t>(v));
  }
  void key(std::uint32_t field, std::uint32_t wire) noexcept {
    varint((static_cast<std::uint64_t>(field) << 3U) | wire);
  }

  // proto3: default (zero) scalars are not written.
  void u64(std::uint32_t field, std::uint64_t v) noexcept {
    if (v != 0) {
      key(field, kVarint);
      varint(v);
    }
  }
  void i64(std::uint32_t field, std::int64_t v) noexcept {
    u64(field, static_cast<std::uint64_t>(v));
  }  // two's complement
  void boolean(std::uint32_t field, bool v) noexcept { u64(field, v ? 1 : 0); }
  void f64(std::uint32_t field, double v) noexcept {
    if (v == 0.0) {
      return;
    }
    key(field, kFixed64);
    const auto bits = std::bit_cast<std::uint64_t>(v);
    for (unsigned i = 0; i < 8; ++i) {
      byte(static_cast<std::uint8_t>(bits >> (8U * i)));
    }
  }
  void text(std::uint32_t field, const char* s, std::size_t n) noexcept {
    key(field, kLen);
    varint(n);
    for (std::size_t i = 0; i < n; ++i) {
      byte(static_cast<std::uint8_t>(s[i]));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }

  // Length-delimited submessage: reserve a 2-byte length (enough for < 16 KiB), patch it after.
  [[nodiscard]] std::size_t begin_message(std::uint32_t field) noexcept {
    key(field, kLen);
    const std::size_t at = pos_;
    byte(0);
    byte(0);
    return at;
  }
  void end_message(std::size_t at) noexcept {
    if (!ok_) {
      return;
    }
    const std::size_t len = pos_ - at - 2;
    if (len >= (1U << 14U)) {
      ok_ = false;
      return;
    }
    out_[at] = static_cast<std::uint8_t>((len & 0x7FU) | 0x80U);  // 2-byte varint (non-minimal is valid)
    out_[at + 1] = static_cast<std::uint8_t>(len >> 7U);
  }

 private:
  void byte(std::uint8_t b) noexcept {
    if (!ok_ || pos_ >= out_.size()) {
      ok_ = false;
      return;
    }
    out_[pos_++] = b;
  }

  std::span<std::uint8_t> out_;
  std::size_t pos_{0};
  bool ok_{true};
};

std::uint64_t source_enum(md::Source s) noexcept {
  switch (s) {
    case md::Source::kZerodha:
      return 1;
    case md::Source::kUpstox:
      return 2;
    case md::Source::kSim:
      return 3;
    default:
      return 0;
  }
}

void write_depth(Writer& w, std::uint32_t field, const md::DepthLevel& d) noexcept {
  if (d.price.units() == 0 && d.qty == 0 && d.orders == 0) {
    return;  // empty level: not on the wire
  }
  const std::size_t at = w.begin_message(field);
  w.i64(1, d.price.units());
  w.u64(2, d.qty);
  w.u64(3, d.orders);
  w.end_message(at);
}

void write_tick(Writer& w, const md::Tick& t) noexcept {
  w.u64(1, t.instrument_id);
  w.i64(2, t.exchange_ts);
  w.i64(3, t.recv_ts);
  w.u64(4, source_enum(t.source));
  w.i64(5, t.ltp.units());
  w.u64(6, t.ltq);
  w.u64(7, t.volume);
  w.u64(8, t.oi);
  w.i64(9, t.open.units());
  w.i64(10, t.high.units());
  w.i64(11, t.low.units());
  w.i64(12, t.prev_close.units());
  w.i64(13, t.avg_price.units());
  w.i64(14, t.bid.units());
  w.i64(15, t.ask.units());
  w.u64(16, t.bid_qty);
  w.u64(17, t.ask_qty);
  for (const auto& d : t.bids) {
    write_depth(w, 18, d);
  }
  for (const auto& d : t.asks) {
    write_depth(w, 19, d);
  }
  w.u64(25, t.seq);
  w.u64(26, t.feed_id);
}

}  // namespace

std::size_t encode_tick(const md::Tick& t, std::span<std::uint8_t> out) noexcept {
  Writer w(out);
  write_tick(w, t);
  return w.size();
}

std::size_t encode_bar(const md::Bar& b, std::span<std::uint8_t> out) noexcept {
  Writer w(out);
  w.u64(1, b.instrument_id);
  if (b.interval == md::BarInterval::k1s) {
    w.text(2, "1s", 2);
  } else {
    w.text(2, "1m", 2);
  }
  w.i64(3, b.ts);
  w.i64(4, b.open.units());
  w.i64(5, b.high.units());
  w.i64(6, b.low.units());
  w.i64(7, b.close.units());
  w.u64(8, b.volume);
  w.u64(9, b.oi);
  w.boolean(10, b.final);
  return w.size();
}

std::size_t encode_snapshot(const md::Tick& t, bool stale, std::span<std::uint8_t> out) noexcept {
  Writer w(out);
  const std::size_t at = w.begin_message(1);
  write_tick(w, t);
  w.end_message(at);
  // Display-only analytics value (not money): percentage change vs previous close.
  if (t.prev_close.units() > 0 && t.ltp.units() > 0) {
    w.f64(2, 100.0 * static_cast<double>(t.ltp.units() - t.prev_close.units()) /
                 static_cast<double>(t.prev_close.units()));
  }
  w.boolean(3, stale);
  return w.size();
}

}  // namespace vyuha::bus
