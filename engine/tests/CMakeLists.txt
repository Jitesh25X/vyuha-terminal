find_package(Threads REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(vyuha_tests
  bar_builder_test.cc
  core_loop_test.cc
  kite_parser_test.cc
  kite_ticker_test.cc
  price_test.cc
  session_manager_test.cc
  upstox_feed_test.cc
  upstox_parser_test.cc
  spsc_ring_test.cc
  version_test.cc
)
target_link_libraries(vyuha_tests PRIVATE vyuha::core vyuha::kite vyuha::upstox vyuha::session Boost::beast vyuha_warnings Threads::Threads GTest::gtest_main)
gtest_discover_tests(vyuha_tests)
target_compile_definitions(vyuha_tests PRIVATE VYUHA_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")

# Reference tests against protoc-generated classes for the vendored Upstox schema. Not built with
# sanitizers: vcpkg's abseil/protobuf aren't instrumented and abseil's layouts change under ASan.
if(NOT VYUHA_SANITIZER)
  find_package(Protobuf CONFIG REQUIRED)
  add_library(upstox_reference_proto STATIC ${PROJECT_SOURCE_DIR}/third_party/upstox/MarketDataFeed.proto)
  target_link_libraries(upstox_reference_proto PUBLIC protobuf::libprotobuf)
  protobuf_generate(TARGET upstox_reference_proto LANGUAGE cpp
                    IMPORT_DIRS ${PROJECT_SOURCE_DIR}/third_party/upstox
                    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/upstox_pb)
  target_include_directories(upstox_reference_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/upstox_pb)

  add_executable(vyuha_reference_tests upstox_reference_test.cc)
  target_link_libraries(vyuha_reference_tests PRIVATE vyuha::upstox upstox_reference_proto vyuha_warnings
                                                      GTest::gtest_main)
  target_compile_definitions(vyuha_reference_tests PRIVATE VYUHA_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
  gtest_discover_tests(vyuha_reference_tests)

  # gRPC adapter end to end (in-process server + client stub).
  add_executable(vyuha_rpc_tests engine_service_test.cc)
  target_link_libraries(vyuha_rpc_tests PRIVATE vyuha_rpc vyuha_warnings GTest::gtest_main)
  gtest_discover_tests(vyuha_rpc_tests)
endif()
