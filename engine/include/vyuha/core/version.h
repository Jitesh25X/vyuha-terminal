#pragma once

#include <string_view>

namespace vyuha {

// Engine version, kept in sync with project(VERSION) in engine/CMakeLists.txt.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace vyuha
