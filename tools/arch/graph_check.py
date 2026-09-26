#!/usr/bin/env python3
"""Loom architecture graph check (RFC 0001 milestone E0).

Builds the named-module import graph from src/ and enforces:

  CURRENT gate (default, runs in CI on every src change):
    1. module-level graph is a DAG;
    2. no NEW non-contract upward edge versus the frozen backlog in
       upward_edge_baseline.txt (additions fail; removals are fine);
    3. no NEW dead import versus dead_imports_baseline.txt: an imported
       module whose exported symbols/namespaces are never referenced in
       the importing file (additions fail; deleting dead imports shrinks
       the snapshot). Per-line silence for intentional imports:
       // arch-check: keep-import.

  FUTURE gate (--target-core8, the Phase B merge gate):
    the RFC 0001 target areas are pairwise acyclic — every one a singleton
    SCC (expected: 9 incl. orchestration). Fails today; passes after the
    14 families land.

Zero third-party dependencies.
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
HERE = pathlib.Path(__file__).parent
ALLOWLIST = HERE / "port_allowlist.txt"
BASELINE = HERE / "upward_edge_baseline.txt"
DEAD_BASELINE = HERE / "dead_imports_baseline.txt"

# Whole-text (multiline) forms applied AFTER comment/string stripping and
# backslash-newline joining, so legal spellings — `import\n cc.foo;`,
# `import /*c*/ cc.foo;`, `export /*c*/ import cc.foo;`, line splices —
# cannot hide an edge or an entire TU from the graph.
MODULE_DECL_RE = re.compile(
    r"\b(?:(export)\s+)?module\s+([A-Za-z0-9_.:]+)\s*;")
IMPORT_STMT_RE = re.compile(
    r"\b(?:(export)\s+)?import\s+(:?[A-Za-z0-9_][A-Za-z0-9_.:]*)\s*[;:]")
CPP_KEYWORDS = {
    "struct", "class", "enum", "union", "namespace", "using", "template",
    "typename", "typedef", "auto", "const", "constexpr", "consteval",
    "constinit", "inline", "static", "extern", "virtual", "explicit",
    "export", "public", "private", "protected", "friend", "operator",
    "return", "if", "else", "for", "while", "switch", "case", "default",
    "do", "break", "continue", "goto", "try", "catch", "throw", "new",
    "delete", "sizeof", "alignof", "requires", "concept", "co_await",
    "co_return", "co_yield", "true", "false", "nullptr", "this", "void",
    "bool", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "noexcept", "override", "final", "mutable", "volatile",
    "concept", "requires",
}

# Target layer rank (RFC 0001 REV 3). Every live area must appear here so
# that no edge is silently skipped.
TARGET_RANK = {
    "cc.types": 0, "cc.constants": 0, "cc.schemas": 0,
    "cc.wire": 0, "cc.core": 0,
    # mailbox/coordinator types are pure-data leaves today.
    "cc.context": 0, "cc.coordinator": 0,
    "cc.config": 1,
    "cc.migrations": 1,
    "cc.utils": 2,
    "cc.vim": 3,
    "cc.hooks": 4,
    "cc.skills": 5,
    "cc.state": 6, "cc.session": 6, "cc.history": 6,
    "cc.task_types": 6, "cc.memdir": 6, "cc.tasks": 6,
    "cc.services": 7,
    "cc.plugins": 7,
    "cc.tools": 8,
    "cc.orchestration": 9,  # planned by RFC 0001 Phase B
    "cc.query": 10,
    "cc.commands": 11,
    "cc.keybindings": 11,
    "cc.ui": 12,
    "cc.buddy": 12,
    "cc.server": 13, "cc.daemon": 13,
    "cc.bridge": 13,
    "cc.bootstrap": 13,
    "cc.cli": 14,
    "cc.entrypoints": 15,
    "cc.benchmarks": 15,
}

# Leaf modules physically located inside a higher-ranked area directory.
# They are standalone transport/util modules with no upward deps; rank them
# with utils so their importers do not acquire artificial upward edges.
MODULE_RANK_OVERRIDE = {
    "cc.cli.ccr_client": 2,
    "cc.cli.sse_transport": 2,
    "cc.cli.transports": 2,
    "cc.cli.websocket_transport": 2,
    "cc.cli.update": 2,
}

CORE8 = ["cc.config", "cc.hooks", "cc.services", "cc.skills",
         "cc.state", "cc.task_types", "cc.tools", "cc.utils"]
TARGET_AREAS = CORE8 + ["cc.orchestration"]


def area_of(module: str) -> str:
    parts = module.split(".")
    return ".".join(parts[:2]) if len(parts) >= 2 else module


def rank_of(module: str):
    if module in MODULE_RANK_OVERRIDE:
        return MODULE_RANK_OVERRIDE[module]
    return TARGET_RANK.get(area_of(module))


def is_contract(module: str, allow: set[str]) -> bool:
    """Structural test — exact-segment, never a substring match."""
    leaf = module.split(".")[-1]
    if leaf in ("port", "contract"):
        return True
    if leaf.endswith("_types"):
        return True
    if module == "cc.types" or module.startswith("cc.types."):
        return True
    if module in allow:
        return True
    return False


def load_allowlist() -> set[str]:
    if not ALLOWLIST.exists():
        return set()
    return {ln.strip() for ln in ALLOWLIST.read_text().splitlines()
            if ln.strip() and not ln.strip().startswith("#")}


def load_pair_baseline(path, sep: str) -> set[tuple[str, str]]:
    p = pathlib.Path(path)
    if not p.exists():
        return set()
    out = set()
    for ln in p.read_text().splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        if sep in ln:
            a, b = ln.split(sep, 1)
            out.add((a.strip(), b.strip()))
    return out


def load_baseline() -> set[tuple[str, str]]:
    return load_pair_baseline(BASELINE, " -> ")


# A file in the module graph. kind: primary | partition | impl
class Unit:
    __slots__ = ("module", "kind", "text", "path", "imports", "reexports")

    def __init__(self, module, kind, text, path):
        self.module = module
        self.kind = kind
        self.text = text
        self.path = path
        self.imports: set[str] = set()
        self.reexports: set[str] = set()


def _normalize_for_scan(text: str) -> str:
    """Phase-2 backslash-newline splices joined (tokens concatenate), then
    comments/strings blanked (positions kept), then preprocessor directives
    blanked (so `<import>`-style include names can't match).

    The stripper is raw-string-naive, but C++ requires import declarations
    to immediately follow the module declaration, so a raw literal cannot
    appear before an import — it cannot hide an edge in practice."""
    text = re.sub(r"\\\r?\n", "", text)
    text = _strip_comments_strings(text)
    text = re.sub(r"(?m)^[ \t]*#.*$", "", text)
    return text


def load_units():
    units: list[Unit] = []
    paths = glob.glob(str(SRC / "**" / "*.cppm"), recursive=True)
    paths += glob.glob(str(SRC / "**" / "*.cpp"), recursive=True)
    for path in paths:
        raw_text = pathlib.Path(path).read_text(
            encoding="utf-8", errors="ignore")
        text = _normalize_for_scan(raw_text)
        m = MODULE_DECL_RE.search(text)
        if not m:
            continue
        mod_raw = m.group(2)
        if ":" in mod_raw:
            kind = "partition"
        elif m.group(1):
            kind = "primary"
        else:
            kind = "impl"
        name = mod_raw.split(":")[0]
        unit = Unit(name, kind, raw_text, path)
        for sm in IMPORT_STMT_RE.finditer(text):
            imp = sm.group(2).split(":")[0]
            if imp.startswith("cc."):
                unit.imports.add(imp)
                if sm.group(1):
                    unit.reexports.add(imp)
        units.append(unit)
    known = {u.module for u in units}
    for u in units:
        u.imports = {i for i in u.imports if i in known}
        u.reexports = {i for i in u.reexports if i in known}
    return units


def module_deps(units):
    deps: dict[str, set[str]] = {}
    for u in units:
        deps.setdefault(u.module, set()).update(u.imports)
    return deps


def tarjan_scc(graph: dict[str, set[str]]) -> list[list[str]]:
    index, low, stack, on = {}, {}, [], set()
    counter, out = [0], []

    def visit(v):
        index[v] = low[v] = counter[0]; counter[0] += 1
        stack.append(v); on.add(v)
        for w in graph.get(v, ()):
            if w not in index:
                visit(w); low[v] = min(low[v], low[w])
            elif w in on:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop(); on.discard(w); comp.append(w)
                if w == v:
                    break
            out.append(comp)

    sys.setrecursionlimit(100000)
    for v in graph:
        if v not in index:
            visit(v)
    return out


def area_graph(deps):
    g = {}
    for m, imps in deps.items():
        a = area_of(m)
        bk = g.setdefault(a, set())
        for i in imps:
            b = area_of(i)
            if b != a:
                bk.add(b)
    return g


def upward_edges(deps, allow):
    out = []
    for m in sorted(deps):
        ra = rank_of(m)
        if ra is None:
            continue
        for i in sorted(deps[m]):
            rb = rank_of(i)
            if rb is not None and rb > ra:
                out.append((m, i, is_contract(i, allow)))
    return out


def _strip_comments_strings(text: str) -> str:
    """Replace comments/string contents with spaces (positions preserved)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" "); i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            while i + 1 < n and (text[i], text[i + 1]) != ("*", "/"):
                out.append(" " if text[i] != "\n" else "\n"); i += 1
            i += 2; out.extend("  ")
        elif c in "\"'":
            quote = c; out.append(" "); i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out.append(" "); i += 1
                    if i < n: out.append(" "); i += 1
                else:
                    out.append(" " if text[i] != "\n" else "\n"); i += 1
            if i < n: out.append(" "); i += 1
        else:
            out.append(c); i += 1
    return "".join(out)


def _parse_interface(text: str, exported_only: bool):
    """Return (names, namespace_paths) declared by one file.

    Style of this tree: `export namespace cc::x { ...plain decls... }`.
    A region stack distinguishes declaration scopes (namespace/class/enum)
    from function bodies and initializers, so call sites like push_back()
    are never mistaken for declarations. Member VARIABLES are deliberately
    not harvested: names like `id`/`string` collide with ubiquitous tokens
    and would mask genuinely unused imports.
    """
    bucket: set[str] = set()
    nsb: set[str] = set()
    text = _strip_comments_strings(text)

    token_re = re.compile(
        r"\b(struct|class|enum|union|namespace|using)\b"
        r"|([A-Za-z_]\w*)\s*\("
        r"|([A-Za-z_]\w*)"
        r"|([{};])")
    kind_re = re.compile(r"\b(namespace|struct|class|enum|union)\b")

    # Stack entries: (kind in {ns,cls,en,other,block}, exported, ns_name)
    stack: list[tuple[str, bool, str]] = [("other", not exported_only, "")]
    pending_export = False
    last_boundary = -1

    def head_exported():
        return pending_export or stack[-1][1]

    def current_ns_path():
        segs: list[str] = []
        for _, _, ns in stack:
            if ns:
                segs.extend(ns.split("::"))
        return "::".join(segs)

    i, n = 0, len(text)
    while i < n:
        m = token_re.match(text, i)
        if not m:
            i += 1
            continue
        i = m.end()

        if m.group(1):  # declaration keyword
            kw = m.group(1)
            if kw == "using":
                rest = text[m.end():m.end() + 200]
                um = re.match(r"\s*(?!enum\b)([A-Za-z_]\w*)\s*=", rest)
                if um and head_exported():
                    bucket.add(um.group(1))
            elif kw == "namespace":
                rest = text[m.end():m.end() + 300]
                if "=" in rest.split(";", 1)[0] or "{" not in rest.split(";", 1)[0]:
                    # namespace ALIAS (`namespace fs = ...;`) or forward form
                    # without a body here — not a namespace path we own.
                    pass
                else:
                    nm = re.match(r"\s*(?:inline\s+)?([A-Za-z_][\w:]*)", rest)
                    if nm:
                        nsb.add(nm.group(1))
                # nested-namespace composition is finalized at the brace
            continue

        brace = m.group(4)
        if brace == "{":
            head = text[last_boundary + 1:m.start()]
            km = kind_re.search(head)
            if km:
                word = km.group(1)
                kind = {"namespace": "ns", "enum": "en"}.get(word, "cls")
                frame_ns = ""
                if word == "namespace":
                    nm = re.search(
                        r"namespace\s+(?:inline\s+)?([A-Za-z_][\w:]*)", head)
                    if nm:
                        frame_ns = nm.group(1)
                        nsb.add("::".join(
                            p for p in (current_ns_path(), frame_ns) if p))
                else:
                    nm = re.search(
                        r"\b(?:struct|class|enum(?:\s+class)?|union)\s+"
                        r"([A-Za-z_]\w*)", head)
                    if nm and head_exported():
                        bucket.add(nm.group(1))
                stack.append((kind, head_exported(), frame_ns))
            elif re.search(r"\bexport\s*$", head):
                stack.append(("block", True, ""))  # `export { ... }`
            else:
                # Function/control/initializer body: locals are not interface.
                stack.append(("other", False, ""))
            pending_export = False
            last_boundary = m.start()
            continue
        if brace == "}":
            if len(stack) > 1:
                stack.pop()
            last_boundary = m.start()
            continue
        if brace == ";":
            head = text[last_boundary + 1:m.start()]
            stripped = head.strip()
            # The head ending in the module declaration carries the whole
            # global module fragment (#include lines); never harvest names
            # from it or from module/import/preprocessor statements.
            is_module_stmts = bool(
                re.search(r"(?:^|\n)\s*(?:export\s+)?(?:module|import)\b",
                          stripped)
                or re.search(r"(?:^|\n)\s*#", stripped)
                or (re.search(r"\bnamespace\b", stripped)
                    and "(" not in stripped))
            top_kind, top_exp, _ = stack[-1]
            exported_stmt = pending_export or (
                top_exp and top_kind in ("ns", "block"))
            local_member = (not exported_only and top_kind == "cls")
            if (exported_stmt or local_member) and not is_module_stmts:
                for f in re.findall(r"([A-Za-z_]\w*)\s*\(", head):
                    if f not in CPP_KEYWORDS:
                        bucket.add(f.lstrip("~"))
                if "(" not in head and "::" not in head:
                    # namespace-scope variable/alias. A qualified
                    # using-declaration (`using cc::x::Name;`) is a REFERENCE
                    # to another module, not a local declaration.
                    tail = re.split(r"[=]", head, maxsplit=1)[0]
                    ids = re.findall(r"[A-Za-z_]\w*", tail)
                    for v in ids[-2:]:
                        if v not in CPP_KEYWORDS:
                            bucket.add(v)
            pending_export = False
            last_boundary = m.start()
            continue

        name_tok = m.group(3)
        if name_tok == "export":
            pending_export = True
            continue
        if name_tok in CPP_KEYWORDS:
            continue
        if m.group(2):  # function/ctor declarator at ns/class/block scope
            top = stack[-1]
            if top[1] and top[0] in ("ns", "cls", "block"):
                bucket.add(m.group(2).lstrip("~"))
            continue
        if stack[-1][0] == "en" and stack[-1][1]:
            bucket.add(name_tok)  # enumerator

    return bucket, nsb


def exported_symbols(units) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Per module: (exported names, declared namespace paths).
    Union over primary + partitions — the module interface."""
    names: dict[str, set[str]] = {}
    ns_paths: dict[str, set[str]] = {}
    for u in units:
        if u.kind == "impl":
            continue
        bucket, nsb = _parse_interface(u.text, exported_only=True)
        names.setdefault(u.module, set()).update(bucket)
        ns_paths.setdefault(u.module, set()).update(nsb)
    return names, ns_paths




def _prefix_chain(body: str, pos: int) -> str:
    """The `a::b::c::` qualifier chain immediately before position pos,
    or "" if the name is not namespace-qualified."""
    i = pos
    segs: list[str] = []
    while i >= 2 and body[i - 2:i] == "::":
        j = i - 2
        k = j
        while k > 0 and (body[k - 1].isalnum() or body[k - 1] == "_"):
            k -= 1
        if k == j:
            return ""
        segs.append(body[k:j])
        i = k
    return "::".join(reversed(segs))


def unit_label(u: Unit) -> str:
    """Stable per-translation-unit identity. Implementation units sharing a
    module name MUST be distinguished by file (one TU may need an import a
    sibling TU does not)."""
    if u.kind == "impl":
        rel = pathlib.Path(u.path).resolve().relative_to(SRC)
        return f"{u.module} [impl:{rel}]"
    if u.kind == "partition":
        return f"{u.module}:{pathlib.Path(u.path).stem}"
    return u.module


def find_dead_imports(units, symbols, ns_paths):
    """An import is dead when NONE of the imported module's exported names
    are referenced in the importing FILE. A candidate name is evidence of
    use only if it (a) is not shadowed by a declaration in this file or the
    module's own interface, and (b) has an unqualified occurrence —
    `::name`/`.name` are member/qualified accesses (genuine qualified uses
    are matched through the namespace paths or a namespace alias)."""
    # namespace path -> set of owning modules. Namespaces are OPEN in C++:
    # several modules may declare the same namespace (e.g. repl_state and
    # repl_screen both open cc::ui::repl_screen), so ownership is a set and
    # iteration order must never decide the result.
    ns_owner: dict[str, set[str]] = {}
    for mod, paths in ns_paths.items():
        for p in paths:
            ns_owner.setdefault(p, set()).add(mod)

    alias_re = re.compile(
        r"\bnamespace\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_][\w:]*)\s*;")

    # Namespace aliases declared ANYWHERE in a module are visible in every
    # unit of that module: an impl unit may say `repl::Foo` where the
    # primary declares `namespace repl = cc::ui::repl_screen;`. It must
    # still import the aliased module itself (alias gives no visibility).
    module_aliases: dict[str, dict[str, str]] = {}
    for v in units:
        bk = module_aliases.setdefault(v.module, {})
        for am in alias_re.finditer(v.text):
            bk[am.group(1)] = am.group(2)

    dead = []
    for u in units:
        kept = set()
        file_silenced = False
        body_lines = []
        pending_keep = False
        for line in u.text.splitlines():
            if "arch-check: keep-imports" in line:
                file_silenced = True
            if "arch-check: keep-import" in line:
                mm = re.search(r"import\s+([A-Za-z0-9_][A-Za-z0-9_.:]*)", line)
                if mm:
                    kept.add(mm.group(1).split(":")[0])
                else:
                    pending_keep = True  # marker on its own line
            elif pending_keep:
                mm = re.search(r"import\s+([A-Za-z0-9_][A-Za-z0-9_.:]*)", line)
                if mm:
                    kept.add(mm.group(1).split(":")[0])
                pending_keep = False
            if not line.lstrip().startswith(("import ", "export import", "//", "*")):
                body_lines.append(line)
        if file_silenced:
            continue
        # NOTE: body is raw text minus full-line comments/imports. Names in
        # trailing comments or string literals still count as use — a
        # false-NEGATIVE-only bias (real dead imports may be missed, but a
        # live import is never wrongly flagged). Tightening this is a
        # separate step that needs its own compile-verified snapshot.
        body = "\n".join(body_lines)
        # Declarations in this file or the module's own interface shadow
        # imported names; sibling implementation units do NOT (per-TU).
        own, _ = _parse_interface(u.text, exported_only=False)
        own |= symbols.get(u.module, set())

        # Namespace aliases visible in this TU: declared here or in any
        # other unit of this module (see module_aliases above).
        aliases = dict(module_aliases.get(u.module, {}))

        for imp in sorted(u.imports):
            if imp in u.reexports or imp in kept:
                continue
            used = False
            for n in symbols.get(imp, ()):
                if not n or n in own:
                    continue
                for mm in re.finditer(rf"\b{re.escape(n)}\b", body):
                    prev = body[mm.start() - 1] if mm.start() > 0 else ""
                    if prev == ".":
                        continue  # member access can't name a free symbol
                    if prev == ":":
                        # `Q::name` counts only when Q is a namespace declared
                        # by the imported module (free function in that
                        # namespace) — never for `object.method`-style
                        # qualification or other modules' scopes.
                        chain = _prefix_chain(body, mm.start())
                        if not chain or not any(
                                chain == p or chain.startswith(p + "::")
                                for p in ns_paths.get(imp, ())):
                            continue
                    used = True
                    break
                if used:
                    break
            if not used:
                # namespace-qualified use: full `cc::area::sub::Foo`, a
                # relative `sub::Foo`, or an aliased `dt::Foo`. Paths shorter
                # than 3 segments (cc::utils, cc::core) are NOT evidence: many
                # modules share the area namespace, so text naming another
                # module in the same area would otherwise match spuriously.
                for path in ns_paths.get(imp, ()):
                    if path.count("::") < 2:
                        continue
                    if path in body:
                        used = True
                        break
                    seg = path.rsplit("::", 1)[-1]
                    if re.search(rf"\b{re.escape(seg)}::", body):
                        used = True
                        break
                if not used:
                    for alias, target in aliases.items():
                        owners: set[str] = set()
                        for p, mods in ns_owner.items():
                            # Only DEEP paths identify an owning module; an
                            # area-level path (cc::ui) is opened by dozens of
                            # modules and would match every target under it.
                            if p.count("::") < 2:
                                continue
                            if target == p or target.startswith(p + "::"):
                                owners |= mods
                        if imp in owners and re.search(
                                rf"\b{re.escape(alias)}::", body):
                            used = True
                            break
            if not used and imp in body:  # dotted form (validated snapshot)
                used = True
            if not used:
                dead.append((unit_label(u), imp))
    return dead


def run(target, allow_dead_imports=False):
    allow = load_allowlist()
    baseline = load_baseline()
    dead_baseline = load_pair_baseline(DEAD_BASELINE, " -> ")
    units = load_units()
    deps = module_deps(units)
    symbols, ns_paths = exported_symbols(units)

    # Fail closed: every area present in the graph must have a rank. A new
    # unranked area would otherwise have all its upward edges silently
    # skipped. (Per-module overrides only relocate individual leaves.)
    unranked = sorted({
        area_of(m) for m in deps if area_of(m) not in TARGET_RANK})

    cycles = [c for c in tarjan_scc(deps) if len(c) > 1]
    up = upward_edges(deps, allow)
    illegal = [(m, i) for m, i, ok in up if not ok]
    new_edges = sorted(set(illegal) - baseline)
    removed = sorted(baseline - set(illegal))
    dead = find_dead_imports(units, symbols, ns_paths)
    new_dead = sorted(set(dead) - dead_baseline)
    dead_removed = sorted(dead_baseline - set(dead))

    areas = area_graph(deps)
    area_sccs = sorted((sorted(c) for c in tarjan_scc(areas) if len(c) > 1),
                       key=len, reverse=True)

    report = {
        "modules": len(deps),
        "units": len(units),
        "module_cycles": cycles,
        "illegal_upward_edges": illegal,
        "new_upward_edges": new_edges,
        "baseline_edges_removed": removed,
        "dead_imports": dead,
        "new_dead_imports": new_dead,
        "dead_baseline_removed": dead_removed,
        "area_sccs_current": area_sccs,
        "unranked_areas": unranked,
    }

    if target:
        tg = {a: set() for a in TARGET_AREAS}
        for m, imps in deps.items():
            a = area_of(m)
            if a not in tg:
                continue
            for i in imps:
                b = area_of(i)
                if b in tg and b != a:
                    tg[a].add(b)
        sccs = [sorted(c) for c in tarjan_scc(tg) if len(c) > 1]
        report["target_area_sccs"] = sccs
        report["target_core8_passes"] = not sccs

    # Default gate fails on: unranked area, module cycles, NEW upward
    # edges, NEW dead imports. Known baseline backlog is allowed.
    current_ok = (not unranked and not cycles and not new_edges
                  and (not new_dead or allow_dead_imports))
    report["passes"] = current_ok and (
        not target or report.get("target_core8_passes", True))
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target-core8", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--allow-dead-imports", action="store_true",
                    help="tolerate NEW dead imports too (escape hatch)")
    args = ap.parse_args()
    r = run(args.target_core8, args.allow_dead_imports)

    gate_ok = r["passes"]

    if args.json:
        print(json.dumps(r, indent=2))
        return 0 if gate_ok else 1

    print(f"modules analyzed: {r['modules']} ({r['units']} units)")
    print(f"module-level cycles: {len(r['module_cycles'])}")
    for c in r["module_cycles"][:10]:
        print("  CYCLE:", " -> ".join(sorted(c)[:8]))
    if r["unranked_areas"]:
        print("UNRANKED AREAS (add them to TARGET_RANK) — FAIL:")
        for a in r["unranked_areas"]:
            print(f"    {a}")
    print(f"non-contract upward edges: {len(r['illegal_upward_edges'])} "
          f"(baseline backlog; {len(r['baseline_edges_removed'])} removed)")
    if r["new_upward_edges"]:
        print("  NEW (not in baseline) — FAIL:")
        for m, i in r["new_upward_edges"]:
            print(f"    {m} -> {i}")
    if r["baseline_edges_removed"]:
        print("  removed since baseline (good):")
        for m, i in r["baseline_edges_removed"][:20]:
            print(f"    {m} -> {i}")
    print(f"dead imports: {len(r['dead_imports'])} (baseline backlog; "
          f"{len(r['dead_baseline_removed'])} removed)")
    if r["new_dead_imports"]:
        print("  NEW (not in baseline) — FAIL:")
        for m, i in r["new_dead_imports"][:40]:
            print(f"    {m} -> {i}  (delete it, or // arch-check: keep-import)")
    print(f"current directory SCCs (>1): {len(r['area_sccs_current'])}")
    for c in r["area_sccs_current"]:
        print("  SCC:", ", ".join(c))
    if args.target_core8:
        print("RFC 0001 Phase B target (9 singleton areas):",
              "PASS" if r["target_core8_passes"] else "FAIL")
        for c in r["target_area_sccs"]:
            print("  TARGET SCC:", ", ".join(c))

    print("graph_check:", "OK" if gate_ok else "FAIL")
    return 0 if gate_ok else 1


if __name__ == "__main__":
    sys.exit(main())
