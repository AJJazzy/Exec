#!/usr/bin/env bash
# Builds the core and tests with AddressSanitizer + UndefinedBehaviorSanitizer, then with
# ThreadSanitizer, and runs the suites. Requires a compiler with sanitizer runtimes installed.
#   CXX=g++ CC=gcc scripts/build/sanitizers.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
common=(-G Ninja -DCMAKE_BUILD_TYPE=Debug -DNEXUS_BUILD_APP=OFF)
cmake -S . -B build/asan "${common[@]}" \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build build/asan
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/asan --output-on-failure
cmake -S . -B build/tsan "${common[@]}" -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_C_FLAGS=-fsanitize=thread \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
cmake --build build/tsan
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/tsan --output-on-failure -E test_performance
