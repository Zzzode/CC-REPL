#!/usr/bin/env python3
"""
Scans .cppm files under src/ that are missing #include lines between
`module;` and `export module`, detects standard library usage in the
file body, and inserts the appropriate #include directives.
"""

import os
import re
from pathlib import Path

# Mapping: (regex_pattern, set_of_headers_to_add)
# Order doesn't matter since we collect into a set and sort at the end.
USAGE_TO_HEADERS: list[tuple[re.Pattern, list[str]]] = []

def _add(patterns: list[str], headers: list[str]):
    for p in patterns:
        USAGE_TO_HEADERS.append((re.compile(p), headers))

# --- Build the mapping ---
_add([r'\bstd::string\b', r'\bstd::to_string\b'], ['<string>'])
_add([r'\bstd::string_view\b'], ['<string_view>'])
_add([r'\bstd::vector\b'], ['<vector>'])
_add([r'\bstd::(?:multi)?map\b'], ['<map>'])
_add([r'\bstd::unordered_map\b'], ['<unordered_map>'])
_add([r'\bstd::(?:multi)?set\b'], ['<set>'])
_add([r'\bstd::unordered_set\b'], ['<unordered_set>'])
_add([r'\bstd::optional\b'], ['<optional>'])
_add([r'\bstd::variant\b'], ['<variant>'])
_add([r'\bstd::expected\b'], ['<expected>'])
_add([r'\bstd::span\b'], ['<span>'])
_add([r'\bstd::array\b'], ['<array>'])
_add([r'\bstd::(?:tuple|pair|make_pair)\b'], ['<tuple>'])
_add([r'\bstd::function\b'], ['<functional>'])
_add([r'\bstd::(?:unique_ptr|shared_ptr|make_unique|make_shared)\b'], ['<memory>'])
_add([r'\bstd::(?:move|forward)\b'], ['<utility>'])
_add([r'\bstd::filesystem\b', r'\bfs::'], ['<filesystem>'])
_add([r'\bstd::chrono\b'], ['<chrono>'])
_add([r'\bstd::thread\b'], ['<thread>'])
_add([r'\bstd::(?:mutex|lock_guard|unique_lock)\b'], ['<mutex>'])
_add([r'\bstd::atomic\b'], ['<atomic>'])
_add([r'\bstd::(?:future|promise|async)\b'], ['<future>'])
_add([r'\bstd::(?:cout|cerr|endl)\b'], ['<iostream>'])
_add([r'\bstd::(?:ostringstream|istringstream|stringstream)\b'], ['<sstream>'])
_add([r'\bstd::(?:ifstream|ofstream|fstream)\b'], ['<fstream>'])
_add([r'\bstd::istreambuf_iterator\b'], ['<iterator>'])
_add([r'\bstd::format\b'], ['<format>'])
_add([r'\bstd::regex\b'], ['<regex>'])
_add([r'\bstd::(?:sort|find|transform|any_of|all_of|none_of|copy|remove_if|max|min|clamp)\b'], ['<algorithm>'])
_add([r'\bstd::numeric_limits\b'], ['<limits>'])
_add([r'\bstd::(?:setw|setprecision|setfill)\b'], ['<iomanip>'])
_add([r'\b(?:size_t|uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t)\b'], ['<cstdint>', '<cstddef>'])
_add([r'\bstd::(?:exception|runtime_error|invalid_argument|logic_error)\b'], ['<stdexcept>'])
_add([r'\bstd::ranges\b'], ['<ranges>'])
_add([r'\bstd::source_location\b'], ['<source_location>'])
_add([r'\bstd::coroutine_handle\b', r'\bco_await\b', r'\bco_yield\b', r'\bco_return\b'], ['<coroutine>'])
_add([r'\bstd::(?:queue|priority_queue)\b'], ['<queue>'])
_add([r'\bstd::stack\b'], ['<stack>'])
_add([r'\bstd::deque\b'], ['<deque>'])
_add([r'\bstd::list\b'], ['<list>'])
_add([r'\bstd::bitset\b'], ['<bitset>'])
_add([r'\bstd::(?:accumulate|iota)\b'], ['<numeric>'])
_add([r'\bassert\s*\('], ['<cassert>'])
_add([r'\b(?:errno|strerror)\b'], ['<cerrno>', '<cstring>'])
_add([r'\bopen\s*\(', r'\bO_CREAT\b', r'\bO_RDONLY\b', r'\bO_WRONLY\b'], ['<fcntl.h>'])
_add([r'\b(?:write|read|close)\s*\('], ['<unistd.h>'])
_add([r'\bgetenv\b'], ['<cstdlib>'])
_add([r'\b(?:signal|SIGINT|SIGTERM)\b'], ['<csignal>'])
_add([r'\bstd::condition_variable\b'], ['<condition_variable>'])


def find_needed_headers(body: str) -> list[str]:
    """Scan file body and return sorted list of needed headers."""
    headers: set[str] = set()
    for pattern, hdrs in USAGE_TO_HEADERS:
        if pattern.search(body):
            headers.update(hdrs)
    return sorted(headers)


def is_missing_includes(content: str) -> tuple[bool, int, int]:
    """
    Check if a file has `module;` followed by `export module` with no #include in between.
    Returns (is_missing, module_line_idx, export_module_line_idx).
    """
    lines = content.splitlines()

    # Find `module;` line
    module_idx = -1
    for i, line in enumerate(lines):
        if line.strip() == 'module;':
            module_idx = i
            break

    if module_idx == -1:
        return False, -1, -1

    # Find `export module` line after `module;`
    export_idx = -1
    for i in range(module_idx + 1, len(lines)):
        stripped = lines[i].strip()
        if stripped.startswith('export module'):
            export_idx = i
            break
        # If we encounter a non-blank, non-comment line that isn't export module,
        # check if it's a #include - if so, file already has includes
        if stripped and not stripped.startswith('//') and not stripped.startswith('///'):
            if stripped.startswith('#include'):
                return False, -1, -1  # Already has includes
            # Some other directive - skip
            break

    if export_idx == -1:
        return False, -1, -1

    # Check there are no #include lines between module; and export module
    for i in range(module_idx + 1, export_idx):
        if lines[i].strip().startswith('#include'):
            return False, -1, -1

    return True, module_idx, export_idx


def process_file(filepath: Path) -> bool:
    """Process a single .cppm file. Returns True if file was modified."""
    content = filepath.read_text(encoding='utf-8')

    missing, module_idx, export_idx = is_missing_includes(content)
    if not missing:
        return False

    lines = content.splitlines()

    # Body is everything after the export module line
    body = '\n'.join(lines[export_idx:])

    headers = find_needed_headers(body)
    if not headers:
        return False

    # Build include block
    include_lines = [f'#include {h}' for h in headers]

    # Insert includes between module; and export module
    # Place them after module; line with a blank line before and after
    new_lines = (
        lines[:module_idx + 1]          # up to and including `module;`
        + ['']                           # blank line after module;
        + include_lines                  # #include lines
        + ['']                           # blank line before export module
        + lines[export_idx:]             # export module and rest
    )

    new_content = '\n'.join(new_lines) + '\n'
    filepath.write_text(new_content, encoding='utf-8')
    return True


def main():
    src_dir = Path(__file__).parent / 'src'
    if not src_dir.exists():
        print(f"ERROR: {src_dir} does not exist")
        return

    cppm_files = sorted(src_dir.rglob('*.cppm'))
    print(f"Found {len(cppm_files)} .cppm files to scan")

    modified = 0
    for f in cppm_files:
        if process_file(f):
            rel = f.relative_to(src_dir.parent)
            print(f"  Fixed: {rel}")
            modified += 1

    print(f"\nDone. Modified {modified} files.")


if __name__ == '__main__':
    main()
