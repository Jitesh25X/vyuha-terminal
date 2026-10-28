# libFuzzer targets: only the parsers, built with no third-party deps so the fuzz preset needs no
# vcpkg. Requires clang (Homebrew llvm@18 on macOS; clang-18 on Ubuntu).
add_executable(kite_ticker_fuzz
  kite_ticker_fuzz.cc
  ${PROJECT_SOURCE_DIR}/src/broker/kite/ticker_parser.cc
  ${PROJECT_SOURCE_DIR}/src/core/price.cc
)
target_include_directories(kite_ticker_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/include)
# ASan+libFuzzer from Homebrew LLVM 18 hangs at startup on current macOS, so macOS fuzzes with UBSan
# plus standard-library bounds checks; Linux (CI) adds ASan. See Makefile FUZZ_SANITIZERS.
set(VYUHA_FUZZ_SANITIZERS "fuzzer,address,undefined" CACHE STRING "-fsanitize list for fuzz targets")
target_compile_options(kite_ticker_fuzz PRIVATE -fsanitize=${VYUHA_FUZZ_SANITIZERS} -fno-sanitize-recover=all -g -O1)
target_compile_definitions(kite_ticker_fuzz PRIVATE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG
                                                    _GLIBCXX_ASSERTIONS)
target_link_options(kite_ticker_fuzz PRIVATE -fsanitize=${VYUHA_FUZZ_SANITIZERS})
target_link_libraries(kite_ticker_fuzz PRIVATE vyuha_warnings)

add_executable(upstox_feed_fuzz
  upstox_feed_fuzz.cc
  ${PROJECT_SOURCE_DIR}/src/broker/upstox/feed_parser.cc
  ${PROJECT_SOURCE_DIR}/src/core/price.cc
)
target_include_directories(upstox_feed_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(upstox_feed_fuzz PRIVATE -fsanitize=${VYUHA_FUZZ_SANITIZERS} -fno-sanitize-recover=all -g -O1)
target_compile_definitions(upstox_feed_fuzz PRIVATE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG
                                                    _GLIBCXX_ASSERTIONS)
target_link_options(upstox_feed_fuzz PRIVATE -fsanitize=${VYUHA_FUZZ_SANITIZERS})
target_link_libraries(upstox_feed_fuzz PRIVATE vyuha_warnings)
