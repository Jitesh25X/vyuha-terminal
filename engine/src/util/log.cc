#include "vyuha/util/log.h"

#include <atomic>
#include <mutex>
#include <utility>

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"

namespace vyuha::log {

namespace {

struct State {
  std::atomic<quill::Logger*> engine{nullptr};
  std::once_flag once;
};

State& state() noexcept {
  static State s;
  return s;
}

}  // namespace

void init() {
  State& st = state();
  std::call_once(st.once, [&st] {
    quill::Backend::start();
    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("stderr");
    st.engine.store(quill::Frontend::create_or_get_logger("engine", std::move(sink)),
                    std::memory_order_release);
  });
}

quill::Logger* engine() noexcept { return state().engine.load(std::memory_order_acquire); }

}  // namespace vyuha::log
