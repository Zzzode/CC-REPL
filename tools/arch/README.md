# Architecture graph check

`graph_check.py` builds Loom's named-module import graph from `src/` and
enforces the invariants of RFC 0001 (see
[`docs/rfcs/0001-module-architecture-target.md`](../../docs/rfcs/0001-module-architecture-target.md)).
It runs in CI on every `src/` change (`.github/workflows/arch-check.yml`)
and has zero third-party dependencies.

## Gates

**Current-state gate (default):**

1. the module-level graph is a DAG (Tarjan SCC);
2. no *new* non-contract upward edge versus `upward_edge_baseline.txt`;
3. no *new* dead import versus `dead_imports_baseline.txt`.

Both baselines are frozen snapshots of known backlog. **Additions fail;
removals are always allowed** and shrink the snapshot as cleanup lands.
Useful outputs while developing:

```bash
python3 tools/arch/graph_check.py             # human-readable
python3 tools/arch/graph_check.py --json      # machine-readable
python3 tools/arch/graph_check.py --target-core8   # future-state Phase B gate
```

**Future-state gate (`--target-core8`):** the nine RFC 0001 target areas
must be pairwise acyclic (nine singleton SCCs). It fails today and becomes
the Phase B completion gate after the 14 cut families land.

## What counts as an upward edge

`TARGET_RANK` gives the total layer order; `MODULE_RANK_OVERRIDE` covers
leaf modules that physically live inside a higher-level area directory.
An edge from a lower-rank area/module to a higher one is illegal unless
the imported module is a structural **contract**: named `*.port` /
`*.contract`, `*_types`, or under `cc.types.*`. Extra contracts are
listed one per line in `port_allowlist.txt`.

## Dead-import detection

An import is reported when none of the imported module's exported names
(types, functions, namespace-scope variables, enumerators, namespace
paths) are referenced in the importing file. The analysis is textual but
guards against common false matches:

- names shadowed by a local declaration do not count;
- `::name` / `.name` qualified or member accesses do not count (genuine
  namespace-qualified uses are matched via the full `cc::...` path or a
  relative `seg::` after a using-namespace);
- `export import` re-exports are never dead.

An intentional import with no textual reference (ADL/operator overload
participation, user-defined literals via `operator""_x`, explicit
ordering requirement) is silenced on the import line:

```cpp
import cc.foo.overloads;  // arch-check: keep-import
```

or on its own line above the import, or file-wide with
`// arch-check: keep-imports`. When a dead import is real, **delete it**
(prefer deleting dead code — project convention).

### Known limits of the textual analysis

The check is deliberately fail-on-addition, so residual imprecision can
only block a genuinely new import — never silently allow a violation.
Known cases where a *required* import may be flagged (use the keep-import
marker):

- ADL-only overload/operator participation and user-defined literals
  (`operator""_x`) that are never named textually;
- `export using ... ;` re-export shapes (none live today);
- aliases targeting area-level namespaces;
- names mentioned only in trailing comments or string literals still
  count as use (false-negative bias — a few real dead imports may be
  missed; tightening requires a fresh compile-verified snapshot).

Module/import *edges* have no such blind spots: extraction runs on
comment-stripped, backslash-newline-spliced text with multiline regexes,
so newlines/comments inside a declaration cannot hide an edge, and an
unknown area fails the gate rather than skipping its edges.

## Refreshing a baseline

After deliberately cleaning up (or, rarely, accepting a sanctioned new
exception), regenerate:

```bash
python3 tools/arch/graph_check.py --json \
  | python3 -c 'import json,sys; [print(a+" -> "+b) for a,b in json.load(sys.stdin)["illegal_upward_edges"]]' \
  > tools/arch/upward_edge_baseline.txt
python3 tools/arch/graph_check.py --json \
  | python3 -c 'import json,sys; [print(a+" -> "+b) for a,b in json.load(sys.stdin)["dead_imports"]]' \
  > tools/arch/dead_imports_baseline.txt
```

Impl-unit entries are keyed `module [impl:src/relative/path.cpp]`, so
sibling TUs never share a verdict.

Baselines are reviewed in the PR that changes them; they must never be
regenerated to silence an accidental new coupling.
