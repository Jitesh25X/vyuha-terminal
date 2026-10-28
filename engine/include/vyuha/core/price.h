#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "vyuha/core/error.h"

namespace vyuha {

// Exact price/money in "price units": 1 rupee = 10'000 units (1 unit = 1/100 paisa), so every
// exchange tick size (₹0.05, ₹0.01, ₹0.0025 for currency) is an integer. Never a binary float
// (CLAUDE.md invariant 8). The same convention lives in price.go / price.py / price.ts.
class Price {
 public:
  static constexpr std::int64_t kUnitsPerRupee = 10'000;
  static constexpr std::int64_t kUnitsPerPaisa = 100;

  constexpr Price() noexcept = default;

  [[nodiscard]] static constexpr Price from_units(std::int64_t units) noexcept { return Price{units}; }
  // Kite's binary feed and many broker payloads carry paise.
  [[nodiscard]] static constexpr Price from_paise(std::int64_t paise) noexcept {
    return Price{paise * kUnitsPerPaisa};
  }
  // Exact decimal rupees, e.g. "2450.05", "-0.0025", "100". At most 4 decimal places; no exponent.
  [[nodiscard]] static std::expected<Price, Error> parse(std::string_view rupees) noexcept;

  [[nodiscard]] constexpr std::int64_t units() const noexcept { return units_; }
  // Rupees with exactly 4 decimals, e.g. "2450.0500" (the Postgres numeric(18,4) text form).
  [[nodiscard]] std::string to_string() const;

  constexpr auto operator<=>(const Price&) const noexcept = default;
  constexpr Price operator+(Price o) const noexcept { return Price{units_ + o.units_}; }
  constexpr Price operator-(Price o) const noexcept { return Price{units_ - o.units_}; }
  constexpr Price operator-() const noexcept { return Price{-units_}; }
  constexpr Price& operator+=(Price o) noexcept {
    units_ += o.units_;
    return *this;
  }
  constexpr Price& operator-=(Price o) noexcept {
    units_ -= o.units_;
    return *this;
  }

 private:
  constexpr explicit Price(std::int64_t units) noexcept : units_(units) {}
  std::int64_t units_{0};
};

}  // namespace vyuha
