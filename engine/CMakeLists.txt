cmake_minimum_required(VERSION 3.28)
project(vyuha_engine VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++20 modules: skip dependency scanning (faster builds; GCC scan flags also break clang-tidy).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # core is also linked into the Python extension

option(VYUHA_BUILD_TESTS "Build GoogleTest tests and Google Benchmark" ON)
option(VYUHA_BUILD_PYTHON "Build the nanobind module vyuha_core (off in sanitizer presets)" ON)
option(VYUHA_FUZZ_ONLY "Build only the libFuzzer targets (fuzz preset; no vcpkg)" OFF)
set(VYUHA_SANITIZER "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

# Strict warnings for our own targets only (never for vcpkg dependencies).
add_library(vyuha_warnings INTERFACE)
target_compile_options(vyuha_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

if(VYUHA_SANITIZER)
  add_compile_options(-fsanitize=${VYUHA_SANITIZER} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${VYUHA_SANITIZER})
endif()

if(VYUHA_FUZZ_ONLY)
  add_subdirectory(fuzz)
  return()
endif()

find_package(quill CONFIG REQUIRED)
find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(cnats CONFIG REQUIRED)

add_library(vyuha_core_lib
  src/core/core_loop.cc
  src/core/price.cc
  src/core/version.cc
  src/core/slew_check.cc
  src/core/wall_clock.cc
  src/md/bar_builder.cc
  src/md/last_value.cc
  src/util/log.cc
  src/util/net.cc
  src/util/secret.cc
)
add_library(vyuha::core ALIAS vyuha_core_lib)
target_include_directories(vyuha_core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vyuha_core_lib PUBLIC quill::quill PRIVATE vyuha_warnings)

# Kite Connect adapter (market data now; orders in M3). Kept out of core so core stays Boost-free.
# Broker-neutral WebSocket feed runner shared by the adapters.
add_library(vyuha_feed STATIC src/broker/common/feed_client.cc)
target_link_libraries(vyuha_feed
  PUBLIC vyuha::core Boost::asio Boost::beast OpenSSL::SSL OpenSSL::Crypto
  PRIVATE vyuha_warnings)
target_compile_definitions(vyuha_feed PUBLIC BOOST_ASIO_NO_DEPRECATED)

add_library(vyuha_kite
  src/broker/kite/ticker.cc
  src/broker/kite/ticker_parser.cc
  src/broker/kite/ticker_shards.cc
)
add_library(vyuha::kite ALIAS vyuha_kite)

# Upstox adapter (market data now; orders in M3).
add_library(vyuha_upstox
  src/broker/upstox/feed.cc
  src/broker/upstox/feed_parser.cc
  src/broker/upstox/subscriptions.cc
)
add_library(vyuha::upstox ALIAS vyuha_upstox)

# Order state machine and router.
add_library(vyuha_oms STATIC src/oms/oms.cc src/oms/order_router.cc)
add_library(vyuha::oms ALIAS vyuha_oms)
target_link_libraries(vyuha_oms PUBLIC vyuha::core PRIVATE vyuha_warnings)

# The pre-trade RiskGate (RISK_ENGINE §3). ApprovedOrder is header-only, so the router can take one without
# linking the gate.
add_library(vyuha_risk STATIC src/risk/risk_gate.cc src/risk/rules.cc src/risk/scope_book.cc
            src/risk/scope_rules.cc)
add_library(vyuha::risk ALIAS vyuha_risk)
target_link_libraries(vyuha_risk PUBLIC vyuha::oms PRIVATE vyuha_warnings)

# Market-data bus: protobuf encoders + NATS publisher (md.tick.*, md.bar.*, KV md_last).
add_library(vyuha_bus src/bus/md_encode.cc src/bus/md_publisher.cc src/bus/system_alert_publisher.cc)
add_library(vyuha::bus ALIAS vyuha_bus)
target_link_libraries(vyuha_bus PUBLIC vyuha::core PRIVATE vyuha_warnings cnats::nats_static)

# Broker session control plane (gRPC handlers call into it).
add_library(vyuha_session src/session/session_manager.cc)
add_library(vyuha::session ALIAS vyuha_session)
target_link_libraries(vyuha_session PUBLIC vyuha::core vyuha::kite vyuha::upstox PRIVATE vyuha_warnings)
target_link_libraries(vyuha_upstox
  PUBLIC vyuha::core
  PRIVATE vyuha_warnings vyuha_feed simdjson::simdjson)
# Non-deprecated Asio: synchronous ops taking an error_code return void (nothing to ignore).
target_compile_definitions(vyuha_kite PUBLIC BOOST_ASIO_NO_DEPRECATED)
target_link_libraries(vyuha_kite
  PUBLIC vyuha::core
  PRIVATE vyuha_warnings vyuha_feed simdjson::simdjson)

find_package(libpqxx CONFIG REQUIRED)
# vcpkg's PostgreSQL wrapper links libpq's static helpers (pgcommon, pgport) per configuration, from
# lib/ for non-Debug and debug/lib/ for Debug. Release-only triplets (CI: x64-linux-release) have no
# debug/lib, so Debug builds got neither and failed to link. Use the release helpers there; a no-op
# with the default triplets. Order matters for GNU ld: libpq → pgcommon → pgport.
if(TARGET PostgreSQL::PostgreSQL AND PostgreSQL_pgcommon_LIBRARY_RELEASE AND NOT PostgreSQL_pgcommon_LIBRARY_DEBUG)
  set_property(TARGET PostgreSQL::PostgreSQL APPEND PROPERTY INTERFACE_LINK_LIBRARIES
               "$<$<CONFIG:DEBUG>:${PostgreSQL_pgcommon_LIBRARY_RELEASE}>"
               "$<$<CONFIG:DEBUG>:${PostgreSQL_pgport_LIBRARY_RELEASE}>")
endif()
# The order event writer and recovery: in every preset, so the nightly sanitizers cover the writer thread.
add_library(vyuha_oms_pg STATIC src/oms/pg_order_store.cc)
target_link_libraries(vyuha_oms_pg PUBLIC vyuha::oms PRIVATE vyuha_warnings libpqxx::pqxx)

# gRPC control API (engine.v1.EngineService) and the engine binary. Built without sanitizers only:
# vcpkg's gRPC/protobuf/abseil aren't instrumented, and abseil's layouts change under ASan. The
# logic they call (SessionManager, feeds, core) is tested in every preset.
if(NOT VYUHA_SANITIZER)
  find_package(Protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)
  set(VYUHA_PROTO_ROOT ${PROJECT_SOURCE_DIR}/../proto)
  add_library(vyuha_engine_proto STATIC
    ${VYUHA_PROTO_ROOT}/vyuha/common/v1/common.proto
    ${VYUHA_PROTO_ROOT}/vyuha/marketdata/v1/marketdata.proto
    ${VYUHA_PROTO_ROOT}/vyuha/orders/v1/orders.proto
    ${VYUHA_PROTO_ROOT}/vyuha/risk/v1/risk.proto
    ${VYUHA_PROTO_ROOT}/vyuha/engine/v1/engine.proto)
  target_link_libraries(vyuha_engine_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
  set(VYUHA_PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/gen)
  target_include_directories(vyuha_engine_proto PUBLIC ${VYUHA_PROTO_OUT})
  protobuf_generate(TARGET vyuha_engine_proto LANGUAGE cpp IMPORT_DIRS ${VYUHA_PROTO_ROOT}
                    PROTOC_OUT_DIR ${VYUHA_PROTO_OUT})
  protobuf_generate(TARGET vyuha_engine_proto LANGUAGE grpc IMPORT_DIRS ${VYUHA_PROTO_ROOT}
                    PROTOC_OUT_DIR ${VYUHA_PROTO_OUT}
                    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
                    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>")

  add_library(vyuha_pg STATIC src/session/pg_instrument_directory.cc)
  target_link_libraries(vyuha_pg PUBLIC vyuha::session PRIVATE vyuha_warnings libpqxx::pqxx)

  add_library(vyuha_rpc STATIC src/rpc/engine_service.cc)
  target_link_libraries(vyuha_rpc PUBLIC vyuha::session vyuha_engine_proto PRIVATE vyuha_warnings)

  add_executable(vyuha_engine src/app/main.cc)
  target_link_libraries(vyuha_engine PRIVATE vyuha_rpc vyuha_pg vyuha::bus vyuha_warnings)
endif()

if(VYUHA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(bench)
endif()

if(VYUHA_BUILD_PYTHON)
  add_subdirectory(bindings/python)
endif()
