#include "vyuha/broker/upstox/feed_parser.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vyuha::broker::upstox {

namespace {

// Protobuf wire types.
constexpr std::uint32_t kVarint = 0;
constexpr std::uint32_t kFixed64 = 1;
constexpr std::uint32_t kLen = 2;
constexpr std::uint32_t kFixed32 = 5;

// Bounds-checked cursor over one message. Any read past the end clears `ok`; callers stop then.
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> b) noexcept : buf_(b) {}

  [[nodiscard]] bool more() const noexcept { return ok_ && pos_ < buf_.size(); }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  // Reads a field key; returns false at end or on error.
  bool next(std::uint32_t& field, std::uint32_t& wire) noexcept {
    if (!more()) {
      return false;
    }
    const std::uint64_t key = varint();
    field = static_cast<std::uint32_t>(key >> 3U);
    wire = static_cast<std::uint32_t>(key & 7U);
    if (field == 0) {
      ok_ = false;
    }
    return ok_;
  }

  std::uint64_t varint() noexcept {
    std::uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (pos_ >= buf_.size()) {
        ok_ = false;
        return 0;
      }
      const std::uint8_t b = buf_[pos_++];
      v |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
      if ((b & 0x80U) == 0) {
        return v;
      }
    }
    ok_ = false;  // more than 10 bytes
    return 0;
  }

  double fixed64_double() noexcept {
    if (pos_ > buf_.size() || buf_.size() - pos_ < 8) {
      ok_ = false;
      return 0;
    }
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) {  // little-endian on the wire
      bits |= static_cast<std::uint64_t>(buf_[pos_ + i]) << (8U * i);
    }
    pos_ += 8;
    return std::bit_cast<double>(bits);
  }

  std::span<const std::uint8_t> bytes() noexcept {
    const std::uint64_t n = varint();
    if (!ok_ || n > buf_.size() - pos_) {
      ok_ = false;
      return {};
    }
    const auto out = buf_.subspan(pos_, static_cast<std::size_t>(n));
    pos_ += static_cast<std::size_t>(n);
    return out;
  }

  void skip(std::uint32_t wire) noexcept {
    switch (wire) {
      case kVarint:
        (void)varint();
        break;
      case kFixed64:
        advance(8);
        break;
      case kLen:
        (void)bytes();
        break;
      case kFixed32:
        advance(4);
        break;
      default:
        ok_ = false;  // groups (3, 4) and invalid types
    }
  }

  // Reads a field of the expected wire type, or marks the reader bad.
  std::uint64_t expect_varint(std::uint32_t wire) noexcept { return check(wire, kVarint) ? varint() : 0; }
  double expect_double(std::uint32_t wire) noexcept { return check(wire, kFixed64) ? fixed64_double() : 0; }
  // Prices travel as fixed64 IEEE-754 rupees; converted at once by the single audited core helper.
  std::optional<Price> expect_price(std::uint32_t wire) noexcept {
    return check(wire, kFixed64) ? Price::from_wire_rupees(fixed64_double()) : std::optional<Price>(Price{});
  }
  std::span<const std::uint8_t> expect_bytes(std::uint32_t wire) noexcept {
    return check(wire, kLen) ? bytes() : std::span<const std::uint8_t>{};
  }

 private:
  bool check(std::uint32_t got, std::uint32_t want) noexcept {
    if (got != want) {
      ok_ = false;
    }
    return ok_;
  }
  void advance(std::size_t n) noexcept {
    if (n > buf_.size() - pos_) {
      ok_ = false;
      return;
    }
    pos_ += n;
  }

  std::span<const std::uint8_t> buf_;
  std::size_t pos_{0};
  bool ok_{true};
};

// Epoch milliseconds from the wire → ns; 0 (treated as "absent") when ≤ 0 or beyond int64 ns.
Nanos ms_to_nanos(std::uint64_t ms) noexcept {
  constexpr auto kMaxMs = static_cast<std::uint64_t>(INT64_MAX / 1'000'000);
  return ms > 0 && ms <= kMaxMs ? static_cast<Nanos>(ms) * 1'000'000 : 0;
}

std::string_view as_text(std::span<const std::uint8_t> b) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes viewed as chars, no copy
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// Decode state for one feed: prices validated as they're read.
struct FeedOut {
  md::Tick* tick{nullptr};
  bool bad_price{false};

  void price(std::optional<Price> p, Price& dst) noexcept {
    if (p.has_value()) {
      dst = *p;
    } else {
      bad_price = true;
    }
  }
  static std::uint64_t non_negative(double v) noexcept {
    return std::isfinite(v) && v > 0 ? static_cast<std::uint64_t>(std::llround(v)) : 0;
  }
};

bool decode_ltpc(std::span<const std::uint8_t> b, FeedOut& o) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    switch (f) {
      case 1:
        o.price(r.expect_price(w), o.tick->ltp);
        break;
      case 2: {  // last trade time, ms
        const Nanos ts = ms_to_nanos(r.expect_varint(w));
        if (ts != 0) {
          o.tick->exchange_ts = ts;
        }
        break;
      }
      case 3:
        o.tick->ltq = static_cast<std::uint32_t>(std::min<std::uint64_t>(r.expect_varint(w), UINT32_MAX));
        break;
      case 4:
        o.price(r.expect_price(w), o.tick->prev_close);
        break;
      default:
        r.skip(w);
    }
  }
  return r.ok();
}

bool decode_quote(std::span<const std::uint8_t> b, FeedOut& o, std::size_t level) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  md::DepthLevel* bid = level < md::kDepthLevels ? o.tick->bids.data() + level : nullptr;
  md::DepthLevel* ask = level < md::kDepthLevels ? o.tick->asks.data() + level : nullptr;
  Price ignored;
  while (r.next(f, w)) {
    switch (f) {
      case 1: {
        const auto q = static_cast<std::uint32_t>(std::min<std::uint64_t>(r.expect_varint(w), UINT32_MAX));
        if (bid != nullptr) {
          bid->qty = q;
        }
        break;
      }
      case 2:
        o.price(r.expect_price(w), bid != nullptr ? bid->price : ignored);
        break;
      case 3: {
        const auto q = static_cast<std::uint32_t>(std::min<std::uint64_t>(r.expect_varint(w), UINT32_MAX));
        if (ask != nullptr) {
          ask->qty = q;
        }
        break;
      }
      case 4:
        o.price(r.expect_price(w), ask != nullptr ? ask->price : ignored);
        break;
      default:
        r.skip(w);
    }
  }
  return r.ok();
}

bool decode_market_level(std::span<const std::uint8_t> b, FeedOut& o) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  std::size_t level = 0;  // full_d30 sends 30 levels; Tick keeps the best 5
  while (r.next(f, w)) {
    if (f == 1) {
      if (!decode_quote(r.expect_bytes(w), o, level++)) {
        return false;
      }
    } else {
      r.skip(w);
    }
  }
  return r.ok();
}

// Day OHLC ("1d") from MarketOHLC; intraday intervals are ignored (bars are built by the engine).
bool decode_ohlc_list(std::span<const std::uint8_t> b, FeedOut& o) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    if (f != 1) {
      r.skip(w);
      continue;
    }
    Reader c(r.expect_bytes(w));
    std::uint32_t cf = 0;
    std::uint32_t cw = 0;
    bool day = false;
    std::optional<Price> open;
    std::optional<Price> high;
    std::optional<Price> low;
    while (c.next(cf, cw)) {
      switch (cf) {
        case 1:
          day = as_text(c.expect_bytes(cw)) == "1d";
          break;
        case 2:
          open = c.expect_price(cw);
          break;
        case 3:
          high = c.expect_price(cw);
          break;
        case 4:
          low = c.expect_price(cw);
          break;
        default:
          c.skip(cw);
      }
    }
    if (!c.ok()) {
      return false;
    }
    if (day) {
      o.price(open, o.tick->open);
      o.price(high, o.tick->high);
      o.price(low, o.tick->low);
    }
  }
  return r.ok();
}

bool decode_market_ff(std::span<const std::uint8_t> b, FeedOut& o) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    bool ok = true;
    switch (f) {
      case 1:
        ok = decode_ltpc(r.expect_bytes(w), o);
        break;
      case 2:
        ok = decode_market_level(r.expect_bytes(w), o);
        break;
      case 4:
        ok = decode_ohlc_list(r.expect_bytes(w), o);
        break;
      case 5:
        o.price(r.expect_price(w), o.tick->avg_price);
        break;
      case 6:
        o.tick->volume = r.expect_varint(w);
        break;
      case 7:
        o.tick->oi = FeedOut::non_negative(r.expect_double(w));
        break;
      default:
        r.skip(w);
    }
    if (!ok) {
      return false;
    }
  }
  return r.ok();
}

bool decode_index_ff(std::span<const std::uint8_t> b, FeedOut& o) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    bool ok = true;
    if (f == 1) {
      ok = decode_ltpc(r.expect_bytes(w), o);
    } else if (f == 2) {
      ok = decode_ohlc_list(r.expect_bytes(w), o);
    } else {
      r.skip(w);
    }
    if (!ok) {
      return false;
    }
  }
  return r.ok();
}

bool decode_first_level(std::span<const std::uint8_t> b, FeedOut& o) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    bool ok = true;
    switch (f) {
      case 1:
        ok = decode_ltpc(r.expect_bytes(w), o);
        break;
      case 2:
        ok = decode_quote(r.expect_bytes(w), o, 0);
        break;
      case 4:
        o.tick->volume = r.expect_varint(w);
        break;
      case 5:
        o.tick->oi = FeedOut::non_negative(r.expect_double(w));
        break;
      default:
        r.skip(w);
    }
    if (!ok) {
      return false;
    }
  }
  return r.ok();
}

bool decode_feed(std::span<const std::uint8_t> b, FeedOut& o) noexcept {
  Reader r(b);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    bool ok = true;
    switch (f) {
      case 1:
        ok = decode_ltpc(r.expect_bytes(w), o);
        break;
      case 2: {  // FullFeed oneof
        Reader ff(r.expect_bytes(w));
        std::uint32_t g = 0;
        std::uint32_t gw = 0;
        while (ok && ff.next(g, gw)) {
          if (g == 1) {
            ok = decode_market_ff(ff.expect_bytes(gw), o);
          } else if (g == 2) {
            ok = decode_index_ff(ff.expect_bytes(gw), o);
          } else {
            ff.skip(gw);
          }
        }
        ok = ok && ff.ok();
        break;
      }
      case 3:
        ok = decode_first_level(r.expect_bytes(w), o);
        break;
      default:
        r.skip(w);  // requestMode
    }
    if (!ok) {
      return false;
    }
  }
  return r.ok();
}

// Header fields (type, currentTs), which may follow the feeds on the wire.
bool read_header(std::span<const std::uint8_t> frame, FrameType& type, Nanos& current) noexcept {
  Reader r(frame);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    if (f == 1) {
      const std::uint64_t t = r.expect_varint(w);
      type = t <= 2 ? static_cast<FrameType>(t) : FrameType::kUnknown;
    } else if (f == 3) {
      current = ms_to_nanos(r.expect_varint(w));
    } else {
      r.skip(w);
    }
  }
  return r.ok();
}

// One map entry { string key = 1; Feed value = 2; }.
bool read_entry(std::span<const std::uint8_t> entry, std::string_view& key,
                std::span<const std::uint8_t>& feed) noexcept {
  Reader r(entry);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  while (r.next(f, w)) {
    if (f == 1) {
      key = as_text(r.expect_bytes(w));
    } else if (f == 2) {
      feed = r.expect_bytes(w);
    } else {
      r.skip(w);
    }
  }
  return r.ok();
}

enum class FeedResult : std::uint8_t { kTick, kBadPrice, kMalformed };

FeedResult decode_tick(std::span<const std::uint8_t> feed, md::InstrumentId id, Nanos default_ts,
                       Nanos recv_ts, md::Tick& tick) noexcept {
  tick = md::Tick{};
  tick.instrument_id = id;
  tick.source = md::Source::kUpstox;
  tick.recv_ts = recv_ts;
  tick.exchange_ts = default_ts;  // ltt (if present) overrides it
  FeedOut out{&tick};
  if (!decode_feed(feed, out)) {
    return FeedResult::kMalformed;
  }
  if (out.bad_price) {
    return FeedResult::kBadPrice;
  }
  tick.bid = tick.bids[0].price;
  tick.bid_qty = tick.bids[0].qty;
  tick.ask = tick.asks[0].price;
  tick.ask_qty = tick.asks[0].qty;
  return FeedResult::kTick;
}

}  // namespace

KeyMap::KeyMap(std::vector<std::pair<std::string, md::InstrumentId>> entries) : entries_(std::move(entries)) {
  std::ranges::sort(entries_);
  const auto dup = std::ranges::unique(entries_, {}, &std::pair<std::string, md::InstrumentId>::first);
  entries_.erase(dup.begin(), dup.end());
}

md::InstrumentId KeyMap::find(std::string_view key) const noexcept {
  const auto it =
      std::ranges::lower_bound(entries_, key, {}, [](const auto& e) { return std::string_view(e.first); });
  return it != entries_.end() && it->first == key ? it->second : 0;
}

namespace detail {

FrameType decode_frame_impl(std::span<const std::uint8_t> frame, const KeyMap& keys, Nanos recv_ts,
                            DecodeStats& stats, TickFn fn, void* ctx) noexcept {
  ++stats.frames;
  FrameType type = FrameType::kUnknown;
  Nanos current = 0;
  if (!read_header(frame, type, current)) {
    ++stats.malformed;
    return type;
  }
  if (type == FrameType::kMarketInfo) {
    ++stats.market_info;
    return type;
  }
  // Feeds map entries { string key = 1; Feed value = 2; }.
  Reader r(frame);
  std::uint32_t f = 0;
  std::uint32_t w = 0;
  md::Tick tick;
  while (r.next(f, w)) {
    if (f != 2) {
      r.skip(w);
      continue;
    }
    std::string_view key;
    std::span<const std::uint8_t> feed;
    if (!read_entry(r.expect_bytes(w), key, feed) || !r.ok()) {
      ++stats.malformed;
      return type;
    }
    ++stats.feeds;
    const md::InstrumentId id = keys.find(key);
    if (id == 0) {
      ++stats.unknown_key;
      continue;
    }
    switch (decode_tick(feed, id, current != 0 ? current : recv_ts, recv_ts, tick)) {
      case FeedResult::kTick:
        ++stats.ticks;
        fn(ctx, tick);
        break;
      case FeedResult::kBadPrice:
        ++stats.bad_price;
        break;
      case FeedResult::kMalformed:
        ++stats.malformed;
        return type;
    }
  }
  if (!r.ok()) {
    ++stats.malformed;
  }
  return type;
}

}  // namespace detail

}  // namespace vyuha::broker::upstox
