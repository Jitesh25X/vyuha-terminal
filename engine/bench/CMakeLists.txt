find_package(Threads REQUIRED)
find_package(benchmark CONFIG REQUIRED)

add_executable(vyuha_bench
  spsc_ring_bench.cc
  version_bench.cc
)
target_link_libraries(vyuha_bench PRIVATE vyuha::core vyuha_warnings Threads::Threads benchmark::benchmark_main)
