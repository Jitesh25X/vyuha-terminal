find_package(Threads REQUIRED)
find_package(benchmark CONFIG REQUIRED)

add_executable(vyuha_bench
  core_loop_bench.cc
  risk_gate_bench.cc
  spsc_ring_bench.cc
  version_bench.cc
)
target_link_libraries(vyuha_bench PRIVATE vyuha::core vyuha::risk vyuha_warnings Threads::Threads benchmark::benchmark_main)
