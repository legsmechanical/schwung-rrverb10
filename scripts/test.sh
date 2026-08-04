#!/usr/bin/env bash
# Native (host-arch) unit tests. No device, no cross-compiler needed.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"
mkdir -p build
CXX="${CXX:-c++}"
"$CXX" -O2 -std=c++17 -Isrc/dsp \
    tests/test_rrverb10.cpp src/dsp/BossEmu.cpp \
    -o build/test_rrverb10
cd build && ./test_rrverb10
