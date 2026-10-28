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

add_library(vyuha_core_lib
  src/core/core_loop.cc
  src/core/price.cc
  src/core/version.cc
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
add_library(vyuha_kite
  src/broker/kite/ticker.cc
  src/broker/kite/ticker_parser.cc
  src/broker/kite/ticker_shards.cc
)
add_library(vyuha::kite ALIAS vyuha_kite)
# Non-deprecated Asio: synchronous ops taking an error_code return void (nothing to ignore).
target_compile_definitions(vyuha_kite PUBLIC BOOST_ASIO_NO_DEPRECATED)
target_link_libraries(vyuha_kite
  PUBLIC vyuha::core
  PRIVATE vyuha_warnings Boost::asio Boost::beast OpenSSL::SSL OpenSSL::Crypto simdjson::simdjson)

if(VYUHA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(bench)
endif()

if(VYUHA_BUILD_PYTHON)
  add_subdirectory(bindings/python)
endif()
