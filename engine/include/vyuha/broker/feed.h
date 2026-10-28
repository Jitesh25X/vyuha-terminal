#pragma once

#include <cstddef>
#include <cstdint>

#include "vyuha/core/clock.h"

namespace vyuha::broker {

// A market-data outage on one shard, from the last frame received to the first frame after the
// reconnect (F-MD-2 AC2: gaps are logged with start/end timestamps).
struct Gap {
  std::size_t shard{0};
  Nanos start{0};
  Nanos end{0};
};

struct FeedConnStats {
  std::uint64_t connects{0};
  std::uint64_t disconnects{0};
  std::uint64_t frames{0};
};

}  // namespace vyuha::broker
