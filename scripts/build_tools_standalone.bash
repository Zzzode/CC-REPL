#!/usr/bin/env bash
# Precompile impl_bash/impl_files modules and build the two standalone binaries (tools_smoke, tools_e2e)
# without going through cc_tools (which transitively pulls cc_services which can
# have independently broken module scans in partial builds).
#
# Usage:
#   build_standalone.bash   # defaults to building everything
#   build_standalone.bash smoke
#   build_standalone.bash e2e
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${CC_TOOLS_STANDALONE_BUILD_DIR:-${ROOT}/build/debug}"
SRC="${ROOT}/src"
TESTS="${ROOT}/tests"
PCM_DIR="${BUILD}/tools_smoke_pcms"
CXX="${CXX:-/opt/homebrew/opt/llvm/bin/clang++}"

# The standalone units `import std;`. Point clang at the std module BMI that
# the project's cc_std target produced in the build tree.
STD_PCM="${STD_PCM:-${BUILD}/src/CMakeFiles/cc_std.dir/std.pcm}"
if [[ ! -f "${STD_PCM}" ]]; then
  echo "error: std module BMI not found at ${STD_PCM} (build cc_std first)" >&2
  exit 1
fi

mkdir -p "${PCM_DIR}"

COMMON=(
  -std=c++23
  -stdlib=libc++
  # Match the project's module mode. The legacy -fmodules/-fcxx-modules/
  # -fimplicit-module-maps combination rejects a TU that keeps textual std
  # headers in its GMF while `import std;`-ing; the project compiles all
  # standalone units with the reduced-BMI writer. impl_bash.cppm keeps its
  # environ accessor C-only precisely so it can `import std;`.
  -fmodules-reduced-bmi
  -fprebuilt-module-path="${PCM_DIR}"
  -fmodule-file=std="${STD_PCM}"
  -I"${BUILD}/_deps/yyjson-src/src"
  -I"${BUILD}/_deps/libuv-src/include"
  -O0 -g
  -Wall
)

if [[ "$(uname -s)" == "Darwin" ]]; then
  SDKROOT="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}"
  COMMON+=(-arch arm64 -isysroot "${SDKROOT}")
else
  # Linux: when clang is a Homebrew llvm using brew glibc/libc++, the linker
  # needs the libc++ and dynamic-loader paths explicitly (mirrors the
  # local-linux CMake preset). Derive them from the compiler location.
  LLVM_PREFIX="$(cd "$(dirname "${CXX}")/.." && pwd)"
  GLIBC_PREFIX="$(cd "${LLVM_PREFIX}/../glibc" 2>/dev/null && pwd || true)"
  if [[ -n "${GLIBC_PREFIX}" && -d "${GLIBC_PREFIX}/lib" ]]; then
    COMMON+=(
      -L"${LLVM_PREFIX}/lib" -L"${GLIBC_PREFIX}/lib"
      -Wl,-rpath,"${LLVM_PREFIX}/lib" -Wl,-rpath,"${GLIBC_PREFIX}/lib"
      -Wl,-dynamic-linker,"${GLIBC_PREFIX}/lib/ld-linux-x86-64.so.2"
      -Wl,--allow-shlib-undefined
    )
  fi
fi

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
  echo "Built: ${bin} ($(wc -c <"${bin}" | tr -d ' ') bytes)"
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
