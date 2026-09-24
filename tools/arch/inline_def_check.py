#!/usr/bin/env python3
"""
inline_def_check.py — RFC 0001 Phase C ratchet on god-interface inline bodies.

Companion to graph_check.py. It counts the SEMANTIC inline definitions in
module INTERFACE units (.cppm) and enforces a frozen per-module snapshot:

  1. Ratchet (fail-on-increase). Every interface currently over the C2 cap is
     listed in inline_def_baseline.txt with its frozen semantic inline-body
     count. A count ABOVE the frozen value fails; a count BELOW it is a shrink
     (reported; re-freeze with --update in the same split commit).
  2. Fail-closed for new god interfaces. An interface that is NOT in the
     baseline but now exceeds the C2 cap fails — a new god interface cannot
     slip in unlisted.
  3. Graduation caps. Baseline flags:
        c1        the six RFC-0001 C1 modules; must be < C1_LIMIT (30) once
                  their Phase C batch merges (flag is added in that commit).
        c2-done   module has graduated; the C2 cap (100) is enforced.
  4. Deliberate retainers. A definition whose signature line carries
        // arch-check: keep-inline
     is exempt (one per marker). Use sparingly for trivial-but-bulky bodies
     that must stay in the interface (e.g. palette accessors).

The counter is deliberately SEMANTIC, not the loose OQ-4 line heuristic: it
counts named function/method/operator/ctor/dtor definitions that carry a
brace body at namespace or class scope in the module purview, excluding
lambdas, control blocks (if/for/while/switch/catch), function-local classes,
namespaces, and data-member initializers. =default/=delete are reported
separately and never count as bodies. Module IMPLEMENTATION units
(`module x;` .cpp) are exempt by design — bodies belong there.

Usage:
  inline_def_check.py            # enforce the ratchet (exit 1 on violation)
  inline_def_check.py --json     # machine-readable result
  inline_def_check.py --update   # rewrite frozen counts to current (shrinks)
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / "src"
BASELINE = HERE / "inline_def_baseline.txt"

C2_LIMIT = 100   # no interface above this once graduated
C1_LIMIT = 30    # the RFC-0001 C1 six must reach this

EXPORT_MODULE_RE = re.compile(r"\bexport\s+module\s+([A-Za-z0-9_.:]+)\s*;")
MODULE_ANY_RE = re.compile(
    r"(?:\bexport\s+)?module\s*(?::\s*private|[A-Za-z_][\w.]*(?:\s*:\s*[\w.]+)?)\s*;")
KEEP_RE = re.compile(r"arch-check:\s*keep-inline")

IDENT_RE = re.compile(r"[A-Za-z_]\w*")
PUNCTS = ['<=>', '->*', '...', '::', '->', '++', '--', '<<=', '>>=',
          '==', '!=', '<=', '>=', '&&', '||', '<<', '>>', '+=', '-=',
          '*=', '/=', '%=', '&=', '|=', '^=', '##',
          '{', '}', '(', ')', '[', ']', ';', ':', '?', ',', '.',
          '+', '-', '*', '/', '%', '&', '|', '^', '~', '!', '<', '>', '=']
CONTROL = {'if', 'else', 'while', 'for', 'switch', 'catch', 'try', 'do'}


# ------------------------------------------------------------ stripping

def strip(text: str) -> str:
    """Blank comments and string/char literals, preserving every newline."""
    n = len(text)
    out: list[str] = []
    i = 0
    raw_re = re.compile(r'(?:u8|u|U|L)?R"([^()\\ \t\x0b\r\n]{0,16})\(')
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i += 2
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            i += 2
            continue
        m = raw_re.match(text, i) if c in 'RLUu"' else None
        if m:
            delim = m.group(1)
            body_start = m.end()
            close = ')' + delim + '"'
            at = text.find(close, body_start)
            if at == -1:
                body_end, i = n, n
            else:
                body_end, i = at, at + len(close)
            out.append(' SSS ')
            out.extend('\n' if ch == '\n' else ' ' for ch in text[body_start:body_end])
            continue
        if c == '"' or c == "'":
            if c == "'" and 0 < i < n - 1 and \
                    text[i - 1].isalnum() and text[i + 1].isalnum():
                out.append(c)          # digit separator 200'000
                i += 1
                continue
            quote = c
            i += 1
            while i < n:
                ch = text[i]
                if ch == '\n':
                    out.append('\n')
                if ch == '\\':
                    i += 2
                    continue
                if ch == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def blank_preprocessor(text: str) -> str:
    lines = text.split('\n')
    out: list[str] = []
    cont = False
    for line in lines:
        if cont:
            out.append('')
            cont = line.rstrip().endswith('\\')
            continue
        if line.lstrip().startswith('#'):
            out.append('')
            cont = line.rstrip().endswith('\\')
            continue
        out.append(line)
    return '\n'.join(out)


def tokenize(text: str) -> list[str]:
    toks: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        m = IDENT_RE.match(text, i)
        if m:
            toks.append(m.group(0))
            i = m.end()
            continue
        for p in PUNCTS:
            if text.startswith(p, i):
                toks.append(p)
                i += len(p)
                break
        else:
            i += 1
    return toks


def count_strict(purview: str) -> tuple[int, int]:
    """Semantic inline-body count and separate =default/=delete count."""
    toks = tokenize(purview)
    n = len(toks)
    match: dict[int, int] = {}
    stk: list[tuple[str, int]] = []
    for idx, tok in enumerate(toks):
        if tok in '([{':
            stk.append((tok, idx))
        elif tok in ')]}':
            if stk and stk[-1][0] == {')': '(', ']': '[', '}': '{'}[tok]:
                _, oi = stk.pop()
                match[oi] = idx
                match[idx] = oi

    saved: list[int] = []
    cur = 0
    bodies = 0
    defaulted = 0
    kind_stack: list[tuple[str, int]] = []

    def header_pairs(hs, end):
        pairs = []
        d = 0
        for k in range(hs, end):
            t = toks[k]
            if t == '(':
                if d == 0 and k in match and match[k] < end:
                    pairs.append((k, match[k]))
                d += 1
            elif t == ')':
                d -= 1
        return pairs

    def is_lambda(hs, end):
        d = 0
        for k in range(hs, end):
            t = toks[k]
            if t in '([':
                if t == '[' and d == 0 and not (k + 1 < end and toks[k + 1] == '[') \
                        and (k == 0 or toks[k - 1] != 'operator') and k in match \
                        and match[k] < end:
                    nxt = toks[match[k] + 1] if match[k] + 1 < end else ''
                    if nxt in ('(', '<', 'mutable', '->'):
                        return True
                d += 1
            elif t in ')]':
                d -= 1
        return False

    def suffix_ok(c, end):
        j = c + 1
        while j < end:
            t = toks[j]
            if t in ('const', 'volatile', 'override', 'final', '&', '&&'):
                j += 1
                continue
            if t in ('noexcept', 'throw'):
                j += 1
                if j < end and toks[j] == '(':
                    j = match.get(j, j) + 1
                continue
            if t == '->':
                j += 1
                d = 0
                while j < end:
                    tk = toks[j]
                    if tk in '([{':
                        d += 1
                    elif tk in ')]}':
                        d -= 1
                    elif d == 0 and tk in (':', 'try'):
                        break
                    j += 1
                continue
            if t == ':':
                return True
            return False
        return True

    def declarator_ok(o):
        p = o - 1
        if p < 0:
            return False
        tok = toks[p]
        if tok == ')' and p >= 2 and toks[p - 1] == '(' and \
                toks[p - 2] == 'operator':
            return True
        if tok == ']' and p >= 2 and toks[p - 1] == '[' and \
                toks[p - 2] == 'operator':
            return True
        if tok == '~' or tok == '>':
            return True
        if IDENT_RE.fullmatch(tok or ''):
            return tok not in CONTROL and tok not in ('decltype', 'requires', 'sizeof')
        k = p
        while k >= 0 and toks[k] in PUNCTS:
            k -= 1
        if k >= 0 and toks[k] == 'operator':
            return True
        return False

    for i, t in enumerate(toks):
        if t == '{':
            hs = cur
            h = toks[hs:i]
            k0 = 1 if h and h[0] == 'export' else 0
            first = h[k0] if k0 < len(h) else ''
            if first == 'namespace' or first == 'extern':
                kind = 'ns'
            elif first in ('class', 'struct', 'union'):
                kind = 'class'
            elif first in CONTROL:
                kind = 'control'
            else:
                kind = 'other'
            is_func = False
            if kind == 'other' and not is_lambda(hs, i):
                for o, c in reversed(header_pairs(hs, i)):
                    if suffix_ok(c, i) and declarator_ok(o):
                        is_func = True
                        break
            if is_func:
                kind = 'func'
            if is_func and all(k in ('ns', 'class') for k, _ in kind_stack):
                bodies += 1
            kind_stack.append((kind, i))
            saved.append(cur)
            cur = i + 1
        elif t == '}':
            if kind_stack:
                kind_stack.pop()
            if saved:
                cur = saved.pop()
            cur = i + 1
        elif t in '([':
            saved.append(cur)
            cur = i + 1
        elif t in ')]':
            if saved:
                cur = saved.pop()
        elif t == ';':
            if all(k in ('ns', 'class') for k, _ in kind_stack):
                if i > 0 and toks[i - 1] in ('default', 'delete') and \
                        i - 2 >= 0 and toks[i - 2] == '=':
                    eq = i - 2
                    for o, c in reversed(header_pairs(cur, eq)):
                        if suffix_ok(c, eq) and declarator_ok(o):
                            defaulted += 1
                            break
            cur = i + 1
    return bodies, defaulted


# ------------------------------------------------------------ baseline

def load_baseline() -> dict[str, dict]:
    data: dict[str, dict] = {}
    if not BASELINE.exists():
        return data
    for raw in BASELINE.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        module = parts[0]
        frozen = int(parts[1])
        flags = set(parts[2:])
        data[module] = {'frozen': frozen, 'flags': flags}
    return data


def analyze_interface(path: pathlib.Path) -> tuple[str, int, int, int]:
    raw = path.read_text(errors='ignore')
    keep = len(KEEP_RE.findall(raw))
    cleaned = blank_preprocessor(strip(raw))
    m = EXPORT_MODULE_RE.search(cleaned)
    if not m:
        return ('', 0, 0, keep)
    purview = cleaned[m.end():]
    bodies, defaulted = count_strict(purview)
    return (m.group(1), bodies, defaulted, keep)


def run() -> dict:
    baseline = load_baseline()
    interfaces = sorted(glob.glob(str(SRC / '**' / '*.cppm'), recursive=True))
    rows = []
    violations: list[str] = []
    shrinks = []
    for path in interfaces:
        module, bodies, defaulted, keep = analyze_interface(pathlib.Path(path))
        if not module:
            continue
        effective = max(0, bodies - keep)
        rel = str(pathlib.Path(path).relative_to(ROOT))
        row = {'module': module, 'path': rel, 'inline': effective,
               'defaulted': defaulted, 'keep': keep}
        spec = baseline.get(module)
        if spec is None:
            if effective > C2_LIMIT:
                violations.append(
                    f"{module}: {effective} inline bodies exceed the {C2_LIMIT} cap "
                    f"but it is NOT in inline_def_baseline.txt (fail-closed; add a "
                    f"baseline entry or extract bodies) — {rel}")
            row['state'] = 'untracked'
        else:
            frozen = spec['frozen']
            row['frozen'] = frozen
            if effective > frozen:
                violations.append(
                    f"{module}: inline bodies INCREASED {frozen} -> {effective} "
                    f"(ratchet; extract bodies or, if justified, re-freeze in review) — {rel}")
                row['state'] = 'increased'
            elif effective < frozen:
                shrinks.append(f"{module}: {frozen} -> {effective}")
                row['state'] = 'shrink'
            else:
                row['state'] = 'at-frozen'
            flags = spec['flags']
            if 'c1' in flags and effective >= C1_LIMIT:
                violations.append(
                    f"{module}: flagged c1 but has {effective} inline bodies "
                    f"(must be < {C1_LIMIT}) — {rel}")
            if 'c2-done' in flags and effective > C2_LIMIT:
                violations.append(
                    f"{module}: flagged c2-done but has {effective} inline bodies "
                    f"(must be <= {C2_LIMIT}) — {rel}")
        rows.append(row)
    return {'interfaces': len(rows), 'violations': violations,
            'shrinks': shrinks, 'rows': rows}


def update_baseline(result: dict) -> None:
    keep_flags = {m: d['flags'] for m, d in load_baseline().items()}
    lines = []
    for r in sorted(result['rows'], key=lambda r: -r['inline']):
        if r['state'] == 'untracked' and r['inline'] <= C2_LIMIT:
            continue
        flags = sorted(keep_flags.get(r['module'], set()))
        lines.append(' '.join([r['module'], str(r['inline']), *flags]).rstrip())
    header = (
        "# RFC 0001 Phase C — frozen SEMANTIC inline-definition counts per god interface.\n"
        "# Columns: <module> <frozen semantic inline bodies> [flags]\n"
        "# The ratchet (tools/arch/inline_def_check.py) FAILS on any increase;\n"
        "# decreases shrink this file in the same split commit. c1 = must reach <30\n"
        "# when that module's batch merges; c2-done = <=100 cap now enforced.\n")
    BASELINE.write_text(header + '\n'.join(lines) + '\n')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--update', action='store_true',
                    help='re-freeze current counts (run in the same commit as a split)')
    args = ap.parse_args()

    result = run()
    if args.update:
        update_baseline(result)

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        tracked = [r for r in result['rows'] if r['state'] != 'untracked']
        over = [r for r in result['rows'] if r['inline'] > C2_LIMIT]
        print(f"interfaces analyzed: {result['interfaces']} "
              f"({len(tracked)} frozen, {len(over)} over {C2_LIMIT})")
        if result['shrinks']:
            print("SHRUNK (re-freeze with --update):")
            for s in result['shrinks']:
                print(f"  - {s}")
        if result['violations']:
            print("INLINE-DEF RATCHET VIOLATIONS:")
            for v in result['violations']:
                print(f"  ! {v}")
        else:
            print("inline_def_check: OK")
    if args.update:
        return 0
    return 1 if result['violations'] else 0


if __name__ == '__main__':
    sys.exit(main())
