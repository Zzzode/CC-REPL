#!/usr/bin/env python3
"""Phase A: convert named-module units from textual std includes to
`import std;`.

Deterministic, idempotent, dry-run by default. It only touches the global
module fragment (the text before the `(export) module ...;` declaration):

  * removes `#include <H>` where H is a pure C++ library header provided by
    `import std;` (the removable set is derived from the configured libc++
    std.cppm minus the Table-25 C-compatibility headers and __config);
  * KIPS C / POSIX / system / third-party / project includes textually —
    named modules do not export macros, and C headers cover nearly all macro
    needs (INT_MAX, int64_t, assert, errno, PRId64, M_PI, ...);
  * skips a std include that is guarded by #if/#ifdef in the GMF (left for a
    human; mixed is legal);
  * inserts `import std;` right after the module declaration when at least
    one std include was removed;
  * drops a `module;` preamble that becomes empty.

Per-file overrides come from --exceptions JSON: {path: {keep: [headers],
no_import: bool}}.
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

# Table-25 C-compatibility headers: kept textually (macros + global names).
C_HEADERS = {
    "cassert", "cctype", "cerrno", "cfenv", "cfloat", "cinttypes", "ciso646",
    "climits", "clocale", "cmath", "csetjmp", "csignal", "cstdarg",
    "cstdbool", "cstddef", "cstdint", "cstdio", "cstdlib", "cstring",
    "ctgmath", "ctime", "cuchar", "cwchar", "cwctype",
}

MOD_DECL_RE = re.compile(r"^[ \t]*(?:export[ \t]+)?module[ \t]+[A-Za-z0-9_.:]+[ \t]*;")
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*<([^>]+)>[^\n]*\n')
IF_OPEN_RE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef)\b')
IF_CLOSE_RE = re.compile(r'^[ \t]*#[ \t]*endif\b')


def removable_set(std_cppm: pathlib.Path) -> set[str]:
    headers = set(re.findall(r'#\s*include\s+<([^>]+)>', std_cppm.read_text()))
    headers.discard("__config")
    return headers - C_HEADERS


def convert(text: str, removable: set[str], keep: set[str], no_import: bool,
            add: list[str] | None = None):
    lines = text.splitlines(keepends=True)
    # Locate module declaration; everything before it is the GMF.
    mod_idx = next((i for i, ln in enumerate(lines) if MODULE_RE.match(ln)), None)
    if mod_idx is None:
        return None  # not a module unit
    gmf = lines[:mod_idx]

    # Ensure required headers are PRESENT in the GMF (global C functions /
    # POSIX macros that today only arrive transitively). Idempotent.
    present = {m.group(1) for ln in gmf for m in [INCLUDE_RE.match(ln)] if m}
    missing = [h for h in (add or []) if h not in present]
    if missing:
        inc_idx = [i for i, ln in enumerate(gmf) if INCLUDE_RE.match(ln)]
        if inc_idx:
            insert_at = max(inc_idx) + 1
        else:
            # No includes: place after the leading `module;` marker.
            marker = next((i + 1 for i, ln in enumerate(gmf)
                           if ln.strip() == "module;"), 0)
            insert_at = marker
        for h in sorted(missing):
            gmf.insert(insert_at, f"#include <{h}>\n")
            insert_at += 1
        lines = gmf + lines[mod_idx:]
        mod_idx = len(gmf)

    removed: list[str] = []
    depth = 0
    new_gmf: list[str] = []
    for ln in gmf:
        m = INCLUDE_RE.match(ln)
        if m and depth == 0 and m.group(1) in removable and m.group(1) not in keep:
            removed.append(m.group(1))
            continue
        # Track #if depth so guarded std includes are never auto-removed.
        if IF_OPEN_RE.match(ln):
            depth += 1
        elif IF_CLOSE_RE.match(ln):
            depth = max(0, depth - 1)
        new_gmf.append(ln)

    if not removed or no_import:
        return None

    rest = lines[mod_idx:]
    # Insert `import std;` after the module declaration line.
    decl_line = rest[0]
    rest = rest[1:]
    insertion = ["\n", "import std;\n"]
    new_lines = new_gmf + [decl_line] + insertion + rest

    # Drop an emptied `module;` preamble: if the only non-blank GMF content
    # was the leading `module;` marker, remove it and surrounding blanks.
    gmf_text = "".join(new_gmf)
    surviving = [l for l in new_gmf
                 if l.strip() and not re.match(r'^\s*module\s*;\s*$', l)]
    if not surviving:
        # Remove `module;` line and collapse the blank run before the decl.
        out = []
        for l in new_lines:
            if re.match(r'^\s*module\s*;\s*\n?$', l):
                continue
            out.append(l)
        # Collapse 3+ newlines at the head into one blank max.
        text2 = "".join(out)
        text2 = re.sub(r'\A\n{2,}', '', text2)
        text2 = re.sub(r'\n{3,}', '\n\n', text2)
        return text2, removed

    return "".join(new_lines), removed


MODULE_RE = MOD_DECL_RE  # alias used above


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--std-cppm", required=True)
    ap.add_argument("--exceptions", default="")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    removable = removable_set(pathlib.Path(args.std_cppm))
    overrides: dict[str, dict] = {}
    if args.exceptions:
        overrides = json.loads(pathlib.Path(args.exceptions).read_text())

    paths = glob.glob(str(SRC / "**" / "*.cppm"), recursive=True)
    paths += glob.glob(str(SRC / "**" / "*.cpp"), recursive=True)
    changed = 0
    total_removed = 0
    for path in sorted(paths):
        p = pathlib.Path(path)
        text = p.read_text(encoding="utf-8", errors="ignore")
        rel = str(p.relative_to(ROOT))
        ov = overrides.get(rel, {})
        result = convert(text, removable, set(ov.get("keep", [])),
                         ov.get("no_import", False), ov.get("add", []))
        if not result:
            continue
        new_text, removed = result
        added = ov.get("add", [])
        changed += 1
        total_removed += len(removed)
        if args.apply:
            p.write_text(new_text)
        print(f"{'APPLY ' if args.apply else 'DRY   '}{rel}  -{len(removed)}: "
              f"{','.join(sorted(set(removed)))}")
    print(f"\nfiles to change: {changed}; include lines removed: {total_removed}")


if __name__ == "__main__":
    sys.exit(main())
