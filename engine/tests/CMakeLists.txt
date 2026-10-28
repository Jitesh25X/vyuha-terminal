find_package(Threads REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(vyuha_tests
  bar_builder_test.cc
  core_loop_test.cc
  kite_parser_test.cc
  kite_ticker_test.cc
  price_test.cc
  spsc_ring_test.cc
  version_test.cc
)
target_link_libraries(vyuha_tests PRIVATE vyuha::core vyuha::kite Boost::beast vyuha_warnings Threads::Threads GTest::gtest_main)
gtest_discover_tests(vyuha_tests)
target_compile_definitions(vyuha_tests PRIVATE VYUHA_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
