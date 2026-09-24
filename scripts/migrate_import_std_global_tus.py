#!/usr/bin/env python3
"""Convert a NON-module translation unit (plain global-module .cpp, e.g.
tests and main.cpp) from textual pure-C++ std includes to `import std;`.

A global-module TU may `import std;` directly. It must NOT mix textual
pure-C++ libc++ headers with the named std module (clang 22 explodes in
that configuration: a golden test TU took >500s vs ~28s with import std).

  * removes `#include <H>` for pure C++ library headers (the std module set);
  * KEEPS C / POSIX / third-party (gtest, ftxui, ...) includes textually;
  * inserts `import std;` before the first `import cc...;` (else at EOF).

Idempotent. Applies only to files with NO module declaration.
"""
from __future__ import annotations

import glob
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PURE = set("""algorithm any array atomic barrier bit bitset charconv chrono
codecvt compare complex concepts condition_variable contract coroutine deque
exception execution expected filesystem flat_map flat_set format forward_list
fstream functional future generator hash_map hash_set initializer_list
iomanip ios iosfwd iostream istream iterator latch limits list locale map
mdspan memory memory_resource mutex new numbers numeric optional ostream print
queue random ranges ratio regex scoped_allocator semaphore set shared_future
shared_mutex source_location span sstream stack stacktrace stdexcept stop_token
streambuf string string_view strstream syncstream system_error thread tuple
typeindex type_traits typeinfo unordered_map unordered_set utility valarray
variant vector version""".split())

INC = re.compile(r'^([ \t]*#[ \t]*include[ \t]*<)([^>]+)>([^\n]*)$', re.M)
CC_IMPORT = re.compile(r"^\s*import\s+cc\.", re.M)
MODULE_DECL = re.compile(r'^\s*(?:export\s+)?module\s+[A-Za-z]')


def convert(path: pathlib.Path) -> bool:
    text = path.read_text(encoding="utf-8", errors="ignore")
    if MODULE_DECL.search(text, re.M) or "import std;" in text:
        return False
    if not CC_IMPORT.search(text, re.M):
        return False
    lines = text.splitlines(keepends=True)
    removed = 0
    out: list[str] = []
    for l in lines:
        m = INC.match(l)
        if m and m.group(2) in PURE:
            removed += 1
            continue
        out.append(l)
    if not removed:
        return False
    # insert before the first `import cc...;` in the filtered list
    idx = next((i for i, l in enumerate(out) if CC_IMPORT.match(l)), len(out))
    out.insert(idx, "import std;\n")
    path.write_text("".join(out))
    return True


def main():
    files = sorted(glob.glob(str(ROOT / "tests/**/*.cpp"), recursive=True))
    files += [str(ROOT / "src/main.cpp"),
              str(ROOT / "src/benchmarks/pare/pare_benchmark_main.cpp")]
    changed = 0
    for f in files:
        p = pathlib.Path(f)
        if p.exists() and convert(p):
            changed += 1
            print("converted", p.relative_to(ROOT))
    print("changed:", changed)


if __name__ == "__main__":
    sys.exit(main())
