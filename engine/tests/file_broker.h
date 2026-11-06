#pragma once

// A broker that outlives the engine: one fsynced line per order in a file, as an exchange's book would
// survive our crash. Used by the crash test and its child process.

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <expected>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "vyuha/oms/order_router.h"

namespace vyuha::testing {

enum class CrashPoint : std::uint8_t { kNone, kBeforeSend, kAfterSend };

class FileBroker final : public oms::IOrderSender, public oms::IBrokerOrderBook {
 public:
  FileBroker(std::string path, CrashPoint crash) : path_(std::move(path)), crash_(crash) {}

  // NOLINTNEXTLINE(bugprone-exception-escape): a test's failure may terminate
  [[nodiscard]] bool send(const oms::Order& /*order*/, std::string_view tag) noexcept override {
    if (crash_ == CrashPoint::kBeforeSend) {
      std::raise(SIGKILL);  // NOLINT(cert-err33-c): it doesn't return
    }
    const std::string line = std::string(tag) + "\n";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): POSIX open
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    const bool ok = fd >= 0 && ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()) &&
                    ::fsync(fd) == 0;
    if (fd >= 0) {
      ::close(fd);
    }
    if (crash_ == CrashPoint::kAfterSend) {
      std::raise(SIGKILL);  // NOLINT(cert-err33-c): it doesn't return
    }
    return ok;
  }

  [[nodiscard]] bool cancel(const oms::Order& /*order*/) noexcept override { return true; }

  [[nodiscard]] std::expected<std::vector<oms::BrokerOrderView>, Error> find_by_tag(
      std::string_view tag) override {
    std::vector<oms::BrokerOrderView> out;
    for (const std::string& line : lines()) {
      if (line == tag) {
        out.push_back({.state = oms::BrokerOrderView::State::kOpen, .broker_order_id = "B-" + line});
      }
    }
    return out;
  }

  [[nodiscard]] std::vector<std::string> lines() const {
    std::vector<std::string> out;
    std::ifstream in(path_);
    for (std::string line; std::getline(in, line);) {
      out.push_back(line);
    }
    return out;
  }

 private:
  std::string path_;
  CrashPoint crash_;
};

}  // namespace vyuha::testing
