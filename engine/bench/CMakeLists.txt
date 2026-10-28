find_package(benchmark CONFIG REQUIRED)

add_executable(vyuha_bench
  version_bench.cc
)
target_link_libraries(vyuha_bench PRIVATE vyuha::core vyuha_warnings benchmark::benchmark_main)
