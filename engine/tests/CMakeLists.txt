find_package(Threads REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(rapidcheck CONFIG REQUIRED)
include(GoogleTest)

add_executable(vyuha_tests
  bar_builder_test.cc
  last_value_test.cc
  core_loop_test.cc
  kite_parser_test.cc
  kite_ticker_test.cc
  md_publisher_test.cc
  order_router_test.cc
  order_state_test.cc
  price_test.cc
  risk_gate_test.cc
  session_manager_test.cc
  upstox_feed_test.cc
  upstox_parser_test.cc
  spsc_ring_test.cc
  version_test.cc
)
# RapidCheck's headers use std::aligned_storage, deprecated in C++23.
set_source_files_properties(order_state_test.cc risk_gate_test.cc PROPERTIES COMPILE_OPTIONS -Wno-deprecated-declarations)
target_link_libraries(vyuha_tests PRIVATE vyuha::core vyuha::oms vyuha::risk rapidcheck vyuha::kite vyuha::upstox vyuha::session vyuha::bus cnats::nats_static Boost::beast vyuha_warnings Threads::Threads GTest::gtest_main)
# Discovery runs the fresh binary to list tests; macOS can take >5 s (the default) to first-launch
# a newly linked binary on a busy machine.
gtest_discover_tests(vyuha_tests DISCOVERY_TIMEOUT 60)
target_compile_definitions(vyuha_tests PRIVATE VYUHA_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")

# Order event writer and recovery on a real Postgres, in every preset (needs VYUHA_TEST_POSTGRES_URL).
add_executable(vyuha_oms_crash_child oms_crash_child.cc)
target_link_libraries(vyuha_oms_crash_child PRIVATE vyuha_oms_pg vyuha::risk vyuha_warnings Threads::Threads)
add_executable(vyuha_pg_tests pg_order_store_test.cc oms_crash_test.cc outage_signal_test.cc)
target_link_libraries(vyuha_pg_tests PRIVATE vyuha_oms_pg vyuha::risk vyuha::bus cnats::nats_static libpqxx::pqxx vyuha_warnings Threads::Threads GTest::gtest_main)
target_compile_definitions(vyuha_pg_tests PRIVATE VYUHA_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/../db/migrations"
                                                  VYUHA_OMS_CRASH_CHILD="$<TARGET_FILE:vyuha_oms_crash_child>")
add_dependencies(vyuha_pg_tests vyuha_oms_crash_child)
gtest_discover_tests(vyuha_pg_tests DISCOVERY_TIMEOUT 60)

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

  add_executable(vyuha_reference_tests upstox_reference_test.cc md_encode_reference_test.cc)
  target_link_libraries(vyuha_reference_tests PRIVATE vyuha::upstox vyuha::bus upstox_reference_proto
                                                      vyuha_engine_proto vyuha_warnings GTest::gtest_main)
  target_compile_definitions(vyuha_reference_tests PRIVATE VYUHA_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
  gtest_discover_tests(vyuha_reference_tests DISCOVERY_TIMEOUT 60)

  # gRPC adapter end to end (in-process server + client stub).
  add_executable(vyuha_rpc_tests engine_service_test.cc pg_instrument_directory_test.cc)
  target_link_libraries(vyuha_rpc_tests PRIVATE vyuha_rpc vyuha_pg libpqxx::pqxx vyuha_warnings GTest::gtest_main)
  target_compile_definitions(vyuha_rpc_tests PRIVATE VYUHA_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/../db/migrations")
  gtest_discover_tests(vyuha_rpc_tests DISCOVERY_TIMEOUT 60)
endif()

# Invariant 1 at compile time (M2.2): each forbidden case in compile_fail.cc must fail to compile, and the
# control (no case) must compile, so a case fails only because of its own line. Syntax only: nothing links.
set(VYUHA_COMPILE_FAIL_CMD ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -I${PROJECT_SOURCE_DIR}/include)
if(CMAKE_OSX_SYSROOT)
  list(APPEND VYUHA_COMPILE_FAIL_CMD -isysroot ${CMAKE_OSX_SYSROOT})
endif()
set(VYUHA_COMPILE_FAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail.cc)
add_test(NAME compile_fail.control COMMAND ${VYUHA_COMPILE_FAIL_CMD} ${VYUHA_COMPILE_FAIL_SRC})
# Each case must fail with the error it's about (clang or GCC wording), not any error.
set(VYUHA_COMPILE_FAIL_CASES
    "CONSTRUCT=private" "BRACE=private" "DEFAULT=deleted" "COPY=deleted" "DERIVE=final"
    "SEND_ORDER=conver" "SEND_DETAILS=conver" "SUBMIT_DETAILS=conver"
    "SUBMIT_WITHOUT_APPROVAL=too few arguments|no matching" "BITCAST=bit_cast|trivially"
    "TEMP_CLOCK=deleted" "PAPER_MISSING=deleted|default constructor|no matching constructor"
    "PAPER_FROM_BOOL=explicit|conver|no matching")
foreach(entry IN LISTS VYUHA_COMPILE_FAIL_CASES)
  string(REPLACE "=" ";" parts "${entry}")
  list(GET parts 0 case)
  list(GET parts 1 expected)
  add_test(NAME compile_fail.${case}
           COMMAND ${VYUHA_COMPILE_FAIL_CMD} -DVYUHA_CASE_${case} ${VYUHA_COMPILE_FAIL_SRC})
  set_tests_properties(compile_fail.${case} PROPERTIES PASS_REGULAR_EXPRESSION "error:[^\n]*(${expected})")
endforeach()
