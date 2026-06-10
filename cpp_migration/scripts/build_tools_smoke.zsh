#!/usr/bin/env zsh
# Build tools_smoke.cpp manually by re-using the C++23 module units from the
# main clang-debug build.  This script exists because `tools_smoke` depends
# only on the two impl_* modules and the bare STL — linking it through
# cc_tools transitively pulls cc_services (sse_client et al.) which can be
# independently broken in partial builds.
set -euo pipefail

ROOT="/Users/bytedance/Develop/CC-REPL/cpp_migration"
BUILD="${ROOT}/build/clang-debug"
SRC="${ROOT}/src"
TESTS="${ROOT}/tests"
BIN="${BUILD}/tools_smoke"
PCM_DIR="${BUILD}/tools_smoke_pcms"
CXX="/opt/homebrew/opt/llvm/bin/clang++"

mkdir -p "${PCM_DIR}"

STD_MODULE_ARGS=""
# Detect build-dir std module if present (used by some CMake configurations);
# fall back to -std=c++23 without standard library modules.
if ls "${BUILD}"/*stdc++*.pcm 1>/dev/null 2>&1; then
  echo "Note: std library modules present"
fi

COMMON=(
  -std=c++23
  -arch arm64
  -isysroot "$(xcrun --sdk macosx --show-sdk-path)"
  -fmodules
  -fcxx-modules
  -fimplicit-module-maps
  -fprebuilt-module-path="${PCM_DIR}"
  -I"${ROOT}/build/clang-debug/_deps/yyjson-src/src"
  -I"${ROOT}/build/clang-debug/_deps/libuv-src/include"
  -O0 -g
  -Wall
)

echo "--- Precompiling impl_bash.cppm ---"
"${CXX}" "${COMMON[@]}" \
  --precompile "${SRC}/tools/bash/impl_bash.cppm" \
  -o "${PCM_DIR}/cc.tools.bash.impl.pcm"

echo "--- Precompiling impl_files.cppm ---"
"${CXX}" "${COMMON[@]}" \
  -fprebuilt-module-path="${PCM_DIR}" \
  --precompile "${SRC}/tools/files/impl_files.cppm" \
  -o "${PCM_DIR}/cc.tools.files.impl.pcm"

echo "--- Compiling tools_smoke.cpp + linking ---"
"${CXX}" "${COMMON[@]}" \
  -fprebuilt-module-path="${PCM_DIR}" \
  "${PCM_DIR}/cc.tools.bash.impl.pcm" \
  "${PCM_DIR}/cc.tools.files.impl.pcm" \
  "${TESTS}/tools_smoke.cpp" \
  -o "${BIN}"

echo "Built: ${BIN}"
ls -lh "${BIN}"
