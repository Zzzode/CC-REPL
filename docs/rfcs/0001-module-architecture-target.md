---
rfc: 1
title: Module Architecture Target Shape
status: implementable
owners: "@Zzzode"
reviewers: ["agent:design-review#3 (approved)", "agent:prr-review#1 (request-changes)", "agent:prr-review#2 (approved)"]
created: 2026-09-23
last-reviewed: 2026-09-24
tracking: "https://github.com/Zzzode/Loom/issues/1"
---

# RFC 0001 — Module Architecture Target Shape

> CI context at time of writing: macos-14 cold build passes in ~80 min at
> default 3-way Ninja parallelism after the BMI-slimming series; this RFC is
> the follow-up that addresses the *structural* findings exposed by that work.
> It extends (does not retract) the "cc_ui is one target today" note in
> `CLAUDE.md` — it states what must change before splitting is possible.

## 1. Summary

Loom uses C++23 named modules correctly at the **topological** level — the
849-module graph is an acyclic DAG with zero module-level cycles. But modules
are currently used as "faster headers": 96% of interface units contain
function definitions, the standard library and FTXUI still enter via textual
includes inside global module fragments, and two clusters of *directory-level*
cycles (the known UI9 and a newly found Core8) prevent splitting the build
into independently linkable libraries.

This RFC proposes the target architecture and a six-phase, independently
shippable path to it. Each phase is measurable (BMI PSS, fan-out, wall time),
reversible, and does not require serializing the build.

## 2. Motivation

### 2.1 Evidence — measured current state (graph analysis, 2026-09-23)

| Metric | Measured | Best practice |
|---|---|---|
| Interface units (`.cppm`) | **849** | — |
| Module implementation units (`module cc.x;` `.cpp`) | **32** | interface ↔ implementation balance |
| Interfaces containing function bodies | **816 / 850 export module units** (849 module primaries + the `cc.ui.app.app:impl` partition) | declarations in interfaces, bodies in impl units |
| God interfaces (inline defs) | agent_runtime **498**, agent.utils **439**, query_engine **433**, repl_screen **321**, messages_list **321**, runtime_registry **316** | tens, not hundreds |
| Module-level cycles | **0** (pure DAG) | DAG |
| Directory-level SCCs | **2**: UI9 (documented) and **Core8 (previously undocumented)** | none |
| Textual `#include`s inside interface GMFs | **8,768** (`<string>` alone in 823) | `import std;` / wrapper modules |
| Interfaces textually including FTXUI | **165** | one wrapper module |
| `cc.utils` target size | **171 modules**, 163 flat at `cc.utils.*` | sub-domain layering |
| Largest interface LOC | repl_screen 4,115; agent_runtime 3,971; messages_list 3,731; query_engine 3,383 | focused units |

### 2.2 Consequences already paid in production

1. **Memory.** A producer compiler must materialize the full closure AST.
   `app.cppm` peaked at **7.9 GB PSS**, which OOM/swap-killed macos-14 CI under
   default parallelism. The emergency series cut it to 4.4 GB via PIMPL
   erasure and a `repl_state` split — mitigation, not cure.
2. **Incremental rebuild fan-out.** Bodies live in interfaces, so editing one
   function body invalidates a BMI and recompiles its entire importer fan-out
   (touching `design_tokens` rebuilds ~62 modules). With proper declaration /
   implementation separation a body change rebuilds one object file.
3. **Unbreakable build boundaries.** The two directory SCCs make
   `cc_utils/cc_tools/cc_services` and the UI responsibility directories
   un-linkable as separate static libraries.
4. **Re-parsed third-party AST.** FTXUI template headers are textually parsed
   in **165** interface closures instead of once.

### 2.3 The diagnosis in one sentence

> The dependency topology is healthy; module *usage* is inverted — interfaces
> carry implementations, and std/third-party code enters textually. Fixing
> usage, not the topology, is where the build cost actually lives.

## 3. Target architecture

### 3.1 Layered dependency graph

Dependencies point downward only. A layer may import any layer below it; it
must never import above. A **contract** (abstract port / callback / plain
DTO) lives in the layer that declares it; the implementation importing that
contract is a legal downward edge. A module that imports a concrete service
implementation belongs ABOVE that service, never beside it.

The non-UI order (REV 3, corrected through three Tarjan reviews — see
`attachments/0001-oq3-phase-b-cut-design.md`) is:

```
cc.third_party.ftxui        ── per OQ-1 (header units preferred); import std;
        │
types / constants / cc.config.*_types / *.port *.contract ── leaves
(cc.config.config / .settings rank WITH utils — they import utils.json;
 only the *_types data leaves sit here)
        │
platform / fs / text / json / serdes / process / crypto ── from cc.utils (Phase D)
        │
state / task_types / vim
        │
hooks   ── event/callback CONTRACTS + pure hook logic only.
          Never imports a concrete services/state implementation.
        │
skills  ── definitions/loading; publishes callbacks via sinks, no tools import
        │
services ── concrete API/MCP/LSP/voice/image implementations.
           MAY implement hook/skill contracts below it; never concrete hook logic.
        │
tools   ── PURE domain tools only (bash, primitives, tool contract types in
          cc.types, registry mechanics). A service-backed tool does NOT live here.
        │
orchestration ── service-backed tools & multi-service flows:
                 agent run/resume/fork subtree, McpTool/LspTool, image-aware
                 file reads, SkillLoader + MCP-snapshot wiring
        │
query / commands
        │
ui (foundation → chrome → widgets/visual → messages/dialogs/permissions/prompt
    → screens → app)
        │
server / cli / entrypoints
```

This order is forced by the real edges: e.g. `McpTool`/`LspTool` and the
agent subtree import concrete services, so they sit above services in
orchestration; voice hooks must become pure logic over injected ports so
services can implement the port without creating a hooks↔services cycle.

### 3.2 Module discipline rules

1. **`.cppm` exports declarations only.** Definitions go in a module
   implementation unit (`module cc.x;` file), or — for genuinely `constexpr`
   / trivial accessors — remain inline by explicit justification.
2. **Named partitions are for PIMPL internals, not layering.** Internal
   partitions (`:impl`) hide state; they never become import shortcuts across
   responsibility areas.
3. **`import std;`** is the only way the standard library enters a module.
4. **Third-party code enters through exactly one wrapper module per vendor**
   (`cc.third_party.ftxui`). No textual third-party include in any other GMF.
5. **No upward edges** (§4.2), verified by Tarjan (`tools/arch/graph_check.py`,
   milestone E0). The ONLY permitted cross-rank edges are structural
   contracts detected by exact leaf segment: a leaf named `port` /
   `contract`, any `*_types` leaf (in any area), or a module under
   `cc.types.*`; extra contracts are listed in
   `tools/arch/port_allowlist.txt`. The lint enforces a total layer rank,
   not convention, and fails closed on any area missing a rank.
6. **One responsibility area per static library once its directory is acyclic
   with respect to every other area.** The single FILE_SET rule stays only
   while a real cycle remains, and the graph check documents which edges.

### 3.3 State model (UI, eventual)

`ReplScreenState` stops being one god struct holding fields for every domain
(messages, dialogs, prompt, teams, agents, voice, …). Each domain owns its
state; the screen layer composes and subscribes. `AppAdapter` becomes a thin
composition root. This is the prerequisite for breaking the UI9 SCC and is the
largest single piece of work in the RFC — therefore last and optional until
the earlier phases deliver their measured wins.

## 4. Detailed changes

> **Gating note.** Phases A and B contain design questions that have no
> answer without a code spike (Open Questions OQ-1, OQ-2, OQ-3). The steps
> below describe the expected path; affected subsections are rewritten when
> the spike closes, before that phase leaves `proposed`. This RFC stays at
> status `provisional` until OQ-1…OQ-4 are resolved and the `accepted` gate
> can be entered.

### 4.1 Phase A — std / FTXUI entry points (largest remaining compile lever)

**Prerequisite: OQ-1 (FTXUI intake) and OQ-2 (`import std;` granularity)
closed by a branch spike — no `src/` changes from the spike land on master.**

Confirmed toolchain facts (dev box, 2026-09-23): CMake 3.31.2 ships the
`CXX_MODULE_STD` mechanism; Homebrew LLVM 22 ships
`share/libc++/v1/std.cppm`; FTXUI is pinned at **v5.0.0 and contains no named
modules**.

**A1. Standard library.** Build the std BMI through CMake `CXX_MODULE_STD`
once per configuration and switch module units to `import std;`. Mixing `import std;` with textual
standard headers was assumed ill-formed, but the 2026-09-24 spike showed
clang 22 compiles such a TU successfully — conversion can therefore be
incremental per file; commits are still grouped per target in leaf →
upstream order for reviewability. The macos preset's `-isysroot` / libc++
`-isystem` / linker flags must be applied to the std BMI compile command
(exact spelling is part of OQ-2).

**A2. FTXUI.** A named-module wrapper that textually includes FTXUI in its
global module fragment **cannot re-export those declarations** — a module
never exports names from GMF textual includes. The wrapper sketch in this
RFC's first draft is therefore invalid and must not be implemented as
written. Two viable mechanisms remain (decision = OQ-1 output):

- **(preferred if the spike passes) header units** — consume FTXUI as C++
  header units (`import "ftxui/...";` / angle form), built once via the
  CMake scan/P1689 path (FILE_SET HEADERS or explicit scanned header set).
  Template instantiation semantics differ from textual inclusion, so a
  full truecolor golden diff is a mandatory gate.
- **(fallback) narrow internal wrapper implementation units** — a few
  module implementation units own the textual FTXUI include and expose only
  non-`ftxui::`-typed interfaces (our own descriptors/callbacks). The 165
  including interfaces then stop naming FTXUI types directly; bigger UI
  surface change, to be sized by the spike.

If neither mechanism shows a measured PSS/wall-time win, Phase A ships A1
only and FTXUI stays textual (recorded as an accepted residual).

**A3. Pilot.** Convert one leaf UI sub-area end to end under the chosen
mechanism before any tree-wide sweep; record producer PSS, wall time, and
(for A2) the full golden comparison.

**A4. Enforcement.** After the sweep, the architecture lint (Phase E,
`tools/arch/graph_check.py`) fails on textual standard-header or FTXUI
includes in module units, with an explicit allowlist for spike files if the
fallback was chosen.

Graduation: pilot + sweep numbers recorded; zero textual includes outside
the allowlist; truecolor goldens byte-identical or manually reviewed;
app-closure producer PSS does not regress and decreases on at least one of
the two mechanisms.

### 4.2 Phase B — break the Core8 SCC (small, sharp, newly found)

> **Authoritative design:** [OQ-3 Phase B cut design REV 3](attachments/0001-oq3-phase-b-cut-design.md).
> The table below is the original sketch and is superseded by the attachment's
> 14-family / 52-edge / 9-singleton design (REV 3). Kept for context only.

**Prerequisite: OQ-3 (orchestration boundary and port shape) closed.** The
edge table is evidence; the cut designs are not. Before implementation the
RFC must name, per edge: the port interface, the module owning it, and the
composition-root injection site.

Verified backward-edge inventory (2026-09-23 graph analysis):

| Backward edge | Edges | Direction of the fix (design pending OQ-3) |
|---|---|---|
| `services.streaming_executor → tools.tool` | 1 | choose ONE: executor moves above tools, OR a tool-execution port is defined in `tools` — not left ambiguous |
| `utils.ide_integration → services.mcp` | 2 | relocate into `services` (misfiled; no abstraction needed) |
| `skills.bundled.debug → tools.tool` | 1 | invert / isolate behind a port; the debug convenience may become a test seam |
| `state.teammate_view_helpers → task_types` | 1 | sink the shared type below both, or merge modules |
| `tools.mcp → hooks / config` | 2 | callback inversion: tools exposes a registration port; hooks/config supply it |
| `hooks.{voice,prompt_suggestion,turn_diffs,assistant_history} → services/state` | 9 | port interfaces owned by `hooks`; implementations live in services, injected at the composition root |
| `tools.agent.{run,resume,fork}` (12 agent_* modules transitively) → services.api / skills | 36 | promote the orchestrating subset to a new `cc.orchestration` layer ABOVE services; exact membership (which of run/resume/fork/runtime/display/memory move) is the OQ-3 deliverable |
| `config → utils.json` / `state → utils` | downward, benign | none |

Layering clarification: `hooks` owning port interfaces below, with
`services` implementing them above, is dependency inversion — not an upward
edge. The lint must encode this structurally (edges into an explicit
`*.port` / contract module are allowed) rather than via a blanket whitelist.

Graduation: Tarjan over the layered nodes (incl. the new orchestration) reports 9 singleton SCCs (Core8 areas + orchestration); `cc_utils`/`cc_tools`/`cc_services`
link as independent static libraries; `graph_check.py` fails CI on any new
non-port back edge; ctest total unchanged.

### 4.3 Phase C — move bodies out of god interfaces

Attack the 55 interfaces over 1,000 LOC in descending measured cost
(agent_runtime 498 inline defs, agent.utils 439, query_engine 433,
runtime_registry 316, repl_screen renderers 321, messages_list 321,
text_input 282, …). Same mechanical pattern proven by the `AppImpl` work:
declarations stay in the `.cppm`, bodies move to module implementation
units, hidden state goes behind internal partitions or type erasure.

| Batch | Targets | Entry criterion | Exit criterion |
|---|---|---|---|
| C1 | top 6 god interfaces | baseline inline-defn counts recorded | only trivial accessors remain inline (< ~30/interface) |
| C2 | remaining >1000-LOC interfaces | C1 green | no batch interface > 100 inline defs |
| C3 | long-tail sweep | C2 green | lint threshold (inline defs/interface) enforced; threshold value chosen from the measured post-C2 distribution |

Graduation per change: debug + release `-Werror` green, ctest green, and for
each: producer BMI PSS, importer fan-out, and "edit one body → number of
recompiled objects" recorded; that last number trends to 1. No behaviour
change.

### 4.4 Phase D — dissolve the `cc.utils` junk drawer

171 modules, 163 flat. Re-home by domain:

- `cc.json` / `cc.yaml` / `cc.text` / `cc.fs` — serialization and text/file
- `cc.process` — bash execution, abort controller, async primitives
- `cc.crypto` — hashing / encoding
- `cc.platform` — clipboard, terminal helpers, env, paths
- misplaced modules move to their real area (ide_integration → services;
  swarm_* → the teams domain)

`json` (fan-in 121) and `error` (fan-in 59) stay pure leaves.

A complete **module to destination mapping table is a deliverable
attached to this RFC before implementation** (mechanical, but reviewed -
name prefixes mislead: `hyperlink` is terminal escapes not HTTP, `cwd`
is an FS primitive, `content_array` is wire blocks).

Graduation: mapping table fully executed; zero new flat
`cc.utils.<thing>` modules (lint-enforced, frozen exception list
allowed); module names stay stable - moves are CMake path updates,
importers do not change.

### 4.5 Phase E - build-system hardening and architecture lint

- **Compiler cache (spike-gated, not assumed).** sccache is present on the
  dev box, ccache is not; named-module/BMI caching is still maturing in
  both tools and CMake 3.31. Phase E starts with a spike proving a warmed
  cache reuses BMI/object output for an unchanged interface. Only on
  success is GHA cache wiring added. If it fails, E ships the lint only and
  the warm-cache goal is deferred with a recorded reason (OQ-5).
- **Architecture lint.** Promote the ad-hoc audit Tarjan script to
  `tools/arch/graph_check.py` (committed, not session-only). It fails CI on:
  (a) any new directory-level SCC edge except into an explicit `*.port`
  contract module; (b) textual FTXUI/std includes in module units outside
  the Phase A allowlist; (c) new god interfaces above the Phase C
  threshold. It runs in a lightweight workflow, never behind the
  80-minute macos build.
- **Reproducible measurement.** Commit the `/proc` PSS sampler under
  `tools/arch/` with usage docs so phase evidence is reproducible.
- Keep default Ninja parallelism. Memory is solved by splitting TUs, not
  throttling jobs (standing project rule).

### 4.6 Phase F - UI state sharding and the UI9 SCC (last, separate RFC)

Only after A-E have landed and their numbers are in. Split
`ReplScreenState` into domain-owned stores and reduce `AppAdapter` to a
composition root. The follow-up RFC must state, per known UI9 back edge
(messages/prompt, foundation/chrome, dialogs/widgets, ...), which
state-ownership change severs it.

**Objective completion invariant (F's "done" definition):**
`graph_check.py` reports all nine UI responsibility directories as
singleton SCCs (UI9 becomes 9 size-1 components); `cc_ui` splits into
area static libraries that link without a cycle; the full truecolor
golden suite and ctest total stay green. F is not claimed complete until
that invariant is measured. Its design and gating live in a follow-up
RFC; this RFC defines only the hand-off boundary and acceptance
invariant.

## 5. Goals

- G1. Producer BMI PSS for the top-6 god interfaces (batch C1) decreases
  versus per-interface baselines recorded in the phase table, with the
  numeric target fixed at the implementable gate; after C1, editing a
  function body recompiles exactly one object file (fan-out = 1) for the
  converted interfaces.
- G2. The module graph contains no directory-level SCC larger than one
  responsibility area; the invariant is enforced in CI.
- G3. Standard library enters module units only via `import std;`; FTXUI
  enters via the mechanism selected by OQ-1 (header units preferred),
  with textual includes confined to an explicit lint-allowlisted set.
- G4. Non-UI targets (`cc_utils`/`cc_tools`/`cc_services`) become
  independently linkable static libraries.
- G5. `cc.utils` is re-homed into sub-domain areas; new flat
  `cc.utils.<thing>` modules are prohibited.
- G6. macos-14 cold builds stay green at default parallelism with no swap
  (achieved 2026-09-23). ~~A single-digit-minute warm-cache PR build is a
  CONDITIONAL goal only: it stands if a BMI-capable compiler cache is proven
  on CI per OQ-5 (ccache 4.x / newer sccache); otherwise it is explicitly
  dropped, never claimed.~~ **MET 2026-09-26 (commit 057ad49):** sccache
  0.17+ caches named-module output on macos-14 — warm build 1m33s
  (100% of 1185 requests hit, 0 non-cacheable); evidence in §12 and the
  resolved OQ-5.

## Non-Goals

- **No return to header files.** Headers force every consumer TU to re-parse
  the full closure; modules parse it once and lazily deserialize. Consumers of
  the fat closure measured 1.5–2.7 GB PSS vs 4.4–5.4 GB for producers — that
  gap only exists with modules.
- **No forced serialization or -j throttling** to manage memory.
- **No premature `cc_ui` split** before Phase F's state work; splitting a real
  SCC fails at link time.
- **No rewrite of UI internals** outside the explicit state-sharding phase.
- Module *names* remain decoupled from file paths; directory re-homing is a
  CMake change, not an importer rewrite.

## 6. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Header-unit / `import std;` path exposes a Clang 22/scan-deps bug, or per-TU atomic switch cascades | OQ-1/OQ-2 spike on a branch decides mechanism and switch order before master changes; FTXUI textual fallback remains an accepted, lint-allowlisted residual if the spike loses |
| Moving 300+ function bodies breaks subtle inline/ODR behaviour | one god interface per commit; dual-preset + full ctest gate; no semantic edits |
| Port inversion for hooks/agent orchestration changes construction order | composition root is already concentrated in `AppAdapter` constructor; wire there, covered by runtime/e2e tests |
| `cc.utils` rename churn | module names stay stable (path-decoupled); move files, update CMake only |
| Phase F UI rework regresses rendering/goldens | deferred and separately RFC'd; truecolor golden suite is the guardrail |

## 7. Success metrics

- macos-14 cold build well inside the CI window **at default parallelism**,
  with no swap (already achieved once at 80 min; target comfortably lower
  after Phase A/C).
- No directory-level SCC larger than one area (graph lint green).
- Top-10 god interfaces reduced from 200–500 inline definitions each to
  declaration-only interfaces; editing a body recompiles one object file.
- Zero textual std/FTXUI includes in module units.
- Incremental CI with warm cache in single-digit minutes — **ACHIEVED 2026-09-26** (sccache 0.17+, warm build 1m33s on macos-14; commit 057ad49, evidence §12/OQ-5).
- `cc_ui` splittable into area libraries (only after Phase F).

## 8. Alternatives considered

1. **Keep the status quo and just raise CI runner size.** Buys wall time,
   fixes none of the fan-out/SCC/third-party re-parse cost, and pays forever.
2. **Revert to traditional headers.** Strictly worse for a codebase shaped as
   one fat closure plus dozens of consumer TUs (per-TU full re-parse, lazy-AST
   advantage lost, incremental fan-out worse).
3. **Split `cc_ui` immediately.** Impossible today: UI9 is a real cycle and
   the link would be circular; requires Phase F first.
4. **Generate one umbrella module per area.** Already mostly avoided (only two
   umbrella modules exist); expanding that pattern would widen every closure
   and undo the slimming work.

## Phases and graduation criteria

The authoritative per-phase scope, measured baselines and graduation
criteria are: the §9 milestone table below, §4.1-§4.6 detailed changes,
the OQ-3 REV 3 attachment (Phase B edge families and the 9-singleton
invariant), and the OQ-4 attachment (Phase C numeric baselines and the
Phase D module mapping). Each phase moves to `done` only when its
measured evidence is recorded in §12.

## Rollout and rollback

Phases ship in the §9 order with E0 first; each is an independent,
revertible commit series. Interface changes use PIMPL / type erasure /
re-export shims (no importer flag-days), and Phase D keeps module names
stable so moves are CMake/path edits. MCP config unification preserves a
reader for old persisted settings. Per-phase rollback detail is in the
PRR (§11) and in the OQ-3 attachment sequencing.

## Testing and verification plan

Every phase: debug + release `-Werror` and serial ctest green, with the
ctest total reconciled exactly on deletions (baseline 1706 @ 2026-09-23);
PSS/fan-out/wall-time evidence recorded; truecolor goldens compared for
Phase A; old-shaped MCP config load test for Phase B; macos-14 CI for
A/B. The graph invariants are enforced by `tools/arch/graph_check.py`
(current gate now; `--target-core8` = 9 singleton SCCs gates the Phase B
merge).

## 9. Sequencing summary

| Phase | Milestone | Scope / graduation | Risk | Expected build payoff |
|---|---|---|---|
| **E0** | `tools/arch/graph_check.py` + allowlist + **frozen baselines** (upward-edge + dead-import snapshots, fail-on-addition) in CI (lands FIRST; A/B graduate against it) | low | structural enabler |
| **A — done (2026-09-25)** | `import std;` (incremental per file); FTXUI header units REJECTED on clang 22, stay textual (reopen at clang ≥23 + cmake 4) — macos-14 + dual-preset green, evidence in §12 | med (spike-gated) | high |
| B | break Core8 per the REV 3 cut design (14 families, 52 edges, hooks/services reordered, service-backed tools lifted, family-14 type sinks), split non-UI libs -> 9 singleton SCCs | med–high | medium + structural |
| **C — done (2026-09-26)** | bodies out of god interfaces — 10 batches; all 6 C1 <30, **0/849 interfaces >100**, ratchet in CI; evidence in §12 (app.app deferred) | low (mechanical) | high, incremental |
| **D — done (2026-09-26)** | re-home `cc.utils` per OQ-4 — 115 zero-content renames into ~30 domain dirs, zero flat modules lint-enforced; module names unchanged; evidence in §12 (cross-target/name migration deferred) | low | low–medium |
| **E — done (2026-09-26)** | PSS/BMI sampler `tools/arch/measure_bmi.py` (35bbf48) + sccache named-module cache wired into macos-14 CI (057ad49): pilot cold 1185/1185 cached with **0 non-cacheable** (8m16s), warm **1185/1185 = 100% (1m33s)**, impl edit 4 misses, leaf-interface edit 338 misses (full importer closure, zero stale); master cold-population reproduced 1184/1184 / 0 non-cacheable (12m01s); evidence in §12 — G6 MET | low–med | warm PR build 1m33s; cold unchanged (~12m) |
| F | UI state sharding, break UI9, split cc_ui | high | structural (own RFC) |

Order: E0 -> (A, C, D may proceed) -> B (gated on the graph_check
singleton prediction) -> E PSS sampler + cache (**done 2026-09-26**) -> F. F awaits a
dedicated RFC after measured results from A–E.

## 10. Open questions

These gate the `accepted` / `implementable` transitions. None is answerable
without a branch spike or an explicit design decision; they are recorded
here so the gate cannot be passed on assertion.

| ID | Question | Owner | Blocks | Acceptance of the answer |
|---|---|---|---|---|
| OQ-1 | ~~How does FTXUI enter module units?~~ **Spike 2026-09-24: mechanism works in isolation. Phase A 2026-09-25: REJECTED at scale on clang 22 — FTXUI stays textual.** The 2026 spike built individual FTXUI v5.0.0 headers as user header units (`-fmodule-header=user`); they compile. But a 5-agent compile-experiment investigation during the actual `import std;` migration proved header units do NOT work end-to-end on clang 22.1.8 + CMake 3.31: (a) per-header HUs each textually compile libc++, so a reduced-BMI closure importing several HUs + the named std module keeps MULTIPLE attached `operator new` candidates (the ambiguity is not deduplicated); (b) a single monolithic umbrella HU that dedups them deterministically **SIGSEGVs clang 22** in `ASTReader` when real header spellings are mapped; (c) consumers mixing HUs with the reduced-BMI project modules show std type splits (`std::format` not viable, `weak_ptr` unreachable); (d) CMake 3.31 rejects `FILE_SET ... TYPE CXX_MODULE_HEADERS` (needs CMake 4). FTXUI therefore remains a **textual GMF include** for all UI units; it is the documented lint allowlisted residual. Reopen after clang ≥23 (with PR #179178) AND CMake 4 on the runners. | @Zzzode | deferred | When toolchain reaches clang 23 + CMake 4: one-leaf HU pilot with truecolor goldens and PSS comparison. |
| OQ-2 | ~~Exact CMake recipe for `import std;`?~~ **Spike 2026-09-24: RESOLVED on the local toolchain.** (1) CMake 3.31 `CXX_MODULE_STD` exists but is gated behind an experimental UUID and builds std with `-std=gnu++23`, mismatching this repo `CXX_EXTENSIONS=OFF` (c++23) — rejected. (2) The robust path is vendoring the shipped `std.cppm` as an ordinary FILE_SET CXX_MODULES target, compiled with `-fno-implicit-module-maps -Wno-reserved-module-identifier` and an include dir at the toolchain `share/libc++/v1` (for `std/*.inc`); works at c++23 with ext OFF, CMake 3.28+, and carries whatever flags the preset already sets (so macos isysroot/libc++ flags flow naturally). Consumer micro-benchmark: a 10-header heavy TU **1.71 s -> 0.13 s (13x)**; std BMI precompiled once (35 MB). (3) **CORRECTED during Phase A 2026-09-25:** a unit must NOT mix textual pure-C++ libc++ headers with `import std;`. It compiled in one micro-test, but at tree scale it causes (i) `operator new` ambiguity in the cc_ui app impl units (LLVM #184957), and (ii) >500s compile explosion in non-module test TUs (245s→>500s; converting to pure `import std;` → 27s). The transform therefore removes pure-C++ headers whenever it adds `import std;`, keeping only C/POSIX/third-party textual headers; non-module TUs (tests, main) are also converted. The 3 app impl units + impl_bash are the deliberate textual-std exceptions (compiler-bug workaround). macos build of the same vendored std.cppm to be confirmed on CI. | @Zzzode | Phase A | vendored std module target builds under both presets; one leaf target converts and dual-preset + macos CI pass; 13x micro result reproduced inside a real producer TU before sweep. |
| OQ-3 | ~~Orchestration boundary and port shape?~~ **Design REV 3, 2026-09-24** ([attachment](attachments/0001-oq3-phase-b-cut-design.md)). Two adversarial Tarjan reviews drove it from REV 1 to REV 3: hooks/services reordered (voice logic behind hooks-owned ports, notifs MCP bridge moved to orchestration, 4 dead hook imports deleted); all **52** live upward edges owned across 14 families; service-backed tools (Mcp/Lsp/image file/SkillLoader) and the 25-edge agent subtree lifted to orchestration; family 14 sinks AgentConfig/permission DTOs to agent_types and moves only spawn_multi_agent/runtime_team_shared up, so the reverse tools->orchestration edges are cut. Completion invariant: **9 singleton SCCs** including orchestration. McpServerConfig x6 unified as a persisted-data leaf; ToolInput split so its json helper stays in tools. | @Zzzode | Phase B | Before bodies move: port/type modules compile and graph_check predicts 9 singleton SCCs. |
| OQ-4 | ~~Numeric baselines and utils mapping?~~ **Artifact attached 2026-09-24:** [OQ-4 baselines + mapping](attachments/0001-oq4-baselines-and-utils-mapping.md). Phase C: all 55 >=1000-LOC interfaces measured (8,950 inline defs; C1 top-6 = 2,328); concrete exits C1 <30/interface, C2 none >100, C3 lint warn-40/error-80. Phase D: all 171 utils modules classified into ~60 destination areas, with the 5 ambiguous ones content-read and resolved (image_store -> cc.media.images; pdf retained leaf; prompt_category is a deletion candidate with zero source importers; system_theme -> cc.platform.terminal; theme -> cc.ui.theme.types data leaf). | @Zzzode | implementable gate | Artifact exists and mapping reviewed; lint freeze-list produced during Phase D execution. |
| OQ-5 | ~~Does sccache cache named-module output?~~ **Spike 2026-09-24: NEGATIVE with the available tool.** sccache 0.4.0-pre.6 (the only such tool on the offline box) reports **"unknown source language" and non-cacheable for `.cppm` BMI compiles**; module implementation units execute but are not stored (1 executed, 0 hits/0 misses). Plain `.cpp` files DO cache (warm hit confirmed). Since the expensive outputs are exactly the `.cppm` producers, sccache gives no benefit for the cost that matters. | @Zzzode | Phase E / G6 | Options for implementable: (a) verify a newer sccache or **ccache 4.x** (not installed offline; testable via brew on CI) caches `.cppm`; (b) until then ship the architecture lint only and DROP the single-digit warm-cache claim from G6 - do not fake it. BMI-level caching may instead come from a future compiler-native/CMake module cache. **RESOLVED 2026-09-26 (option a):** Homebrew sccache **0.17.0+** understands cmake 4's quoted response/modmap files and caches the `.cppm` producers — proven on macos-14 via the `cache-pilot` branch (PR #2, closed after merge of the production wiring): cold 1185 requests / 0 hits / **0 non-cacheable** (first execution, run 36220055700), then on rerun **warm 1185/1185 = 100%, build 8m16s → 1m33s**; an impl-unit edit missed only 4 (edited TU + downstream, run 36221411055); a leaf-interface edit missed 338 = producer + full importer closure with **zero stale hits** and 1706/1706 (run 36221834986). Production wiring merged in 057ad49 (`CMAKE_{C,CXX}_COMPILER_LAUNCHER=sccache` + actions/cache, ≥0.17 gate, always-on stats, no `-j` change); the master cold-population run 36223064718 reproduced 1184/1184 cached / 0 non-cacheable / 1706/1706 (1184 because cc.hooks.voice_hooks was deleted in a253b81). The offline box stays on 0.4.0-pre.6 and must not enable the launcher; that build additionally **served stale objects over changed BMIs**, confirming the hard ≥0.17 floor. G6 MET. |

## 11. Production Readiness Review

Filled for the implementable gate against `.claude/skills/rfc/templates/prr-checklist.md`.
Phase-owning agent reviews each row again when the phase ships.

**Correctness & tests**
- Every behavior-changing family gets new/updated tests; the serial ctest
  total is reconciled on every deletion (baseline 1706 @ 2026-09-23).
- Phase A header units can change FTXUI template instantiation → full
  truecolor golden comparison is a hard Phase A graduation item (not
  assumed byte-identical without the run).
- Phase B moves keep `messages.jsonl` / `dump-prompts` output identical
  (no wire or trace shape changes); MCP persisted-settings round-trip
  (6 struct copies unified) needs an old-shaped-config load test.
- Phase D deletes `cc.utils.prompt_category` together with its only
  consumer tests/test_utils.cpp (5 assertions); the ctest delta is exact.

**Build system**
- E0 `tools/arch/graph_check.py` lands first; current gate (module DAG +
  no new non-contract upward edge + no new dead import, each versus a
  frozen fail-on-addition snapshot in `tools/arch/*_baseline.txt`) runs in
  the lightweight arch-check workflow; `--target-core8` is the Phase B
  merge gate and must print 9 singleton SCCs before bodies move. The dead
  -import analysis is per translation unit (impl units sharing a module
  name are keyed by file) and distinguishes naming from transitive
  reachability; intentional imports are silenced with
  `// arch-check: keep-import`.
- All work at default Ninja parallelism; memory reduced by TU
  splitting/type erasure, never `-j` caps.
- Phase C only decreases inline-definition counts; the C3 lint threshold
  (warn 40 / error 80 proposed) is set from the measured post-C2
  distribution before enforcement.
- Phase A: zero new textual std/FTXUI includes outside the allowlist.

**Rollback**
- Each numbered family / batch is an atomic commit, independently
  revertible; module NAMES stay stable during Phase D (path-decoupled),
  so a revert is a CMake/path change, not an importer rewrite.
- Interface changes go through the proven PIMPL / erasure / re-export
  shim patterns (no importer flag-days).

**Observability**
- Session traces and dump prompts remain valid across B/D (pure
  structural moves); no new ad-hoc print paths.
- PSS sampler committed under tools/arch in Phase E so all phase
  evidence is reproducible.

**Documentation**
- CLAUDE.md updated when targets/layering change (cc_ui single-target
  note when Phase F splits it; cc_orchestration target added in B).
- New non-obvious constraints appended to docs/decisions/design-decisions.md.

**Deletion / deprecation**
- Dead code deleted in-phase: the 4 dead hook imports (B family 1),
  prompt_category (D), obsolete McpServerConfig copies (B family 9).
- The 6→1 MCP config unification keeps a reader for the old persisted
  shape rather than silently dropping user settings.

**Platform**
- Every phase dual-preset (local-linux debug+release) `-Werror` and
  serial ctest green; Phase A/B additionally verified on macos-14 CI
  (isysroot/libc++ flags, header units under CI cmake 4).

PRR reviewers (agent): `agent:prr-review#1` — request-changes on the first
E0 submission (see §12 correction rows); `agent:prr-review#2` —
**approved** the redesigned frozen-snapshot gate after adversarial
re-verification (2026-09-24). The implementable gate is cleared.
2026-09-24; re-run per phase at implementation.

## 12. Implementation History

Append-only.

| Date | Event | Outcome / evidence |
|---|---|---|
| 2026-09-23 | RFC opened (provisional), graph audit of 849 modules | PSS and SCC measurements recorded in section 2; rfc_lint green |
| 2026-09-23 | First design review against the `accepted` checklist | REQUEST CHANGES: FTXUI wrapper sketch invalid (GMF includes are not exported), import-std per-TU constraint missing, Phase B port design unspecified, Phase F lacked a completion invariant, sccache assumed; G1/G3 not falsifiable. Logged as OQ-1..OQ-5 and sections 4.1-4.6 rewritten. Status deliberately remains `provisional`. |
| 2026-09-24 | Branch `rfc-0001-spike-import-std-ftxui` spikes OQ-1/OQ-2 (no `src/` changes on master) | Both mechanisms proven on clang 22/cmake 3.31: vendored std.cppm FILE_SET target works at c++23 (consumer 1.71s->0.13s, mixed TU tolerated); FTXUI headers compile to header units; end-to-end header-unit consumption deferred to CI cmake 4. OQ-1/OQ-2 updated in place. OQ-3/OQ-4/OQ-5 still open. |
| 2026-09-24 | OQ-3 design produced from the live graph (`attachments/0001-oq3-phase-b-cut-design.md`) | Agent cluster mapped by real symbols: 5 upward-importing modules (run/resume/fork/utils + the `cc.tools.agent` facade) have zero external importers -> promote to a new cc_orchestration target; 7 store/display/memory agent modules stay in tools. Other 7 edge families given per-edge cuts; 3 hooks->state imports proven DEAD (removed, full build green, then reverted pending Phase B). OQ-4/OQ-5 still open; status remains provisional. |
| 2026-09-24 | Independent agent design reviews #1 and #2 | request-changes. #1 found a hooks↔services cycle from the voice port + uncut notifs→mcp bridge, 4 dead hook imports, 3 voice edges, and 11 unaddressed service-backed-tool edges. #2 (over 9 nodes incl. orchestration) found the REV-2 fix still left a tools↔orchestration SCC (4 team/spawn modules import the moved facade), family-12 count 5 not 1, total 52 not 48, McpServerConfig 6. Both verified by the reviewer’s own Tarjan run; REV 2 then REV 3 attached. |
| 2026-09-24 | E0 produced: `tools/arch/graph_check.py` + `.github/workflows/arch-check.yml` + port_allowlist. Current gate (module DAG, no new non-contract upward edge) passes today; --target-core8 correctly FAILs today and prints the Core8 SCC (the Phase B gate). Tracking issue #1 opened. PRR §11 filled and passed agent prr-review#1. | graph: 849 modules, 0 cycles, Core8 SCC present as expected; lint green. |
| 2026-09-24 | **Status -> implementable.** E0 graph_check + CI + allowlist, tracking issue #1, PRR §11 filled (agent prr-review#1 approved). Phases A-F may start in the §9 order; Phase A still carries the CI-only confirmations (FTXUI cmake-4 header units, macos vendored std.cppm), Phase B merges only when --target-core8 prints 9 singleton SCCs. |
| 2026-09-24 | Independent agent design review #3 (full re-graph, 850 .cppm + 34 .cpp, 1,825 internal edges) | **APPROVED.** Simulated all 14 families over 9 layered nodes = 9 singleton SCCs; live upward total exactly 52; all feasibility claims verified (agent DTOs std-only, ToolInput json split, voice ports leak no concrete types). Editorial non-blockers (stale REV-2/48/8 strings, runtime_team_shared optional move, test-seam note, config unique pair) applied. **Status -> accepted.** Implementable gate still requires: tracking issue, PRR fill, graph_check predicting 9 singleton SCCs, and CI confirmation of the FTXUI cmake-4 + macos import-std spikes. |
| 2026-09-24 | OQ-4 artifact attached (55-interface C baselines, 8,950 inline defs; all 171 utils modules mapped, 5 ambiguous ones resolved incl. one zero-importer deletion candidate). OQ-5 spiked: sccache 0.4.0-pre.6 cannot cache `.cppm` BMI ("unknown source language") - warm-cache goal contingent on ccache 4.x/newer sccache being verified on CI, else dropped from G6. All five OQs now have an answer/artifact; the design (accepted) gate is reviewable, status still provisional pending reviewer approval and the CI-gated FTXUI/import-std confirmations. |
| 2026-09-24 | **CORRECTION to the two rows above re: agent prr-review#1.** The implementable gate was NOT approved at first E0 submission: prr-review#1 returned REQUEST-CHANGES — (B1) the default run exited 0 despite detecting 15 non-contract upward edges (short-circuit in the pass predicate), and (B2) E0 promised a dead-import check that did not exist. The gate was therefore redesigned as two frozen fail-on-addition snapshots (`upward_edge_baseline.txt`, 23 edges incl. 8 pre-RFC migrations backlog; `dead_imports_baseline.txt`) and a per-TU dead-import detector. Dead-import precision was validated by two adversarial agents that removed disputed imports and compiled the real TU with clang-22: round 1 = 14/15 correct (fixed: findings must be keyed per translation unit); round 2 = 14/15 correct (fixed: namespace aliases declared in sibling units + open-namespace ownership sets + deep-only alias paths); the second round also ran a full debug rebuild + `ctest -j1` = 1706/1706. Snapshot frozen at 153. |
| 2026-09-24 | **agent prr-review#2 — REQUEST-CHANGES on the redesigned gate.** B1 reopened: line-oriented parsing made legal C++ spellings invisible (`import\n x;`, `import /*c*/ x;`, backslash splices, `export /*c*/ import`, comment inside the module declaration dropped the whole TU), all compile-verified by the reviewer; plus unranked areas silently skipped. Fixed: module/import extraction now runs on comment-stripped, phase-2-splice-joined text with whole-text multiline regexes; the gate fails closed on unranked areas; all five evasion forms re-tested and caught; graph results identical (849 modules, 23 edges, 153 dead, 0 cycles). B2 (this correction entry + §11/§3.2 wording). |
| 2026-09-24 | **agent prr-review#2 — APPROVED (implementable gate).** Re-verified the fixes adversarially: all five original disguise forms plus seven extra (keyword/name/export split by splice, block comments, CRLF, disguised cycle, raw-string with fake import lines) caught and partly compile-verified; fail-closed ranking works in both output modes; extraction drift across 933 live files = zero mismatches; `--target-core8` still fails today with the Core8 SCC. Reviewer accepted the false-negative-only dead-import body bias for E0 (documented; tightening needs a fresh compile-verified snapshot). E0 is cleared; phases A–F may start in §9 order. |
| 2026-09-25 | **Phase A START → DONE (`import std;`; FTXUI header units deferred).** Orchestrated under ultracode: a 44-agent read-only audit (883 units; 872 convertible; 0 `__cpp_lib_*` feature-test macros) fed two deterministic transforms. Result: **919 files changed, +2,186/−7,891, 915 TUs on `import std;`**. New `cc_std` target locates and compiles the toolchain `std.cppm` (configure-time discovery, staged into the build tree; no vendored generated file). Compile-driven fixes: ~200 files regain explicit C/POSIX headers for macros/global C symbols (`import std;` supplies neither) — C types (size_t/uint64_t/FILE), global functions (`::tolower`, setenv, localtime_r, WIFEXITED); structural repairs for duplicate `module;` markers and named imports misplaced in the GMF. **Compiler-defect work (clang 22.1.8, LLVM #184957, fixed main PR #179178, absent in 22.1.x):** a 5-agent compile-experiment workflow disproved global `-fno-modules-reduced-bmi` (one golden TU spans >30 min at 100% CPU), per-target full BMI (consumer SIGSEGV from mixed BMI flavors), and FTXUI header units (multi-HU operator-new dup / umbrella-HU clang SIGSEGV / CMake 3.31 rejects CXX_MODULE_HEADERS). Resolution: keep reduced BMI globally; 3 `cc.ui.app.app` impl units use textual std (legal for impl units); `impl_bash.cppm` GMF environ helper rewritten to a C-only accessor so the unit can `import std;`. **Non-module TUs (49 test TUs + `main.cpp` + the benchmark main; the 3 `tests/e2e/*` binaries stay textual by design at `cxx_std_20`) also converted** — mixing textual std + project modules there cost 245s→>500s on `test_dialog_toolpermission`; pure `import std;` → **27s**. Standalone builder (`scripts/build_tools_standalone.bash`) wired to the std BMI + libc++/glibc link paths. |
| 2026-09-25 | **Phase A measured evidence (local-linux, clang 22.1.8, `-Werror`).** Clean debug build 0 error; clean release (`-O0 -DNDEBUG`) build 0 error; **`ctest -j1` 1706/1706 on BOTH presets**, incl. every truecolor/golden snapshot (UI rendering byte-identical → zero behavior change). Heaviest producer `ui/app/app.cppm` **peak PSS 3,222 MB** under reduced BMI (prior post-/goal reduced peak 4,426 MB → improved; 3-way mac parallelism ≈ 9.7 GB < 14 GB). E0 graph gate green (0 new upward edges; 18 dead imports deleted, 3 compiler-verified-needed imports annotated `// arch-check: keep-import` for cross-unit-alias/comment-masked cases). FTXUI header-unit CI pilot REMOVED from Phase A scope per the 2026-09-25 OQ-1 rejection; pending clang ≥23 + CMake 4. macos-14 full CI confirmation remains the only unverified item before the Phase A commit is declared done on CI. |
| 2026-09-25 | **Independent final implementation review (`prr-final-review#3` → REQUEST-CHANGES; `#3b` → APPROVE).** #3 found two blockers invisible to local-linux: (B1) seven TUs called unistd-only symbols (`close/pipe/fork/dup2/execl/_exit/read/write/getpid`, `STD{IN,OUT,ERR}_FILENO`) after the transform dropped `<unistd.h>`, compiling locally only via glibc transitivity — restored in `daemon_client`, `headers_helper_impl`, `bash_execution`, `ide_integration`, `terminal_size`, `xaa_idp_login`, and `test_enterprise_auth.cpp`; (B2) `scripts/build_tools_standalone.bash` regressed the macOS branch (`xcrun --show-sdkPath`) — restored to `--show-sdk-path`, shebang corrected to bash, stale impl_bash comment fixed. The reviewer then ran its own tree-wide C/POSIX ownership audit (~926 TUs, HEAD-vs-now) confirming no other Darwin header gap, syntax-compiled all seven TUs, and re-ran the standalone script → **APPROVE**. Re-verified locally after the fixes: debug+release builds 0 error, `ctest -j1` **1706/1706 on both presets** (debug 93 s, release 109 s). Note: ctest registers 1708 tests (2 gtest DISABLED), so the summary denominator is "out of 1706" — reconciles the recorded baseline. |
| 2026-09-25 | **macos-14 CI failures (commits 9bc0121, 936c7a7) — two further Darwin/cmake-only defects; fixed before declaring Phase A done.** (1) **cmake 4 synth-BMI rule:** Homebrew cmake 4.x compiles module BMIs through a synthesized `target@synth` precompile rule that drops the target's PRIVATE `target_compile_options`, so the global `-Werror` promoted clang 22's `-Wreduced-bmi-output-overrided` while compiling `std.cppm` (local cmake 3.31 uses a scanned-object rule that inherits the flags → invisible locally). Fix: pin `-Wno-reserved-module-identifier -fno-implicit-module-maps -w` on the staged std.cppm via `set_source_files_properties(COMPILE_FLAGS)`, which reach every compile on both cmake 3.31 and 4.x. (2) **Darwin `stdout`/`stderr` macro collision:** glibc exposes these as `extern FILE*`, but Apple's `<stdio.h>` `#define`s them (`__stdoutp`/`__stderrp`); with the new `import std;` consumers the member-access spelling diverged between the declaring BMI and `statusline_runner`, yielding "no member named 'stdout' in HookCommandResult". Renamed the two affected struct fields (`HookCommandResult`, `BashToolOutput`) `stdout/err`→`out/err` across all C++ consumers (the `tests/e2e` TUs already used the `stdout_` workaround; `process.stdout` JS-in-string sites untouched). Local dual-preset builds stay green; macos-14 full build + ctest remains the closing gate. |
| 2026-09-25 | **Phase A DONE — macos-14 CI green (run 36052184220, attempt 2, commit dacb24e).** First attempt died after ~53 min in Build with "runner lost communication" (host killed, no compile error, logs lost with the runner); attempt 2 passed clean → the first was infra flake, not a deterministic OOM. Measured on macos-14 (clang 22.1.8 arm64, cmake 4.x, `-Werror`, default Ninja parallelism): Configure 41 s; **full cold module build 2196/2196 with 0 errors in 12 min 40 s**; serial `ctest` **100% = 1706/1706 in 179 s** (1708 registered incl. 2 DISABLED), all truecolor/golden snapshots byte-identical. Combined with local-linux dual-preset (debug 1706/1706, release 1706/1706) and `prr-final-review#3b/#3c` APPROVE, every Phase A graduation criterion is met. Commits: 9bc0121 (migration), 936c7a7 (cmake-4 synth-BMI flags), dacb24e (Darwin stdout/stderr rename). FTXUI header units remain deferred to clang ≥23 + cmake 4 (OQ-1); Phase A ships with FTXUI textual. |
| 2026-09-25 | **Phase C START; batch 1/10 — `cc.services.mcp.client` bodies extracted.** Discovery ran a 13-agent workflow (extraction recipe from the existing 32 impl units; per-interface inventory for the C1 six + 4 C2; fresh post-Phase-A semantic baseline — same 6 C1 modules, 45/55 interfaces >100 inline; adversarial critic that picked this first batch). Split `client.cppm` 1777→354 LOC (declaration-only) into five `module cc.services.mcp.client;` impl units wired into the existing cc_services PRIVATE source list: stdio / SSE / HTTP transports + protocol + requests. Import closure shed from the interface BMI: `cc.utils.json` (yyjson) and `cc.utils.http` (httplib) leave the cppm — http is now imported only by client_http_transport.cpp, json only by protocol/requests. Three JsonVal-naming private members were demoted to anonymous-namespace free functions in client_protocol.cpp (a declaration naming JsonVal would have pinned the json import); the recursive helper now takes `client_name` and its sole caller get_prompt was co-located. Each transport has one out-of-line destructor (strong body emission; under clang modules the vtable stays with the interface unit); trivial is_connected() stays inline. **Measured: producer `client.cppm` peak PSS 354 MB → 121 MB (−66%)** on local-linux; full debug+release `-Werror` builds clean; serial `ctest -j1` 1706/1706 on both presets (incl. test_mcp_stdio, test_sse_mock, test_services); graph_check OK. Independent review (agent) **APPROVED** (byte-identical moved-body + string-literal multiset vs HEAD; default-arg/key-function/GMF audits clean). |
| 2026-09-25 | **Phase C batch 1 confirmed on macos-14 (commit ad2241c, run 36067996231).** Full module Build 8 min 34 s + serial ctest 1706/1706 in 2 min 45 s, 0 errors — the five-impl-unit split and POSIX/socket GMF distribution compile on Darwin. Batch 1 (cc.services.mcp.client) is fully done; the C3 ratchet (tools/arch/inline_def_check.py, commit 59364ec) is live in arch-check CI with cc.services.mcp.client the first c2-done graduate (84 -> 12 semantic bodies). |
| 2026-09-25 | **Phase C batch 2/10 — `cc.tools.runtime_registry` bodies extracted.** Discovery-driven split of runtime_registry.cppm 2725 -> 424 LOC (declaration-only) into EIGHT `module cc.tools.runtime_registry;` impl units in a new cc_tools PRIVATE source block: json helpers, executors, native-agents, computer-use, skills, register (key function RuntimeFunctionTool::check_permission), the mega-dispatcher, and a separate team_dispatch TU (the dispatcher's team_create/team_delete branches were extracted into new detail free functions first, so no single TU exceeds ~360 LOC). Identity-sensitive entities kept in the cppm: 40 `constexpr auto` function-pointer aliases, the two inline team wrappers taking `&native_agent_status_is_terminal`/`&cleanup_native_agent_transcript_artifacts` (declared in cppm, defined exactly once in the native-agents TU — nm-verified single strong symbol), the inline computer-use override variables, and the dispatcher's function-local `static shared` map (not promoted — SIOF). Default arguments moved to declarations only; the `#ifdef _WIN32` powershell block moved verbatim. One real LLVM #184957 PCM-reachability keep-import remains (`cc.tools.synthetic_output_tool` — removing it SIGSEGVs clang 22, empirically confirmed; a draft `cc.utils.uuid_utils` marker of the same kind was disproven and dropped). Dead baseline edges grep/shared_tool/http legitimately shed. **C3 ratchet: runtime_registry 82 -> 5 semantic bodies (flagged c1 + c2-done — second C1 graduate).** Full debug+release `-Werror` builds clean; serial `ctest -j1` 1706/1706 on both presets (an earlier interleaved ctest showed spurious failures from a concurrent ninja rebuild; a clean serial rerun was 1706/1706); graph_check + inline_def_check green; targeted Tools/Tasks/Notif/Hook 235/235. Independent adversarial review **APPROVED** (999-string multiset identical, 44/44 dispatcher branches in order, nm single-definition audit clean). macos-14 is the cross-platform gate. |
| 2026-09-25 | **Phase C batch 2 confirmed on macos-14 (commit 85f0832, run 36077042815).** Full Build 8 min 40 s + serial ctest 1706/1706 in 2 min 50 s — the eight-impl-unit split, the `#ifdef _WIN32` dispatch branch, and the LLVM #184957 `synthetic_output_tool` keep-import all hold under the Apple SDK + cmake-4 synth BMI path. runtime_registry is the second C1/C2 graduate (82 -> 5). |
| 2026-09-25 | **Phase C batch 3/10 — `cc.tools.agent_runtime` bodies extracted (largest non-UI god interface).** agent_runtime.cppm 3950 -> 786 LOC, declaration-only, split into six `module cc.tools.agent_runtime;` impl units (text, yaml, json, builtin/plugin-discovery, sidechain/transcript-persistence, store+lifecycle). Singleton identity preserved: `native_agent_store()` declared once in the cppm and defined exactly once in the store unit (nm: one strong symbol, no weak duplicate; 351 external call sites); the private member template `NativeAgentStore::update<Fn>` stays in the class body and instantiates the 21 now-out-of-line store methods (15 weak COMDAT, zero strong dup). run_agent/fork_subagent/resume_agent/get_agent_lifecycle each one decl + one def. Default arguments moved declarations-only; the 25 unexported builtin_detail prompt constants moved verbatim (422-line block byte-identical); the three fork constants became TU-local in their single user unit. Dead `kForkPlaceholderResult` and unused `<cstdio>` removed. `cc.utils.json`/`cc.utils.yaml` correctly STAY in the primary (JsonVal/YamlValue appear in surviving declarations; their shedding is a separate `:parsing` partition follow-up); `cc.utils.team_helpers` remains only as an empirically-verified LLVM #184957 `operator new` reachability keep-import. **C3 ratchet: 160 -> 18 semantic bodies (c1 + c2-done — third C1 graduate).** Full debug+release `-Werror` builds clean; serial `ctest -j1` 1706/1706 both presets; targeted Agent/Tools/Tasks/Hook/Notif 265/265; graph + inline gates green. Independent adversarial review **APPROVED** (737-string multiset differs only by the deleted dead constant; char literals 60=60; nm singleton + 136 strong defs with zero duplicates). macos-14 is the cross-platform gate. |
| 2026-09-25 | **Phase C batch 3 confirmed on macos-14 (commit bfbc198).** Build 19 min 07 s (heavier fan-out — agent_runtime has 25 external importers) + serial ctest 1706/1706 in 3 min 47 s. The `team_helpers` LLVM #184957 keep-import workaround holds under the Apple SDK/cmake-4 synth BMI path; the singleton/template-boundary split links on Darwin. agent_runtime is the third C1/C2 graduate (160 -> 18). |
| 2026-09-25 | **Phase C batch 4/10 — `cc.tools.agent.utils` bodies extracted.** agent_sub_utils.cppm 3039 -> 803 LOC, declaration-only, split into seven `module cc.tools.agent.utils;` impl units (json, config, tools/mcp, hooks, teammates, messages, budget). Cross-archive guard hazard handled: the three RAII guards' destructors (AgentTodoCleanupGuard, AgentShellTaskCleanupGuard, AgentMcpCleanupGuard — constructed/destroyed by importers such as agent_tool) are declared in the cppm and defined exactly once, strong, in the hooks unit (nm: one T D1/D2 per guard, zero weak duplicates; importers/test bind U). Default arguments on 7 functions moved declarations-only; the WIF*/FILE hooks GMF (`<sys/wait.h>` + `<cstdio>`) is confined to the hooks unit. Shed 20 primary imports (12 genuinely dead + 8 relocated to the units that use them) and removed 9 dead exported aliases proven unreferenced across all six importers; team/swarm_backends/skill correctly stay (surviving cppm declarations name Team/MemberRole/AgentColor/SkillDefinition); one real `cc.utils.tool_helpers` keep-import in the budget unit (PERSISTED_OUTPUT_TAG, shallow-namespace checker blind spot). Dead baseline edges 150 -> 148. **C3 ratchet: 162 -> 6 semantic bodies (c1 + c2-done — fourth C1 graduate).** Full debug+release `-Werror` builds clean; serial `ctest -j1` 1706/1706 both presets; targeted 265/265; graph + inline gates green. Independent adversarial review **APPROVED** (515-string multiset identical; nm guard cross-archive audit clean; importer sweep proves the 9 alias removals safe). macos-14 is the cross-platform gate. |
| 2026-09-25 | **Phase C batch 4 confirmed on macos-14 (commits 8c78283 + b1f8624, run on b1f8624).** The first mac run's Build passed but Test showed 1/1706: `tools_smoke` returned timed_out=true with exit_code=0. Tracing found a genuine pre-existing cross-thread double-reap race in `impl_bash.cppm`'s timeout watchdog — its `waitpid(WNOHANG)` 50 ms after SIGTERM could reap the child and discard its status, leaving the main thread's waitpid ECHILD on a zeroed wstatus (exit 0). Fixed by escalating liveness with `kill(pid, 0)` so only the main thread reaps (independent concurrency review APPROVED; not a Phase C defect — impl_bash is untouched by the splits). Re-run: Build 8 min 17 s + serial ctest 1706/1706 in 2 min 46 s. agent.utils is the fourth C1/C2 graduate (162 -> 6). |
| 2026-09-25 | **Phase C batch 5/10 — `cc.query.query_engine` bodies extracted (engine core; 19 importers).** query_engine.cppm 3363 -> 824 LOC, declaration-only, GMF now EMPTY, split into ten `module cc.query.query_engine;` impl units in a new cc_query PRIVATE source list (ctor, system_prompt, loop, http, wire, json, tools, conversation, compaction, util). **Headline import-closure win: textual `<httplib.h>` evicted from the interface GMF into only query_engine_http.cpp — producer query_engine BMI on disk 50.0 MB -> 3.2 MB (~15.6x).** This required deleting the private member `add_beta_headers(httplib::Headers&)` and demoting it to a TU-local anonymous-namespace free function in the http unit (its two call sites compute the same optional::has_value() conditions; nm: one local `t` symbol, not exported); raw httplib types are not exported by cc.utils.http so the textual include stays in the one TU that names them. cppm cc.* imports 26 -> 10 (wire/analytics/memdir/session/compaction/agent_runtime/etc. moved to the units that use them; api_microcompact and json correctly stay for surviving return/parameter types). ~24 out-of-line static members defined without the `static` storage keyword, each once; generate_id/generate_session_id/add_jitter defined once in the util TU with function-local thread_local RNGs (nm-verified); the maybe_run_memory_extraction nested-QueryEngine on a member jthread (captures, single-flight Guard, kill switch) and function-local BlockAccum moved verbatim; the distinct cc::core vs cc::query::wire api_messages_endpoint entities preserved (no ODR clash). Default arguments on 7 members moved declarations-only. Two genuine keep-imports (tool_helpers constexpr tags; global ::memdir:: calls). Dead edge removed. **C3 ratchet: 109 -> 23 semantic bodies (c1 + c2-done — fifth C1 graduate).** Full debug+release `-Werror` builds clean (0 warnings); serial `ctest -j1` 1706/1706 both presets; targeted Query/Wire/Compact/Tools/FixQueryEngine 253/253; graph + inline gates green. Independent adversarial review **APPROVED** (345-literal multiset identical incl. beta-header strings; nm 86 strong impl defs + 23 accessor defs with zero duplicates; add_beta_headers condition audit clean). macos-14 is the cross-platform gate. |
| 2026-09-25 | **Phase C batch 5 confirmed on macos-14 (commit ccf6354).** Build 13 min 11 s (cc_query's first PRIVATE source block under the Apple linker) + serial ctest 1706/1706 in 3 min 03 s. The empty-GMF split and httplib demotion hold on Darwin. query_engine is the fifth C1/C2 graduate (109 -> 23; producer BMI 50 MB -> 3.2 MB). |
| 2026-09-25 | **Phase C batch 6/10 — `cc.ui.visual.markdown` split; the LLVM #184957 UI canary.** markdown.cppm 1788 -> 528 LOC, declaration-only, split into seven `module cc.ui.visual.markdown;` impl units in the cc_ui PRIVATE block (lexer, cache, linkify, render_code, render, api, component). **Canary result: all five FTXUI-GMF-mixing units compile cleanly WITH `import std;` on the first try — zero textual-std fallbacks** (only the documented 3 app_* units still need that opt-out), lowering the risk for the remaining UI batches. Import-closure win: `import cc.ui.visual.code_highlight` left the cppm (render_code_block was its only user), so the code_highlight + cc.types.types closure leaves markdown's ~9 importers (dyndep: markdown.cppm now requires only std); the unused cc.utils.hyperlink edge was removed. global_token_cache defined once (strong T) in the cache unit with its function-local static; MarkdownComponentBase ctor/Render/OnEvent anchored once in the component unit (vtable/typeinfo owned by the interface unit under clang modules — one strong copy); defaults (opts={}, max_size=256) declarations-only; render glyph tables/attributes moved verbatim. **C3 ratchet: 43 -> 7 semantic bodies (c2-done — first UI graduate; not a top-6 C1).** Full debug+release `-Werror` clean; serial `ctest -j1` 1706/1706 both presets including all truecolor/Markdown golden snapshots (130 UI/Markdown/golden cases byte-identical); graph + inline gates green. Independent adversarial UI review **APPROVED** (173-literal multiset identical incl. box/color glyph escapes; render bodies byte-for-byte; nm singleton/vtable/dyndep clean). macos-14 (FTXUI + reduced BMI under Apple SDK) is the decisive gate. |
| 2026-09-25 | **Phase C batch 6 confirmed on macos-14 (commit cf5893d) — UI/#184957 canary PASSED.** Build 13 min 04 s + serial ctest 1706/1706 in 3 min 50 s with all truecolor/Markdown golden snapshots byte-identical under the Apple SDK + cmake-4 synth BMI path. New FTXUI-GMF + `import std` impl units compile clean (zero textual-std fallbacks), so the remaining UI batches (messages_list/text_input/repl_screen) can use import std by default. markdown is the first UI c2-done graduate (43 -> 7). |
| 2026-09-25 | **Phase C batch 7/10 — `cc.ui.messages.messages_list` split (C1 UI god interface).** messages_list.cppm 3720 -> 891 LOC, declaration-only, into seven `module cc.ui.messages.messages_list;` impl units (filter, search, geometry, envelope, payload_row, view, component). **Import closure:** the ~20-module variant-owner fan-in lands once in messages_list_search.cpp (exhaustive std::visit with static_assert), and the 379-LOC render_payload_row takes the faithful-renderer closure in its own TU; the primary keeps only message_row/virtual_list/visual.markdown (the latter as a compile-verified keep-import for a global-qualified StreamingMarkdown* member). **std-mode split, empirically reproduced (LLVM #184957):** the two empty-GMF units (filter, geometry) hit `operator new ambiguous` with import std and correctly use the textual libc++ opt-out; the four FTXUI-GMF units + the empty-GMF search TU compile with import std (5/7). Test-visible detail symbols (find_divider_before_visible_index, render_unseen_divider) stay exported; MessagesListComponent ctor/Render/OnEvent anchored once in the component TU with the vtable/typeinfo single in the interface unit; 22 trivial accessors (20 palette + 2 input) and 5 inline vars stay; default args declarations-only. tests/test_ui_e2e.cpp's connector-glyph source path updated to payload_row (U+23BF now lives there). **C3 ratchet: 72 -> 22 semantic bodies (c1 + c2-done).** Full debug+release `-Werror` clean; serial `ctest -j1` 1706/1706 both presets, truecolor goldens byte-identical (165-literal/37-char/19-ColorRGB multisets match; 22 visit arms identical); graph + inline gates green. Independent adversarial review **APPROVED**. macos-14 is the gate (textual-std opt-outs must hold under the Apple SDK too). |
| 2026-09-25 | **Phase C batch 7 confirmed on macos-14 (commit e78ad2c) — all six C1 god-interfaces now extracted.** Build 11 min 52 s + serial ctest 1706/1706 in 3 min 03 s, truecolor goldens byte-identical. The two empty-GMF textual-std opt-out units (filter/geometry, LLVM #184957 aligned-operator-new workaround) compile identically under the Apple SDK. messages_list is the sixth C1/C2 graduate (72 -> 22). Remaining Phase C work: the C2-100 bodies text_input and repl_screen (largest UI); app.app deferred (zero BMI win). |
| 2026-09-25 | **Phase C batch 8/10 — `cc.ui.widgets.text_input` split (C2 UI god interface).** text_input.cppm 2462 -> 624 LOC, declaration-only, into four `module cc.ui.widgets.text_input;` impl units in the cc_ui PRIVATE block: buffer (ctor + ~40 edit/history/suggestion/paste members), events (HandleEvent with its `after_change` goto tail + reverse-history search), vim (HandleVimEvent/`after_vim` + 9 vim operators), render (Render/RenderInputArea + the free TextInput() factory). **All four units compile with `import std` — zero textual-std fallbacks** (the batch-6 canary + batch-7 hold for both FTXUI-GMF and empty-GMF editor units). Three heavy imports shed from the primary (`cc.utils.parse_references`, `cc.ui.foundation.design_figures`, `cc.ui.prompt.placeholder_cascade`), each re-imported only by the unit that uses it; the private `TruncatedPasteResult` alias + `maybe_truncate_paste` wrapper demote to a TU-local `truncate_paste_result` free function in buffer (grep-verified zero external callers). Both goto shared tails (update_suggestions + on_change; vim adds recompute_derived) are whole-function preserved with labels after unconditional returns. Default args declarations-only; the former static helpers drop `static` out-of-line; dead GMF includes removed from the primary (cctype, screen_interactive, screen/string — bare `size_t` keeps cstddef) and an unused cctype from events. **Measured producer BMI on local-linux (same `-c`/modmap path, scratch pre/post): 25,529,952 -> 18,827,232 B (−26.3%, 1.36×); primary object 2.39 MB -> 39 KB**; bodies redistribute buffer 1.12 MB / render 1.97 MB / events 111 KB / vim 64 KB. nm: every moved member one strong T in exactly one TU, C1/C2 ctor aliases share one address, zero cross-unit dups. **C3 ratchet: 96 -> 28 semantic bodies (c2-done).** Full debug+release `-Werror` clean (0 warnings); serial `ctest -j1` 1706/1706 both presets; no golden file touched; graph + inline gates green. Independent adversarial review **APPROVED** (62 members + ctor init-list + 3 detail free functions + factory token-identical; goto convergence, ODR, Render tree/public surface verified; three cosmetic nits, two applied). macos-14 is the gate. |
| 2026-09-25 | **Phase C batch 8 confirmed on macos-14 (commit 30aeef8, run 36131266023).** Build 13 min 37 s + serial ctest 1706/1706 in 3 min 14 s; all golden/visual-snapshot suites pass byte-identical (E2E_Gate.FullConversationGoldenSnapshot, DialogRenderers.Golden_*, VisualSnapshot.ToolPermission*, ImagePaste.ClipboardCard_GoldenSnapshot) under the Apple SDK + cmake-4 synth BMI path; the four import-std editor units and the three shed-import closures compile and link on Darwin. text_input is the seventh C2 graduate (96 -> 28; producer BMI −26.3%). Remaining Phase C work: repl_screen (largest UI, 4101 LOC, frozen at 82) then the phase is done apart from the deferred app.app (zero producer BMI win). |
| 2026-09-25 | **Phase C batch 9/10 — `cc.ui.screens.repl_screen` split; the sixth and final C1 god interface (largest UI).** repl_screen.cppm 4101 -> 523 LOC, declaration-only, into ten `module cc.ui.screens.repl_screen;` impl units in the cc_ui PRIVATE block: messages (RenderMessages + the 12-module message-renderer closure), scroll, prompt_buffer (both empty-GMF TEXTUAL-std, LLVM #184957, self-contained algorithm/utility headers), welcome, prompt_render, dialog_queue, layout (RenderReplScreen + both [&s] lambdas, function-local spinner_frame), agents (out-of-line AgentMenuListBase key-function anchor), dialog_panels (permission classifiers demoted to anon namespace), events (both ReplScreen factories verbatim). The eight FTXUI-GMF units compile with import std — zero flips vs the canary prediction. Primary cc imports collapse to types + messages_list + visual.markdown (global-qualified StreamingMarkdown*) + agent_cards plus the export-import of repl_state; nine verified-dead imports + one duplicate removed (graph dead-import baseline −3). String shapes preserved (`\x1f` cache key, 800 ms/1000 ms escape-again, `@history `, Esc strings, y/n/a order); five function-local statics stay function-local; per-render TextInputImpl preserved; pre-existing RGB untouched; dead welcome cluster moved not deleted. nm: member bodies one strong T each in exactly one unit, vtable/typeinfo in the interface unit, C1/C2 ctor aliases one address, classifiers local t. **Measured producer BMI on local-linux (same -c/modmap, scratch pre/post): 50,505,412 -> 29,125,816 B (−42.3%, 1.73×); primary object 877 KB -> 35 KB. C3 ratchet: semantic inline bodies 82 -> 0.** Full debug+release `-Werror` clean (0 warnings); serial ctest -j1 1706/1706 both presets (one release run showed a single non-reproducing timing failure; two further full serial runs 1706/1706); no golden touched; graph + inline gates green. Independent adversarial review **APPROVED** (machine-verified body + string/char-literal multiset identity, ODR, public surface, slot order, declared-cursor math); three hygiene findings applied (textual-std self-containment, duplicate struct removed, dead aliases + orphan import dropped). macos-14 is the gate. |
| 2026-09-25 | **Phase C batch 9 confirmed on macos-14 (commit 392268f, run 36144159600) — all six C1 god interfaces now extracted and <30.** Build 13 min 17 s + serial ctest 1706/1706 in 3 min 11 s; all 27 golden/snapshot suites pass byte-identical; the 8-import-std/2-textual-std unit mix and the AgentMenuListBase key-function link hold under the Apple SDK + cmake-4 synth BMI path. repl_screen inline bodies 82 -> 0; producer BMI −42.3%. A follow-up corrects its ratchet flag to `c1 c2-done` (OQ-4 lists repl_screen 321 among the C1 six — agent_runtime 498, agent.utils 439, query_engine 433, repl_screen 321, messages_list 321, runtime_registry 316; text_input 282 is C2). Remaining Phase C: batch 10, the four frozen C2 interfaces still >100 — cc.utils.json 131, cc.state.selectors 118, cc.utils.swarm_backends 116, cc.hooks.remaining_hooks 108 — after which C2's "no interface >100" exit is met and C3's ratchet already enforces it. |
| 2026-09-25 | **Phase C batch 10/10 starts — `cc.hooks.remaining_hooks` DELETED as zero-importer dead code (not split).** A read-only split planner for the four frozen C2 interfaces discovered that remaining_hooks (591 LOC, 27 exported TS-parity hook classes, 108 frozen inline bodies) has **zero `import cc.hooks.remaining_hooks;` anywhere — and a full-history `git log -S` shows it was born unreferenced in the bulk-port commit and never wired.** Independent adversarial verification: all 27 distinctive class names have zero qualified references across src/tests/benchmarks/scripts/tooling (same-named hits are unrelated types); no string-based hook registry/factory, X-macro, textual include, or CMake glob reaches it; nm shows zero of the object's 1691 global symbols undefined-referenced by any other object or the loom binary (it was an unreferenced libcc_hooks.a archive member); ninja dyndep shows zero dependent PCMs. Live sibling modules cover the same concepts (repl_bridge, ide_integration, ide_at_mentioned, away_summary, swarm_hooks). Per CLAUDE.md "prefer deleting dead code to fixing it", removed the file + its FILE_SET row + its frozen ratchet row (commit da4e6eb; 593 deletions). This satisfies the C2 cap for this interface by removal. Debug + release `-Werror` clean; serial ctest -j1 1706/1706 both presets; graph_check + inline_def_check green; independent adversarial review **VERDICT: SAFE TO DELETE**. |
| 2026-09-25 | **remaining_hooks deletion confirmed on macos-14 (commit da4e6eb, run 36151170868).** Build 11 min 47 s (2322 compile steps, −2 vs batch 9's 2324 — the module interface + object gone) + serial ctest 1706/1706 in 3 min 03 s; no behavioural change on either platform. (Process note: the first push a176612 accidentally omitted the two wiring edits when an `git add` pathspec aborted staging; caught before CI configured, amended to da4e6eb with all three files and force-pushed with lease, stale run canceled.) Remaining batch-10 C2 splits: cc.state.selectors 118, cc.utils.swarm_backends 116, cc.utils.json 131. |
| 2026-09-26 | **Phase C batch 10 — `cc.state.selectors` split confirmed on macos-14 (commit 18f634e, run 36155036356).** selectors.cppm 830 -> 549 LOC into six `module cc.state.selectors;` impl units in the first cc_state PRIVATE block (core/bridge/ui_tasks/companion_mcp/conversation/features); 113 pure `[[nodiscard]] noexcept` accessors move out (35/12/19/16/11/20 strong T, one each, zero dups), the MemoizedSelector class template + five dead factory templates stay (118 -> 5, c2-done); only conversation imports cc.types.types (TokenUsage const-ref). All six plain import std (no FTXUI in this closure). Measured producer BMI on local-linux 399,020 -> 136,656 B (−65.8%, 2.92×). First macos attempt had 1/1706: `McpStdio.WatchdogKillsSilentChild` — a 200 ms-kill/5 s-budget child-watchdog scheduling test in cc.services.mcp (transport_stdio.cppm, unrelated to the cc.state refactor; no causal module path); `gh run rerun --failed` passed that test in 0.37 s with 1706/1706, classifying it as a loaded-runner timing flake (same family as the batch-4/5 infra flakes), not a regression. Independent adversarial review APPROVED (113/113 bodies + templates byte-identical, noexcept/signature parity). Remaining batch-10: cc.utils.json (opaque-yyjson split done locally) then cc.utils.swarm_backends. |
| 2026-09-26 | **Phase C batch 10 — `cc.utils.json` split confirmed on macos-14 (commit 09f0db1, run 36161544494).** json.cppm 549 -> 397 LOC into seven `module cc.utils.json;` impl units in the existing cc_utils PRIVATE block; the two transforms hold under the Apple SDK + cmake-4 synth BMI path: (1) the **opaque yyjson boundary** — primary GMF forward-declares the four yyjson struct tags instead of textually including <yyjson.h> (all primary uses pointer-only; external raw-pointer consumers compile through the same global-namespace entity), so the textual C header no longer reaches the ~160 importers; (2) the four **type-erased iterator trampolines** (non-capturing fn-pointer + void* ctx, walk sequence/arities identical, all ~191 call sites incl. capturing/move-only callables unchanged). 131 -> 22 c2-done; measured producer BMI 1,321,284 -> 301,636 B (−77.2%, 4.38×) before fan-out. Build 16 min 09 s (2346 steps) + serial ctest 1706/1706; fuzz_json_parse and the migration/parser suites green. Independent adversarial review APPROVED (opaque type-identity, trampoline lifetime/calling-convention, literal multiset). Remaining: cc.utils.swarm_backends (final C2 target). |
| 2026-09-26 | **Phase C batch 10 — `cc.utils.swarm_backends` split confirmed on macos-14 (commit b22fcea, run 36169615399).** swarm_backends.cppm 1843 -> 967 LOC, empty-GMF declaration-only primary + eight impl units (shell/detect/tmux/iterm/inprocess/executor/registry/detail). Under the Apple SDK: shell seam singletons one strong T+guard in shell.o only; three guard/base dtors strong in executor.o (six class vtables/typeinfo strong in the interface unit, every other TU binds U); 17 static-inline data members one strong B each (11 detect + 6 registry); zero #184957 operator-new ambiguity after shedding team_helpers/bash_execution and emptying the GMF. **The stale no-op per-file `-O0` pin (it referenced a non-existent pre-move path) was removed; tmux (307 LOC) and detail (207) compile at real Apple -O2 with no optimizer crash** — validating the split-the-TU remedy. 116 -> 22 c2-done; producer BMI 3,707,012 -> 1,290,804 B (−65.2%, 2.87×). Build 13 min 15 s (2362 steps) + serial ctest 1706/1706 (33 swarm/teams/pane/tools/tasks cases green). Independent adversarial review APPROVED (89/89 bodies identical, singleton unified, lock-order/!inside polarity, literal multiset differs only by 3 deleted dead-code strings). |
| 2026-09-26 | **Phase D — re-home `cc.utils` into domain directories, confirmed on macos-14 (commit 88b4aae + comment fix 3c92be7, run 36174887208).** Per the OQ-4 mapping: **115 files `git mv`'d with zero content changes** (every rename 0 insertions/deletions), only path lines in cc_utils.cmake/cc_services.cmake edited. Module NAMES stay `cc.utils.*` (name/path decoupled), so zero importers/imports/BMI edges changed. The 103 formerly-flat interfaces plus the batch-10 impl `.cpp` units now live under ~30 domain dirs (fs, serdes, platform, security, process, shell, model, http, git, plugin, teams, swarm, text, session, …), joining the 15 pre-existing; no flat files and no empty dirs remain. Specials: prompt_category RETAINED (real assertions in test_utils.cpp — OQ-4's deletion candidate disproved in ctest) -> prompt/; ide_integration -> src/services on cc_services per OQ-3 (name unchanged; its importers use the module name); statusline_runner -> statusline/, theme -> theme/, system_theme -> platform/, image_store/pdf -> media/. Verified: 171-module set unchanged and each declared once; CMake on-disk set equals listed set both ways with FILE_SET/PRIVATE membership preserved; repo-wide no stale filesystem path and no moved file uses `__FILE__`; debug+release `-Werror` clean, serial ctest 1706/1706 both, graph/inline green; independent review APPROVED (byte-identical rename audit, OQ-4 grouping 0 mismatches). mac build 15 min 36 s (2362 steps) + 1706/1706, 0 errors — scan-deps resolves every re-pathed module under cmake-4/Apple. |
| 2026-09-26 | **Phase D lint enforced.** inline_def_check.py now fails on any `cc.utils.*` interface placed flat directly in src/utils/ (must be under src/utils/<area>/, name unchanged); per-module frozen exceptions in the new (empty) flat_utils_exceptions.txt; negative-tested. This completes Phase D's graduation criteria as scoped by the implementable-gate decision: files re-homed and **zero new flat modules, lint-enforced**. Cross-library/target and module-NAME migration (renaming cc.utils.* to cc.fs.* etc., moving statusline/theme to cc_ui) is a separate rename-heavy change and remains out of scope. |
| 2026-09-26 | **PHASE C COMPLETE (pending status flip) — all three sub-criteria measured and green.** C1: the six named god interfaces are extracted and each < 30 inline bodies — query_engine 23, messages_list 22, agent_runtime 18, agent.utils 6, runtime_registry 5, repl_screen 0 (all flagged `c1 c2-done`). C2: `inline_def_check.py` now reports **849 interfaces analyzed, 0 over 100** (the four frozen >100 C2 modules resolved in batch 10: remaining_hooks 108 DELETED as proven zero-importer dead code, selectors 118→5, json 131→22, swarm_backends 116→22). C3: the fail-on-increase semantic ratchet + fail-closed new-interface gate run in the Architecture check workflow and were green on every batch. Across batches 1–10 the recipe proved: module impl units in plain PRIVATE target_sources (never FILE_SET); single strong definition for key functions/singletons/guard dtors/static data; default args declarations-only; third-party textual headers (httplib/yyjson) and heavy import closures confined to one TU; LLVM #184957 handled empirically (FTXUI-GMF units import std, selected empty-GMF units use textual-std, opaque-tag boundary for yyjson). Every batch independently agent-reviewed (all APPROVED), dual local-linux `-Werror` + serial ctest 1706/1706, and macos-14 green with goldens byte-identical. `cc.ui.app.app` remains explicitly DEFERRED (zero producer-BMI win; edit-isolation only). Next: Phase D (re-home cc.utils per the OQ-4 mapping). |
| 2026-09-26 | **Phase E — reproducible producer-BMI/PSS sampler landed (commit 35bbf48), confirmed on macos-14 (run on 56f016c).** New stdlib-only `tools/arch/measure_bmi.py` compiles one interface TU via its exact compile_commands argv + @modmap and records producer peak PSS from `/proc/<pid>/smaps_rollup` (the RFC metric, never RSS), emitted reduced-BMI `.pcm` bytes, object bytes and wall time; human/`--json` output, direct + ninja modes (ninja pipe drained on a reader thread, no `-j`), source by path/suffix/basename, exit 3 off-Linux (mac evidence stays CI wall timing), NOT in the static arch-check workflow. Validated: bmi_bytes exactly reproduces recorded anchors (query_engine 3,171,976; mcp/client 879,120); both modes leave the tree deterministic. Independent adversarial review REQUEST-CHANGES → both MAJors (ninja pipe deadlock on large rebuild; broken documented source shorthands) and all MINORs fixed/re-verified. The OQ-5 compiler-cache sub-goal is being decided by a separate `cache-pilot` branch / draft PR (sccache cold/warm/invalidation) and is not yet wired on master. |
| 2026-09-26 | **Phase B starts (gated on the `--target-core8` graph prediction).** Batch 1 (commit 56f016c, REV3 family F1): deleted four dead `cc.state.app_state` imports in hooks/{assistant_history,background_task_navigation,prompt_suggestion,turn_diffs} (token-level audit: none of app_state's 37 exported names used; zero importers of the four modules) and the matching edges from both baselines — cc.state leaves the area SCC. Batch 3 (commit 906e888): renamed the already-services-owned/built `cc.utils.ide_integration` to `cc.services.ide_integration` (3 importers; C++ namespace cc::utils::ide unchanged, name-lookup identical), re-keyed its frozen inline row (44) and dropped the two now-same-area upward edges — cc.utils leaves the SCC. macos-14: both green (b1 run 14m35s; b3 run 13m11s), serial ctest 1706/1706 each, independent reviews APPROVED. After B1+B3 the remaining current-directory SCC is `{cc.hooks, cc.services, cc.skills, cc.tools}` (target: 9 singletons); `--target-core8` still FAILS as expected until the remaining families (tool DTO sinks, service-backed tools, the B11 orchestration lift) land. Phase B is sized at ~11 batches + one atomic composition-root commit and continues incrementally; cc.utils.json/selectors/swarm_batch10 work is done. |
| 2026-09-26 | **Phase B batch 4 — dead skills→tools edge cut (commit 2febc0e, macos run 36221204666).** `src/skills/bundled/debug.cppm` carried `import cc.tools.tool;` but referenced none of its names (only two doc comments mention ToolResult/ToolRegistry; the debug skill dispatches through the skill loader, never the tool registry) — the single skills→tools area edge. Token-level audit, then deleted with its frozen upward-edge row: **cc.skills leaves the area SCC**, which contracts to `{cc.hooks, cc.services, cc.tools}`. No code path changed (pure dead-import removal). Local debug+release `-Werror` clean, serial ctest 1706/1706 both; macos-14 Build 11m16s + Test 3m03s, **1706/1706**; independent adversarial review APPROVED. |
| 2026-09-26 | **Phase B — OQ-3 family 2 AMENDED and retired: delete dead `cc.hooks.voice_hooks` (commit a253b81, macos CI run 36222915815).** REV3 prescribed building a cc.hooks-owned voice port with services.voice behind it, but the assumed consumer does not exist: voice_hooks (1245 LOC) had zero importers, was never imported in full history (born unreferenced in the bulk-port commit), zero of its ~1440 symbols are referenced by any other object or `bin/loom` (nm over all libs/bins/tests), no registry/string/textual reach path, and the `docs/2026-09-16-retained-feature-gap-audit.md` audit marks voice as owner-NOT-wanted. `src/services/voice/*` is independently live and unit-tested (injects its transcriber, needs nothing from voice_hooks), so deleting the dead consumer leaves voice behaviour intact. Building a port for zero consumers would manufacture maintenance — family 2 is amended from "build port" to DELETE (CLAUDE.md: prefer deleting dead code); live upward edges **52 → 49**, the 9-singleton end state is unchanged. Removed: FILE_SET row, the 75 frozen inline rows, 3 voice upward-edge rows. The SCC stays `{hooks,services,tools}` (families 3/4/8/12 + B11 still required). Local debug+release `-Werror`, serial ctest 1706/1706, graph/inline gates green; independent adversarial dead-code review VERDICT: SAFE TO DELETE; macos-14 Build 7m13s + Test 2m44s, **1706/1706**. Newly orphaned `cc.context.voice` / `voice_stream_stt` / `voice_keyterms`, the `/voice` stub and an unused VoiceMode flag are recorded separate non-blocking follow-ups. |
| 2026-09-26 | **Phase E COMPLETE — sccache named-module cache proven and wired (OQ-5 option (a) succeeds; G6 MET; commit 057ad49).** Proof ran on the throwaway `cache-pilot` branch (PR #2, closed; two temp marker commits NOT merged) with Homebrew **sccache 0.17.0** — the version that understands cmake-4 quoted response/modmap files. macos-14 measurements (all runs 1706/1706): **cold** 1185 compile requests, 0 hits, **0 non-cacheable** (run 36220055700 first execution, Build 8m16s) — every `.cppm` producer cached, reversing the 2026-09-24 0.4.0-pre.6 "unknown source language" result; **warm** rerun of the same SHA: **1185/1185 = 100% hits, Build 1m33s**; **impl-unit edit** (987b2f7, run 36221411055): 4 misses / 1181 hits (edited TU + downstream only), Build 1m36s; **leaf-interface edit** (97a584b, run 36221834986): 338 misses / 847 hits = producer + full importer closure, Build 6m48s — exact dependency invalidation with **zero stale hits** (tests then re-verify behaviour, and they pass). Production wiring in 057ad49: `CMAKE_C/CXX_COMPILER_LAUNCHER=sccache` + actions/cache (5G, keyed on cmake/toolchain inputs) on the macos-14 job only, CI-time minor-version gate **≥0.17**, always-on `sccache --show-stats`, no `-j`/parallelism change (transparent to scan/compile/link pools). Master cold-population run 36223064718 reproduced it on the real tree: **1184/1184 cached, 0 non-cacheable, Build 12m01s, Test 3m02s, 1706/1706** (1184 vs pilot 1185 = voice_hooks deleted in a253b81); the closed-PR cache is correctly not restorable from master, so this run was intentionally cold and the next src-touching master push starts warm. The distro **0.4.0-pre.6 stays forbidden** — beyond non-cacheability it served stale objects over changed BMIs in the 2026-09-24 spike; the offline Linux box never sets the launcher. Independent adversarial review of the workflow diff APPROVED before merge. |
| 2026-09-26 | **Phase E warm-cache reproduced on master production wiring (commit 4de0731, run 36225150657).** The first src/CMake-input-unchanged push after the cold-population run restored the sccache cache **by exact key** and built with **1184/1184 = 100% hits, 0 misses, 0 non-cacheable — Build 1m36s** (vs 12m01s cold), Test 2m52s, **1706/1706** — the G6 single-digit-minute goal now holds on the real master branch, not only the throwaway pilot. The commit itself is a comment-only ci.yml fix (an adversarial evidence audit caught that the comment claimed Homebrew ships 0.18 while all runs installed 0.17.0; the ≥0.17 gate was already correct). |
| 2026-09-26 | **Phase B follow-up closure — complete voice feature removal (RFC-0001 series B, 8 commits on master; confirmed locally; macos gate pending).** The 2026-09-26 a253b81 row recorded the newly orphaned `cc.context.voice` / `voice_stream_stt` / `voice_keyterms`, the `/voice` stub and the unused VoiceMode flag as separate non-blocking follow-ups; the retained-feature gap audit marks voice as owner-NOT-wanted. Serial multi-commit deletion: (0) dead `agent_cards -> cc.utils.swarm_backends` import + frozen baseline row; (1) `src/context/voice.cppm`, `src/services/voice/{voice,voice_keyterms,voice_stream_stt}.cppm` (+ empty dir, CMake rows) and 2 VoiceService tests; (2) `src/commands/voice.cppm` + group-D registration; (3) `VoiceMode = 1 << 2` in config.cppm (bit left dormant, siblings unrenumbered) and `Feature::VoiceMode` + `VOICE_MODE` registry row in feature_flags.cppm; (4) `RenderVoiceModeNotice` + its notice-stack push in logo_v2.cppm and the two regenerated logov2_render_modes_missing goldens (one voice row replaced by an equal-width space-fill row per file, byte-verified); (5) `src/ui/prompt/voice_indicator.cppm`, the repl_state voice fields and `ProjectVoiceFooterStatus`, the app render-loop voice pulse branch (and its only justified `reduced_motion_enabled` + the theme_provider import it alone required), the repl_screen_layout projection, and the prompt_input_footer NotificationData voice fields/early-return/has_active term, plus 2 footer tests; (6) the remaining voice-only primitives — text_input_widget `VoiceState`/`AudioWaveformData`/`RenderWaveformCursor`, partial_completions `GlimmerStyle::VoiceTranscribing`, `highlight_priority::VoiceInterim`, combined_highlights `voice_interim_range`, and the `hide_placeholder_text`/`hide_text` placeholder path across text_input/text_input_render/placeholder_cascade, plus 3 HideText tests; (7) the living hazard catalog entries in design-decisions.md amended in place with REMOVED 2026-09-26 + one-line historical notes (this row). KEEP items verified untouched: ui_types `voice_start`/`voice_end` attachment markers, prompt_suggestion `loom_voice`/`matches_claude_voice` dictation heuristic, and the generic Audio attachment waveform thumbnail. Local evidence (local-linux, clang 22, `-Werror`): every commit debug-built clean and passed serial `ctest -j1`; total **1706 → 1699** (−2 service, −2 footer, −3 placeholder, 0 from the command/flag/logo/primitives commits); graph_check green throughout (dead-imports 139 backlog) and inline_def_check green with two genuine re-freezes (logo_v2 31→30, text_input_widget 33→32); release build + serial ctest green at series end. macos-14 confirmation is the only remaining gate. |
| 2026-09-26 | **Voice-removal series confirmed on macos-14 (tip 833df2c, CI run 36232048693 + Architecture check 36232048691).** Both gates success: Build 5m58s, serial Test 3m09s, **1699/1699** — matches the local 1706→1699 reconciliation exactly. The sccache wiring also passed its first *structural* change since being enabled: the four `src/cmake/targets/*.cmake` FILE_SET-row edits rotated the primary cache key (`7b1426f0… → 745867e6…`, proving `src/cmake/**` is in `hashFiles`); the restore-keys prefix restored the prior cache and sccache content hashing produced **1071 hits / 107 misses / 0 non-cacheable (90.92%)** — the 107 misses are exactly the deleted/edited TUs plus their importer closure, with the 2.32 GB prior cache reused and zero stale objects. Independent adversarial review of the full 8-commit range (range `910d312..833df2c`) returned APPROVED with zero BLOCKER/MAJOR before push; it independently re-ran both preset builds and serial ctest, verified the two goldens differ only by voice-row→equal-width-space-fill, and confirmed no non-voice token references remain. New serial-ctest baseline: **1699**. |
