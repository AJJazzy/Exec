#!/usr/bin/env bash
# Configure, build and test Nexus with Ninja (macOS or Linux).
#   scripts/build/build.sh [Debug|Release] [extra cmake args...]
# On macOS this also builds Nexus.app (Ninja + Swift). Use scripts/build/xcode.sh for the Xcode project.
set -euo pipefail
cd "$(dirname "$0")/../.."
CONFIG="${1:-Release}"
shift || true
BUILD_DIR="build/$(echo "$CONFIG" | tr "[:upper:]" "[:lower:]")"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "$@"
cmake --build "$BUILD_DIR"
ctest --test-dir "$BUILD_DIR" --output-on-failure
echo "Build output: $BUILD_DIR/bin"
