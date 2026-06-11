#!/usr/bin/env bash
# Generate code coverage report using the clang-coverage preset.
# Requires: Homebrew LLVM (llvm-profdata, llvm-cov)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/clang-coverage"
LLVM_BIN="/opt/homebrew/opt/llvm/bin"
PROFDATA_TOOL="${LLVM_BIN}/llvm-profdata"
COV_TOOL="${LLVM_BIN}/llvm-cov"
PROFILE_DIR="${BUILD_DIR}/profiles"
MERGED_PROF="${BUILD_DIR}/coverage.profdata"
REPORT_DIR="${BUILD_DIR}/coverage-report"

# 1. Configure and build with coverage preset
echo "=== Configuring with clang-coverage preset ==="
cmake --preset clang-coverage -S "${PROJECT_ROOT}"

echo "=== Building ==="
cmake --build "${BUILD_DIR}" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# 2. Run tests with profile output
echo "=== Running tests ==="
rm -rf "${PROFILE_DIR}"
mkdir -p "${PROFILE_DIR}"
export LLVM_PROFILE_FILE="${PROFILE_DIR}/test-%p-%m.profraw"
ctest --test-dir "${BUILD_DIR}" -j4 --output-on-failure || true

# 3. Merge raw profiles
echo "=== Merging raw profiles ==="
RAW_PROFILES=$(find "${PROFILE_DIR}" -name '*.profraw' 2>/dev/null)
if [ -z "${RAW_PROFILES}" ]; then
    echo "ERROR: No .profraw files found in ${PROFILE_DIR}" >&2
    exit 1
fi
"${PROFDATA_TOOL}" merge -sparse ${RAW_PROFILES} -o "${MERGED_PROF}"

# 4. Collect test binaries for coverage object list
# Find all executables in the build directory that are test binaries
OBJECTS=()
while IFS= read -r bin; do
    OBJECTS+=("-object" "${bin}")
done < <(find "${BUILD_DIR}" -type f -perm +111 -name 'test_*' 2>/dev/null; \
         find "${BUILD_DIR}/bin" -type f -perm +111 2>/dev/null || true)

if [ ${#OBJECTS[@]} -eq 0 ]; then
    echo "ERROR: No test binaries found for coverage reporting" >&2
    exit 1
fi

# 5. Generate HTML report
echo "=== Generating HTML coverage report ==="
rm -rf "${REPORT_DIR}"
mkdir -p "${REPORT_DIR}"
"${COV_TOOL}" show \
    "${OBJECTS[@]}" \
    -instr-profile="${MERGED_PROF}" \
    -format=html \
    -output-dir="${REPORT_DIR}" \
    -show-line-counts-or-regions \
    -show-instantiations=false \
    -ignore-filename-regex='(build/|third_party/|_deps/|googletest)'

echo ""
echo "=== Coverage Summary ==="
"${COV_TOOL}" report \
    "${OBJECTS[@]}" \
    -instr-profile="${MERGED_PROF}" \
    -ignore-filename-regex='(build/|third_party/|_deps/|googletest)'

echo ""
echo "HTML report: ${REPORT_DIR}/index.html"
