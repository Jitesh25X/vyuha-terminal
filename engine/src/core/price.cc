#include "vyuha/core/price.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace vyuha {

namespace {

constexpr std::size_t kMaxDecimals = 4;

}  // namespace

std::expected<Price, Error> Price::parse(std::string_view rupees) noexcept {
  std::string_view s = rupees;
  bool negative = false;
  if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
    negative = s.front() == '-';
    s = std::string_view(s.data() + 1, s.size() - 1);
  }
  const std::size_t dot = s.find('.');
  // Views built from pointer + length (substr may throw, and parse is noexcept).
  const bool has_dot = dot != std::string_view::npos;
  const std::string_view whole(s.data(), has_dot ? dot : s.size());
  const std::string_view frac =
      has_dot ? std::string_view(s.data() + dot + 1, s.size() - dot - 1) : std::string_view{};
  if (whole.empty() && frac.empty()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "price: no digits"});
  }
  if (has_dot && frac.empty()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "price: no digits after the decimal point"});
  }
  if (frac.size() > kMaxDecimals) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "price: more than 4 decimal places"});
  }
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  std::int64_t units = 0;
  const auto push_digit = [&units](char c) noexcept -> bool {
    if (c < '0' || c > '9') {
      return false;
    }
    const auto d = static_cast<std::int64_t>(c - '0');
    if (units > (kMax - d) / 10) {
      return false;
    }
    units = units * 10 + d;
    return true;
  };
  for (const char c : whole) {
    if (!push_digit(c)) {
      return std::unexpected(
          Error{ErrorCode::kInvalidArgument, "price: not a decimal number or out of range"});
    }
  }
  for (std::size_t i = 0; i < kMaxDecimals; ++i) {
    if (!push_digit(i < frac.size() ? frac[i] : '0')) {
      return std::unexpected(
          Error{ErrorCode::kInvalidArgument, "price: not a decimal number or out of range"});
    }
  }
  return Price{negative ? -units : units};
}

std::optional<Price> Price::from_wire_rupees(double rupees) noexcept {
  constexpr double kMaxRupees = 1e11;  // keeps units well inside int64
  if (!std::isfinite(rupees) || rupees < 0 || rupees > kMaxRupees) {
    return std::nullopt;
  }
  return Price{std::llround(rupees * static_cast<double>(kUnitsPerRupee))};
}

std::string Price::to_string() const {
  // Work in unsigned magnitude so INT64_MIN formats correctly.
  const bool negative = units_ < 0;
  const std::uint64_t mag =
      negative ? static_cast<std::uint64_t>(-(units_ + 1)) + 1U : static_cast<std::uint64_t>(units_);
  const auto per = static_cast<std::uint64_t>(kUnitsPerRupee);
  std::string frac = std::to_string(mag % per);
  frac.insert(0, kMaxDecimals - frac.size(), '0');
  return (negative ? "-" : "") + std::to_string(mag / per) + "." + frac;
}

}  // namespace vyuha
