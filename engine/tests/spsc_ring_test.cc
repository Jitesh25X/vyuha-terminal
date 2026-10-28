#include "vyuha/core/spsc_ring.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vyuha::SpscRing;

TEST(SpscRing, StartsEmpty) {
  SpscRing<int, 4> ring;
  int out = -1;
  EXPECT_FALSE(ring.try_pop(out));
  EXPECT_EQ(out, -1);
  EXPECT_TRUE(ring.empty_approx());
  EXPECT_EQ((SpscRing<int, 4>::capacity()), 4U);
}

TEST(SpscRing, UsesAllSlotsThenReportsFull) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.try_push(i));
  }
  EXPECT_FALSE(ring.try_push(99));
  EXPECT_EQ(ring.size_approx(), 4U);
  int out = -1;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(ring.try_push(4));  // a freed slot is reusable
  EXPECT_FALSE(ring.try_push(5));
}

TEST(SpscRing, FifoAcrossManyWraps) {
  SpscRing<std::uint64_t, 8> ring;
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  // Varying fill levels exercise every slot/index alignment.
  for (int round = 0; round < 1000; ++round) {
    const auto burst = static_cast<std::uint64_t>(round % 8) + 1;
    for (std::uint64_t i = 0; i < burst; ++i) {
      ASSERT_TRUE(ring.try_push(next_in++));
    }
    std::uint64_t out = 0;
    while (ring.try_pop(out)) {
      ASSERT_EQ(out, next_out++);
    }
  }
  EXPECT_EQ(next_in, next_out);
}

TEST(SpscRing, MovesOwningTypes) {
  SpscRing<std::unique_ptr<std::string>, 2> ring;
  auto value = std::make_unique<std::string>("tick");
  ASSERT_TRUE(ring.try_push(std::move(value)));
  EXPECT_EQ(value, nullptr);
  auto rejected = std::make_unique<std::string>("kept");
  ASSERT_TRUE(ring.try_emplace(std::make_unique<std::string>("second")));
  EXPECT_FALSE(ring.try_push(std::move(rejected)));
  ASSERT_NE(rejected, nullptr);  // a failed push leaves the argument untouched
  std::unique_ptr<std::string> out;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(*out, "tick");
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(*out, "second");
}

TEST(SpscRing, ProducerAndConsumerOnSeparateCacheLines) {
  // Not a guarantee of the API, but the reason the class exists: no false sharing between sides.
  EXPECT_GE((alignof(SpscRing<int, 4>)), vyuha::kCacheLine);
  EXPECT_GE((sizeof(SpscRing<int, 4>)), 3 * vyuha::kCacheLine);
}

// A payload wider than one atomic word: a torn read (slot reused before it was consumed, or
// read before it was fully written) shows up as words that disagree.
struct Wide {
  std::array<std::uint64_t, 8> words{};
};

TEST(SpscRing, ConcurrentStreamKeepsOrderAndIntegrity) {
  constexpr std::uint64_t kCount = 2'000'000;
  auto ring = std::make_unique<SpscRing<Wide, 1024>>();

  std::thread producer([&ring] {
    for (std::uint64_t seq = 0; seq < kCount; ++seq) {
      Wide w;
      w.words.fill(seq);
      while (!ring->try_push(w)) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  std::uint64_t bad = 0;
  Wide w;
  while (expected < kCount) {
    if (!ring->try_pop(w)) {
      std::this_thread::yield();
      continue;
    }
    for (const auto word : w.words) {
      bad += word != expected ? 1 : 0;
    }
    ++expected;
  }
  producer.join();
  EXPECT_EQ(bad, 0U);
  EXPECT_TRUE(ring->empty_approx());
}

TEST(SpscRing, ConcurrentWithTinyRingStaysCorrectWhenAlwaysFull) {
  constexpr std::uint64_t kCount = 200'000;
  SpscRing<std::uint64_t, 2> ring;
  std::thread producer([&ring] {
    for (std::uint64_t seq = 1; seq <= kCount; ++seq) {
      while (!ring.try_push(seq)) {
      }
    }
  });
  std::uint64_t last = 0;
  std::uint64_t out = 0;
  while (last < kCount) {
    if (ring.try_pop(out)) {
      ASSERT_EQ(out, last + 1);
      last = out;
    }
  }
  producer.join();
}

}  // namespace
