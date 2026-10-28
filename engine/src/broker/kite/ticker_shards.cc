#include "vyuha/broker/kite/ticker_shards.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vyuha::broker::kite {

std::expected<std::vector<Shard>, Error> plan_shards(std::span<const Subscription> subs,
                                                     const TickerLimits& limits) {
  // Deduplicate, keeping the richest mode, in a stable order (first appearance).
  std::vector<Subscription> unique;
  unique.reserve(subs.size());
  std::unordered_map<std::uint32_t, std::size_t> seen;
  for (const auto& s : subs) {
    if (s.token == 0) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, "kite: instrument token 0"});
    }
    const auto [it, inserted] = seen.try_emplace(s.token, unique.size());
    if (inserted) {
      unique.push_back(s);
    } else {
      Mode& m = unique[it->second].mode;
      m = std::max(m, s.mode);
    }
  }
  if (limits.max_per_connection == 0 || unique.size() > limits.max_per_connection * limits.max_connections) {
    return std::unexpected(Error{ErrorCode::kCapacityExceeded,
                                 "kite: subscriptions exceed per-key limit (connections × instruments)"});
  }
  std::vector<Shard> shards;
  for (std::size_t at = 0; at < unique.size(); at += limits.max_per_connection) {
    const std::size_t end = std::min(unique.size(), at + limits.max_per_connection);
    shards.push_back(Shard{{unique.begin() + static_cast<std::ptrdiff_t>(at),
                            unique.begin() + static_cast<std::ptrdiff_t>(end)}});
  }
  return shards;
}

namespace {

std::string token_list(const Shard& shard, auto&& keep) {
  std::string out = "[";
  bool first = true;
  for (const auto& s : shard.subs) {
    if (!keep(s)) {
      continue;
    }
    if (!first) {
      out += ',';
    }
    out += std::to_string(s.token);
    first = false;
  }
  out += ']';
  return out;
}

}  // namespace

std::vector<std::string> subscribe_messages(const Shard& shard) {
  std::vector<std::string> out;
  if (shard.subs.empty()) {
    return out;
  }
  out.push_back(R"({"a":"subscribe","v":)" + token_list(shard, [](const Subscription&) { return true; }) +
                "}");
  constexpr std::array<const char*, 3> kNames{"ltp", "quote", "full"};
  for (const Mode m : {Mode::kLtp, Mode::kQuote, Mode::kFull}) {
    const bool used = std::ranges::any_of(shard.subs, [m](const Subscription& s) { return s.mode == m; });
    if (used) {
      out.push_back(std::string(R"({"a":"mode","v":[")") + kNames.at(static_cast<std::size_t>(m)) + "\"," +
                    token_list(shard, [m](const Subscription& s) { return s.mode == m; }) + "]}");
    }
  }
  return out;
}

}  // namespace vyuha::broker::kite
