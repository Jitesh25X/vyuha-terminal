#include "vyuha/broker/upstox/subscriptions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vyuha::broker::upstox {

namespace {

constexpr std::array<const char*, 4> kModeNames{"ltpc", "option_greeks", "full", "full_d30"};

ModeLimit limit_of(const Limits& l, Mode m) noexcept {
  switch (m) {
    case Mode::kLtpc:
      return l.ltpc;
    case Mode::kOptionGreeks:
      return l.option_greeks;
    case Mode::kFull:
      return l.full;
    case Mode::kFullD30:
      return l.full_d30;
  }
  return {0, 0};
}

void append_json_string(std::string& out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      constexpr std::string_view kHex = "0123456789abcdef";
      out += "\\u00";
      out += kHex.at((static_cast<unsigned char>(c) >> 4U) & 0xFU);
      out += kHex.at(static_cast<unsigned char>(c) & 0xFU);
    } else {
      out += c;
    }
  }
  out += '"';
}

}  // namespace

std::expected<std::vector<Subscription>, Error> plan_subscriptions(std::span<const Subscription> subs,
                                                                   Plan plan) {
  std::vector<Subscription> unique;
  unique.reserve(subs.size());
  std::unordered_map<std::string_view, std::size_t> seen;
  for (const auto& s : subs) {
    if (s.key.empty()) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, "upstox: empty instrument key"});
    }
    const auto [it, inserted] = seen.try_emplace(s.key, unique.size());
    if (inserted) {
      unique.push_back(s);
    } else {
      unique[it->second].mode = std::max(unique[it->second].mode, s.mode);
    }
  }
  const Limits lim = limits_for(plan);
  std::array<std::size_t, 4> count{};
  for (const auto& s : unique) {
    ++count.at(static_cast<std::size_t>(s.mode));
  }
  const auto modes_used = std::ranges::count_if(count, [](std::size_t n) { return n > 0; });
  for (std::size_t m = 0; m < count.size(); ++m) {
    if (count.at(m) == 0) {
      continue;
    }
    const ModeLimit ml = limit_of(lim, static_cast<Mode>(m));
    const std::size_t cap = modes_used > 1 ? ml.combined : ml.individual;
    if (ml.individual == 0) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, "upstox: mode not available on this plan"});
    }
    if (count.at(m) > cap) {
      return std::unexpected(
          Error{ErrorCode::kCapacityExceeded, "upstox: subscriptions exceed the mode limit"});
    }
  }
  return unique;
}

std::vector<std::string> subscribe_messages(std::span<const Subscription> subs, std::size_t chunk) {
  std::vector<std::string> out;
  chunk = std::max<std::size_t>(chunk, 1);
  std::size_t guid = 0;
  for (std::size_t m = 0; m < kModeNames.size(); ++m) {
    std::vector<std::string_view> keys;
    for (const auto& s : subs) {
      if (static_cast<std::size_t>(s.mode) == m) {
        keys.emplace_back(s.key);
      }
    }
    for (std::size_t at = 0; at < keys.size(); at += chunk) {
      std::string msg = R"({"guid":"vyuha-sub-)" + std::to_string(++guid) +
                        R"(","method":"sub","data":{"mode":")" + kModeNames.at(m) + R"(","instrumentKeys":[)";
      const std::size_t end = std::min(keys.size(), at + chunk);
      for (std::size_t i = at; i < end; ++i) {
        if (i != at) {
          msg += ',';
        }
        append_json_string(msg, keys[i]);
      }
      msg += "]}}";
      out.push_back(std::move(msg));
    }
  }
  return out;
}

}  // namespace vyuha::broker::upstox
