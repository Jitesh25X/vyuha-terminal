#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace vyuha {

// A secret held in locked memory (mlock, best effort) and zeroised on destruction (engine
// CLAUDE.md). Not copyable; never printable: reveal() exists only to hand the bytes to a socket.
class SecretRef {
 public:
  SecretRef() = default;
  explicit SecretRef(std::string_view value);
  SecretRef(const SecretRef&) = delete;
  SecretRef& operator=(const SecretRef&) = delete;
  SecretRef(SecretRef&& other) noexcept;
  SecretRef& operator=(SecretRef&& other) noexcept;
  ~SecretRef();

  [[nodiscard]] std::string_view reveal() const noexcept { return {data_.get(), size_}; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

 private:
  void wipe() noexcept;
  std::unique_ptr<char[]> data_;  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::size_t size_{0};
  bool locked_{false};
};

}  // namespace vyuha
