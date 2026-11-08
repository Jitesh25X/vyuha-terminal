#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "vyuha/md/tick.h"

namespace vyuha::bus {

// Protobuf wire encoders for vyuha.marketdata.v1.{Tick, Bar, Snapshot} (proto/vyuha/marketdata/v1).
// Hand-written so the publisher thread neither allocates nor depends on the protobuf runtime;
// `vyuha_reference_tests` checks the bytes against protoc-generated classes. Each returns the
// encoded size, or 0 if `out` is too small (callers size buffers with kMax*Bytes).

inline constexpr std::size_t kMaxTickBytes = 512;
inline constexpr std::size_t kMaxBarBytes = 128;
inline constexpr std::size_t kMaxSnapshotBytes = kMaxTickBytes + 32;

inline constexpr std::size_t kMaxSystemAlertBytes = 256;

// vyuha.common.v1.SystemAlert, for NATS `sys.alert`.
struct SystemAlertFields {
  std::string_view code;
  std::string_view severity;
  std::string_view banner;
  bool active{false};
  std::int64_t since_ms{0};
};

[[nodiscard]] std::size_t encode_system_alert(const SystemAlertFields& a,
                                              std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode_tick(const md::Tick& t, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode_bar(const md::Bar& b, std::span<std::uint8_t> out) noexcept;
// Latest snapshot for the NATS KV `md_last`: the tick plus change % vs previous close.
[[nodiscard]] std::size_t encode_snapshot(const md::Tick& t, bool stale,
                                          std::span<std::uint8_t> out) noexcept;

}  // namespace vyuha::bus
