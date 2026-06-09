#!/usr/bin/env bash
# format-cpp.sh — run clang-format 21 across the cpp_migration tree
#
# Pins formatter to clang-format 21.x to keep macOS / Linux output identical.
# Resolves the binary in this order:
#   1. CLANG_FORMAT env override
#   2. clang-format-21 on PATH (Linux)
#   3. /opt/homebrew/bin/clang-format (macOS / Homebrew, currently 21.x)
#   4. clang-format on PATH — only accepted if version starts with "21."
#
# Usage:
#   scripts/format-cpp.sh           # rewrite files in place
#   scripts/format-cpp.sh --check   # dry-run, exit non-zero on diff (for CI)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

REQUIRED_MAJOR="21"

resolve_clang_format() {
    if [[ -n "${CLANG_FORMAT:-}" ]]; then
        echo "${CLANG_FORMAT}"
        return
    fi
    if command -v clang-format-21 >/dev/null 2>&1; then
        command -v clang-format-21
        return
    fi
    if [[ -x "/opt/homebrew/bin/clang-format" ]]; then
        echo "/opt/homebrew/bin/clang-format"
        return
    fi
    if command -v clang-format >/dev/null 2>&1; then
        command -v clang-format
        return
    fi
    echo ""
}

CLANG_FMT="$(resolve_clang_format)"
if [[ -z "${CLANG_FMT}" ]]; then
    echo "error: clang-format not found. Install LLVM ${REQUIRED_MAJOR} (clang-format-${REQUIRED_MAJOR})." >&2
    exit 127
fi

VERSION_LINE="$("${CLANG_FMT}" --version 2>&1 | head -1)"
if ! echo "${VERSION_LINE}" | grep -qE "version ${REQUIRED_MAJOR}\."; then
    echo "error: unsupported clang-format version." >&2
    echo "       found: ${VERSION_LINE}" >&2
    echo "       expected: clang-format ${REQUIRED_MAJOR}.x" >&2
    echo "       override with CLANG_FORMAT=/path/to/clang-format-${REQUIRED_MAJOR}" >&2
    exit 1
fi

mode="write"
for arg in "$@"; do
    case "${arg}" in
        --check|-n) mode="check" ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *) echo "unknown argument: ${arg}" >&2; exit 2 ;;
    esac
done

mapfile -d '' files < <(find "${ROOT_DIR}/src" "${ROOT_DIR}/tests" \
    \( -name '*.cppm' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
    -not -path '*/build/*' -not -path '*/install/*' -print0 2>/dev/null)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no C++ sources found under ${ROOT_DIR}/{src,tests}" >&2
    exit 0
fi

echo "[format-cpp] using ${CLANG_FMT}"
echo "[format-cpp] ${VERSION_LINE}"
echo "[format-cpp] mode=${mode}, files=${#files[@]}"

if [[ "${mode}" == "check" ]]; then
    "${CLANG_FMT}" --dry-run --Werror "${files[@]}"
else
    "${CLANG_FMT}" -i "${files[@]}"
fi
