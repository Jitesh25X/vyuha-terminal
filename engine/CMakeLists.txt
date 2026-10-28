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
set(VYUHA_SANITIZER "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

# Strict warnings for our own targets only (never for vcpkg dependencies).
add_library(vyuha_warnings INTERFACE)
target_compile_options(vyuha_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

if(VYUHA_SANITIZER)
  add_compile_options(-fsanitize=${VYUHA_SANITIZER} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${VYUHA_SANITIZER})
endif()

add_library(vyuha_core_lib
  src/core/version.cc
)
add_library(vyuha::core ALIAS vyuha_core_lib)
target_include_directories(vyuha_core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vyuha_core_lib PRIVATE vyuha_warnings)

if(VYUHA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(bench)
endif()

if(VYUHA_BUILD_PYTHON)
  add_subdirectory(bindings/python)
endif()
