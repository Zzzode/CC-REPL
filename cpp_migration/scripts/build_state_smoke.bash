#!/usr/bin/env zsh
# Standalone build script for tests/state_smoke.cpp (Phase 3-F AppState validator).
#
# We avoid going through `cc_state` via CMake in this script only because
# `cc_state` -> `cc_utils` -> `cc_services` transitively pulls in the
# services/api/sse_client.ddi module-scan failure which can break unrelated
# builds.  Instead, we precompile *only* the two modules we actually import
# (cc.state.store_impl, cc.state.persistence_json) and link the test binary
# against the system runtime + yyjson.
set -euo pipefail

ROOT="/Users/bytedance/Develop/CC-REPL/cpp_migration"
BUILD="${ROOT}/build/clang-debug"
SRC="${ROOT}/src"
TESTS="${ROOT}/tests"
PCM_DIR="${BUILD}/state_smoke_pcms"
CXX="/opt/homebrew/opt/llvm/bin/clang++"
SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
YYJSON_DIR="${BUILD}/_deps/yyjson-src/src"

mkdir -p "${PCM_DIR}"

COMMON=(
  -std=c++23
  -arch arm64
  -isysroot "${SDKROOT}"
  -fmodules
  -fcxx-modules
  -fimplicit-module-maps
  -fprebuilt-module-path="${PCM_DIR}"
  -I"${YYJSON_DIR}"
  -O0 -g
  -Wall
)

echo "--- Precompiling store_impl.cppm ---"
"${CXX}" "${COMMON[@]}" \
  --precompile "${SRC}/state/store_impl.cppm" \
  -o "${PCM_DIR}/cc.state.store_impl.pcm"

echo "--- Precompiling persistence_json.cppm (imports store_impl) ---"
"${CXX}" "${COMMON[@]}" \
  --precompile "${SRC}/state/persistence_json.cppm" \
  -o "${PCM_DIR}/cc.state.persistence_json.pcm"

echo "--- Compiling state_smoke.cpp ---"
BIN="${BUILD}/state_smoke"
"${CXX}" "${COMMON[@]}" \
  -fprebuilt-module-path="${PCM_DIR}" \
  "${PCM_DIR}/cc.state.store_impl.pcm" \
  "${PCM_DIR}/cc.state.persistence_json.pcm" \
  "${TESTS}/state_smoke.cpp" \
  "${YYJSON_DIR}/yyjson.c" \
  -o "${BIN}"

echo "Built: ${BIN} ($(stat -f %z "${BIN}") bytes)"
