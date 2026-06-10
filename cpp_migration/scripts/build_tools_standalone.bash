#!/usr/bin/env zsh
# Precompile impl_bash/impl_files modules and build the two standalone binaries (tools_smoke, tools_e2e)
# without going through cc_tools (which transitively pulls cc_services which can
# have independently broken module scans in partial builds).
#
# Usage:
#   build_standalone.bash   # defaults to building everything
#   build_standalone.bash smoke
#   build_standalone.bash e2e
set -euo pipefail

ROOT="/Users/bytedance/Develop/CC-REPL/cpp_migration"
BUILD="${ROOT}/build/clang-debug"
SRC="${ROOT}/src"
TESTS="${ROOT}/tests"
PCM_DIR="${BUILD}/tools_smoke_pcms"
CXX="/opt/homebrew/opt/llvm/bin/clang++"
SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"

mkdir -p "${PCM_DIR}"

COMMON=(
  -std=c++23
  -arch arm64
  -isysroot "${SDKROOT}"
  -fmodules
  -fcxx-modules
  -fimplicit-module-maps
  -fprebuilt-module-path="${PCM_DIR}"
  -I"${ROOT}/build/clang-debug/_deps/yyjson-src/src"
  -I"${ROOT}/build/clang-debug/_deps/libuv-src/include"
  -O0 -g
  -Wall
)

precompile_modules() {
  echo "--- Precompiling impl_bash.cppm ---"
  "${CXX}" "${COMMON[@]}" \
    --precompile "${SRC}/tools/bash/impl_bash.cppm" \
    -o "${PCM_DIR}/cc.tools.bash.impl.pcm"

  echo "--- Precompiling impl_files.cppm ---"
  "${CXX}" "${COMMON[@]}" \
    -fprebuilt-module-path="${PCM_DIR}" \
    --precompile "${SRC}/tools/files/impl_files.cppm" \
    -o "${PCM_DIR}/cc.tools.files.impl.pcm"
}

build_target() {
  local name="$1"
  local src="$2"
  local bin="${BUILD}/${name}"
  echo "--- Building ${name} ---"
  "${CXX}" "${COMMON[@]}" \
    -fprebuilt-module-path="${PCM_DIR}" \
    "${PCM_DIR}/cc.tools.bash.impl.pcm" \
    "${PCM_DIR}/cc.tools.files.impl.pcm" \
    "${src}" \
    -o "${bin}"
  echo "Built: ${bin} ($(stat -f %z "${bin}") bytes)"
}

main() {
  precompile_modules
  local what="${1:-all}"
  case "${what}" in
    smoke)   build_target tools_smoke "${TESTS}/tools_smoke.cpp" ;;
    e2e)     build_target tools_e2e   "${TESTS}/tools_e2e.cpp"   ;;
    all|*)
      build_target tools_smoke "${TESTS}/tools_smoke.cpp"
      build_target tools_e2e   "${TESTS}/tools_e2e.cpp"
      ;;
  esac
}

main "$@"
