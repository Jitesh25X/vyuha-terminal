find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(vyuha_tests
  version_test.cc
)
target_link_libraries(vyuha_tests PRIVATE vyuha::core vyuha_warnings GTest::gtest_main)
gtest_discover_tests(vyuha_tests)
