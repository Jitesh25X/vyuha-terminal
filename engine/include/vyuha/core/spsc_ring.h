#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace vyuha {

// Cache line used to keep producer and consumer state apart. 128 covers Apple silicon's line size
// and x86's adjacent-line prefetcher (std::hardware_destructive_interference_size is avoided: GCC
// warns that its value is ABI-unstable when used in headers).
inline constexpr std::size_t kCacheLine = 128;

// Element types: moved in and out by assignment, so slots never construct or destroy on the hot path.
template <class T>
concept SpscElement = std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
                      std::is_nothrow_destructible_v<T>;

// Bounded wait-free single-producer / single-consumer queue (ARCHITECTURE.md §4: IO decode → core).
//
// Exactly one thread may call the producer side (try_push/try_emplace) and exactly one thread the
// consumer side (try_pop); either may call the observers. All N slots are usable.
//
// Indices grow monotonically (64-bit: no wrap in practice) and map to slots with `& (N - 1)`.
// The producer publishes a slot with a release store of `tail_`; the consumer's acquire load of
// `tail_` makes the slot contents visible (and symmetrically for `head_` when a slot is freed).
// Each side keeps a cached copy of the other side's index, so it touches the shared cache line
// only when the ring looks full (producer) or empty (consumer).
//
// Slots hold default-constructed T up front; a popped slot is left in its moved-from state until
// reused. Allocate large rings on the heap (the object is N * sizeof(T) plus four cache lines).
template <SpscElement T, std::size_t N>
  requires(N >= 2 && std::has_single_bit(N))
class SpscRing {
 public:
  SpscRing() noexcept = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;
  ~SpscRing() = default;

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

  // Producer: returns false (and leaves `value` untouched) when the ring is full.
  [[nodiscard]] bool try_push(T&& value) noexcept {
    return push_impl([&](T& slot) { slot = std::move(value); });
  }

  [[nodiscard]] bool try_push(const T& value) noexcept
    requires std::is_nothrow_copy_assignable_v<T>
  {
    return push_impl([&](T& slot) { slot = value; });
  }

  // Producer: assigns T{args...} into the next slot.
  template <class... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
    return push_impl([&](T& slot) { slot = T{std::forward<Args>(args)...}; });
  }

  // Consumer: moves the oldest element into `out`; returns false when the ring is empty.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) {
        return false;
      }
    }
    out = std::move(slot(head));
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Observers: exact when called from the producer or consumer thread while the other side is idle,
  // otherwise a snapshot that may already be stale.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return tail >= head ? tail - head : 0;
  }
  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  static constexpr std::size_t kMask = N - 1;

  // `index & kMask` is always < N (N is a power of two), so the unchecked subscript is in bounds.
  T& slot(std::size_t index) noexcept {
    return slots_[index & kMask];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  }

  template <class Write>
  bool push_impl(Write&& write) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == N) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == N) {
        return false;
      }
    }
    std::forward<Write>(write)(slot(tail));
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer-owned line: its index plus its cached view of the producer's index.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_{0};
  // Producer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_{0};
  alignas(kCacheLine) std::array<T, N> slots_{};
};

}  // namespace vyuha
