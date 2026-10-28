#pragma once

#include "quill/Logger.h"

namespace vyuha::log {

// Starts Quill's async backend thread and a stderr sink once per process. Formatting happens on
// the backend thread, so the core thread only enqueues (engine/CLAUDE.md hot-path rule).
void init();

// The engine logger, or nullptr before init() (tests and embedded use run without logging).
[[nodiscard]] quill::Logger* engine() noexcept;

}  // namespace vyuha::log
