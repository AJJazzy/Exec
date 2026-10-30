#!/usr/bin/env bash
# Generates build/xcode/Nexus.xcodeproj (Apple Silicon + Intel universal by default), builds it and
# runs the tests. Open the project with: open build/xcode/Nexus.xcodeproj
#   scripts/build/xcode.sh [Debug|Release] [--no-build]
set -euo pipefail
cd "$(dirname "$0")/../.."
CONFIG="${1:-Release}"
ARCHS="${NEXUS_ARCHS:-arm64;x86_64}"
cmake -S . -B build/xcode -G Xcode -DCMAKE_OSX_ARCHITECTURES="$ARCHS"
if [[ "${2:-}" == "--no-build" ]]; then
  echo "Generated build/xcode/Nexus.xcodeproj"
  exit 0
fi
cmake --build build/xcode --config "$CONFIG" -- -quiet
ctest --test-dir build/xcode -C "$CONFIG" --output-on-failure
echo "App: build/xcode/bin/$CONFIG/Nexus.app"
