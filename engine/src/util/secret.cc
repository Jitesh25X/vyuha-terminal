#include "vyuha/util/secret.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace vyuha {

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a raw buffer is what gets mlock'ed
SecretRef::SecretRef(std::string_view value)
    : data_(std::make_unique<char[]>(value.size() + 1)),
      size_(value.size()),
      locked_(::mlock(data_.get(), size_ + 1) == 0) {  // may fail under RLIMIT_MEMLOCK: still zeroised
  std::copy(value.begin(), value.end(), data_.get());
}
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

SecretRef::SecretRef(SecretRef&& other) noexcept
    : data_(std::move(other.data_)),
      size_(std::exchange(other.size_, 0)),
      locked_(std::exchange(other.locked_, false)) {}

SecretRef& SecretRef::operator=(SecretRef&& other) noexcept {
  if (this != &other) {
    wipe();
    data_ = std::move(other.data_);
    size_ = std::exchange(other.size_, 0);
    locked_ = std::exchange(other.locked_, false);
  }
  return *this;
}

SecretRef::~SecretRef() { wipe(); }

void SecretRef::wipe() noexcept {
  if (!data_) {
    return;
  }
  // Volatile writes so the compiler can't drop the zeroing of memory about to be freed.
  volatile char* p = data_.get();
  for (std::size_t i = 0; i <= size_; ++i) {
    p[i] = 0;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  if (locked_) {
    ::munlock(data_.get(), size_ + 1);
  }
  data_.reset();
  size_ = 0;
  locked_ = false;
}

}  // namespace vyuha
