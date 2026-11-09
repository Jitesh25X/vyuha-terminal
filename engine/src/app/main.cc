// vyuha_engine: core loop + broker sessions + gRPC control API (engine.v1.EngineService).
// Env: ENGINE_GRPC_ADDR (default 127.0.0.1:7443; private network + mTLS in prod, see README),
//      NATS_URL (market data out; empty = count only), POSTGRES_URL (instrument master; empty = none).

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "../rpc/engine_service.h"
#include "quill/LogMacros.h"
#include "vyuha/bus/md_publisher.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"
#include "vyuha/core/slew_check.h"
#include "vyuha/core/version.h"
#include "vyuha/md/tick.h"
#include "vyuha/session/instrument_directory.h"
#include "vyuha/session/pg_instrument_directory.h"
#include "vyuha/session/session_manager.h"
#include "vyuha/util/log.h"

namespace {

constexpr std::size_t kFeedRings = 4;  // concurrent data-entitled broker sessions

std::atomic<bool> g_stop{false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): signal flag

extern "C" void on_signal(int /*sig*/) { g_stop.store(true); }

// Until the NATS publisher lands (M1.7), the core forwards into this counter.
class CountingSink final : public vyuha::md::IMdSink {
 public:
  void on_tick(const vyuha::md::Tick& /*t*/) noexcept override {
    ticks.fetch_add(1, std::memory_order_relaxed);
  }
  void on_bar(const vyuha::md::Bar& /*b*/) noexcept override { bars.fetch_add(1, std::memory_order_relaxed); }
  std::atomic<std::uint64_t> ticks{0};
  std::atomic<std::uint64_t> bars{0};
};

#if defined(__linux__)
// D-36: warns when the host's NTP slew limit is missing or above what the 1 s OPS window assumes. Startup
// only.
void warn_if_slew_unsafe(quill::Logger* log) {
  namespace fs = std::filesystem;
  std::string conf;
  bool found = false;
  const auto add = [&conf, &found](const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
      return;
    }
    conf.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    conf.push_back('\n');
    found = true;
  };
  add("/etc/chrony/chrony.conf");
  add("/etc/chrony.conf");
  for (const char* dir : {"/etc/chrony/conf.d", "/etc/chrony/chrony.d"}) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (it->path().extension() == ".conf") {
        add(it->path());
      }
    }
  }
  if (!found) {
    LOG_WARNING(log,
                "clock: no chrony config found, so the NTP slew limit is unknown (the OPS window assumes at "
                "most {} ppm)",
                vyuha::kMaxSlewPpm);
    return;
  }
  const vyuha::SlewCheck check = vyuha::check_chrony_slew(conf);
  if (!check.ok) {
    const char* why = "";
    if (check.unreadable) {
      why = " (a maxslewrate line can't be read)";
    } else if (!check.configured) {
      why = " (its default: none is set)";
    }
    LOG_WARNING(log,
                "clock: chrony's maxslewrate is {} ppm{}; set it to {} or less (D-36), or a clock-second can "
                "be shorter than a real one during a correction",
                check.ppm, why, vyuha::kMaxSlewPpm);
  }
  if (!check.steps) {
    LOG_WARNING(log,
                "clock: chrony isn't set to `makestep 1 -1` (D-36): after a host pause it would slew the "
                "clock instead of stepping it, and the pause check would not see it");
  }
}
#endif

std::string env_or(const char* name, const char* def) {
  const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read at startup, single-threaded
  return v != nullptr && *v != '\0' ? v : def;
}

int run() {
  vyuha::log::init();
  quill::Logger* log = vyuha::log::engine();
#if defined(__linux__)
  warn_if_slew_unsafe(log);
#endif
  const std::string addr = env_or("ENGINE_GRPC_ADDR", "127.0.0.1:7443");

  const vyuha::WallClock clock;
  CountingSink counting;
  // Market data out: NATS when configured (M1.7), else just counted.
  const std::string nats_url = env_or("NATS_URL", "");
  std::unique_ptr<vyuha::bus::MdPublisher> publisher;
  if (!nats_url.empty()) {
    publisher =
        std::make_unique<vyuha::bus::MdPublisher>(vyuha::bus::PublisherConfig{.url = nats_url}, clock);
    if (auto ok = publisher->start(); !ok) {
      LOG_ERROR(log, "engine: {}", ok.error().detail);
      return 1;
    }
  }
  vyuha::md::IMdSink& sink = publisher ? static_cast<vyuha::md::IMdSink&>(*publisher) : counting;
  vyuha::CoreLoop core(vyuha::CoreConfig{}, clock, sink);
  std::vector<std::unique_ptr<vyuha::TickRing>> rings;
  std::vector<vyuha::TickRing*> ring_ptrs;
  for (std::size_t i = 0; i < kFeedRings; ++i) {
    rings.push_back(std::make_unique<vyuha::TickRing>());
    ring_ptrs.push_back(rings.back().get());
    (void)core.add_input(*rings.back());
  }
  // Instrument ids → broker ids from the Postgres instrument master (quant sync, F-MD-1).
  const std::string pg_url = env_or("POSTGRES_URL", "");
  const vyuha::session::StaticInstrumentDirectory no_instruments({});
  std::unique_ptr<vyuha::session::PgInstrumentDirectory> pg;
  if (!pg_url.empty()) {
    pg = std::make_unique<vyuha::session::PgInstrumentDirectory>(pg_url);
    const auto n = pg->refresh();
    if (!n) {
      LOG_ERROR(log, "engine: {}", n.error().detail);
      return 1;
    }
    LOG_INFO(log, "instrument master loaded: {} instruments with broker ids", *n);
  }
  const vyuha::session::InstrumentDirectory& instruments =
      pg ? static_cast<const vyuha::session::InstrumentDirectory&>(*pg) : no_instruments;
  vyuha::session::SessionManager sessions(vyuha::session::SessionManagerConfig{}, instruments, ring_ptrs,
                                          clock);
  vyuha::rpc::EngineServiceImpl service(sessions);

  grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || bound_port == 0) {
    LOG_ERROR(log, "engine: cannot listen on {}", addr);
    return 1;
  }
  std::jthread core_thread([&core](const std::stop_token& st) { core.run(st); });
  LOG_INFO(log, "vyuha engine {} listening on {} (gRPC)", vyuha::version(), addr);
  log->flush_log();

  (void)std::signal(SIGINT, on_signal);
  (void)std::signal(SIGTERM, on_signal);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  LOG_INFO(log, "engine shutting down");
  server->Shutdown();
  core_thread.request_stop();
  core_thread.join();
  if (publisher) {
    const auto st = publisher->stats();
    LOG_INFO(log, "md publisher: published_ticks={} published_bars={} dropped={} errors={}",
             st.published_ticks, st.published_bars, st.dropped, st.publish_errors);
    publisher->stop();
  }
  log->flush_log();  // Quill formats on its backend thread: drain it before exit
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& e) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): last-resort message, logging may be down
    (void)std::fprintf(stderr, "vyuha_engine: fatal: %s\n", e.what());
    return 1;
  }
}
