#include "vyuha/core/price.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

namespace {

using vyuha::ErrorCode;
using vyuha::Price;

TEST(Price, UnitsArePaisaHundredths) {
  EXPECT_EQ(Price::from_paise(245005).units(), 24'500'500);  // ₹2450.05
  EXPECT_EQ(Price::from_units(500).to_string(), "0.0500");   // ₹0.05 tick is exact
}

struct ParseCase {
  const char* text;
  std::int64_t units;
  const char* canonical;
};

class PriceParse : public ::testing::TestWithParam<ParseCase> {};

TEST_P(PriceParse, ParsesAndRoundTrips) {
  const auto& c = GetParam();
  const auto p = Price::parse(c.text);
  ASSERT_TRUE(p.has_value()) << c.text;
  EXPECT_EQ(p->units(), c.units);
  EXPECT_EQ(p->to_string(), c.canonical);
  EXPECT_EQ(Price::parse(p->to_string())->units(), c.units);  // round trip through the canonical text
}

INSTANTIATE_TEST_SUITE_P(
    Cases, PriceParse,
    ::testing::Values(ParseCase{"2450.05", 24'500'500, "2450.0500"}, ParseCase{"0.0025", 25, "0.0025"},
                      ParseCase{"100", 1'000'000, "100.0000"}, ParseCase{"-12.5", -125'000, "-12.5000"},
                      ParseCase{"+0.10", 1'000, "0.1000"}, ParseCase{".05", 500, "0.0500"},
                      ParseCase{"0", 0, "0.0000"},
                      ParseCase{"922337203685477.5807", std::numeric_limits<std::int64_t>::max(),
                                "922337203685477.5807"}));

TEST(Price, ParseRejects) {
  for (const char* bad :
       {"", "-", ".", "1.", "1.23456", "1e3", "12a", "1,000", "922337203685477.5808", " 1"}) {
    const auto p = Price::parse(bad);
    ASSERT_FALSE(p.has_value()) << bad;
    EXPECT_EQ(p.error().code, ErrorCode::kInvalidArgument);
  }
}

TEST(Price, FormatsExtremes) {
  EXPECT_EQ(Price::from_units(std::numeric_limits<std::int64_t>::min()).to_string(), "-922337203685477.5808");
  EXPECT_EQ(Price::from_units(-1).to_string(), "-0.0001");
}

// Broker wire formats that send binary doubles (Upstox) convert here, once.
TEST(Price, FromWireRupeesRoundsToTheUnit) {
  EXPECT_EQ(Price::from_wire_rupees(219.3).value_or(Price{}).units(), 2'193'000);
  EXPECT_EQ(Price::from_wire_rupees(0.05).value_or(Price{}).units(), 500);
  EXPECT_EQ(Price::from_wire_rupees(24450.05).value_or(Price{}).to_string(), "24450.0500");
  for (const double bad : {-0.05, std::numeric_limits<double>::infinity(), std::nan(""), 1e12}) {
    EXPECT_FALSE(Price::from_wire_rupees(bad).has_value()) << bad;
  }
}

TEST(Price, ArithmeticAndOrdering) {
  const Price a = Price::from_paise(10'000);  // ₹100
  const Price tick = Price::from_units(500);  // ₹0.05
  EXPECT_EQ((a + tick).to_string(), "100.0500");
  EXPECT_EQ((a - tick).to_string(), "99.9500");
  EXPECT_LT(a - tick, a);
  Price b = a;
  b += tick;
  b -= tick;
  EXPECT_EQ(b, a);
}

}  // namespace
