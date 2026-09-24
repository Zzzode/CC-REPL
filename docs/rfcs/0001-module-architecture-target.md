---
rfc: 1
title: Module Architecture Target Shape
status: provisional
owners: "@Zzzode"
reviewers: []
created: 2026-09-23
last-reviewed: 2026-09-23
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
| Interfaces containing function bodies | **816 / 850** | declarations in interfaces, bodies in impl units |
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
must never import above. Ports (abstract interfaces) live in the lower layer
so a lower layer can accept behaviour injected from above without importing it.

```
cc.third_party.ftxui        ── single wrapper module; import std; everywhere
        │
types / constants / config ── zero-dependency leaves
        │
platform / fs / text / json / process / crypto ── sub-domain leaves (from cc.utils)
        │
state / task_types / vim
        │
tools (pure domain logic; depends only on declared ports)
        ▲            ▲  ports implemented above, injected at composition
        │            │
services (MCP / LSP / API / voice concrete implementations)
        │
hooks (event contracts and port interfaces)
        │
query / commands / skills / orchestration (agent run/resume/fork live HERE)
        │
ui
  foundation → chrome → widgets/visual → messages/dialogs/permissions/prompt
             → screens → app
        │
server / cli / entrypoints
```

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
5. **No upward edges** (§4.2). Enforced by a CI graph check, not convention.
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

Graduation: Tarjan over directory nodes reports singleton components for
the eight areas (Core8 → 8× size 1); `cc_utils`/`cc_tools`/`cc_services`
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
- G6. macos-14 cold and warm builds stay green at default parallelism with no
  swap; warm-cache PR CI reaches single-digit minutes.

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
- Incremental CI with warm cache in single-digit minutes.
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

## 9. Sequencing summary

| Phase | Scope | Risk | Expected build payoff |
|---|---|---|---|
| A | `import std;` (per-TU atomic) + FTXUI per OQ-1 | med (spike-gated) | high |
| B | break Core8 SCC via OQ-3 port design, split non-UI libs | med | medium + structural |
| C | bodies out of god interfaces | low (mechanical) | high, incremental |
| D | re-home `cc.utils` | low | low–medium |
| E | graph_check.py + PSS sampler + (spike-gated) sccache | low–med | very high for PR latency if spike passes |
| F | UI state sharding, break UI9, split cc_ui | high | structural |

A, C and D can start as soon as the RFC is implementable; A is gated
on the OQ-1/OQ-2 spike and B on OQ-3. E's lint part can land first; its
cache part is spike-gated (OQ-5). F awaits a dedicated RFC after measured
results from A-E.

## 10. Open questions

These gate the `accepted` / `implementable` transitions. None is answerable
without a branch spike or an explicit design decision; they are recorded
here so the gate cannot be passed on assertion.

| ID | Question | Owner | Blocks | Acceptance of the answer |
|---|---|---|---|---|
| OQ-1 | ~~How does FTXUI enter module units?~~ **Spike 2026-09-24 (branch rfc-0001-spike-import-std-ftxui): the clang mechanism WORKS** — FTXUI v5.0.0 headers build cleanly as user header units (`-fmodule-header=user`): color.hpp 8.3 MB / 1.3 s one-off, dom/elements.hpp 12 MB, component/component.hpp 19 MB / 2.0 s; FTXUI headers are module-compatible. Manual consumer wiring needs the scan-deps-generated module-map/`-fmodule-file` set, which is exactly what **CMake 4 `FILE_SET CXX_MODULE_HEADERS`** generates; CMake 3.31 (offline dev box) rejects the file-set type, so the end-to-end CMake path is validated on CI (brew cmake is 4.x). Remaining decision at implementable: full header set enumeration strategy (list FTXUI public headers wholesale vs. the subset Loom imports) and the leaf-subarea golden pilot. Status: mechanism proven, CMake-4 integration CI-gated. | @Zzzode | Phase A | CI pilot: one leaf UI subarea built through CXX_MODULE_HEADERS; truecolor goldens byte-identical or reviewed; consumer PSS/wall-time recorded vs textual. |
| OQ-2 | ~~Exact CMake recipe for `import std;`?~~ **Spike 2026-09-24: RESOLVED on the local toolchain.** (1) CMake 3.31 `CXX_MODULE_STD` exists but is gated behind an experimental UUID and builds std with `-std=gnu++23`, mismatching this repo `CXX_EXTENSIONS=OFF` (c++23) — rejected. (2) The robust path is vendoring the shipped `std.cppm` as an ordinary FILE_SET CXX_MODULES target, compiled with `-fno-implicit-module-maps -Wno-reserved-module-identifier` and an include dir at the toolchain `share/libc++/v1` (for `std/*.inc`); works at c++23 with ext OFF, CMake 3.28+, and carries whatever flags the preset already sets (so macos isysroot/libc++ flags flow naturally). Consumer micro-benchmark: a 10-header heavy TU **1.71 s -> 0.13 s (13x)**; std BMI precompiled once (35 MB). (3) The claimed "cannot mix textual std headers with import std" rule does NOT hold on clang 22 — a mixed TU compiled exit 0 — so conversion is incremental per file, not atomic per target (still prefer per-target commits). macos build of the same vendored std.cppm to be confirmed on CI. | @Zzzode | Phase A | vendored std module target builds under both presets; one leaf target converts and dual-preset + macos CI pass; 13x micro result reproduced inside a real producer TU before sweep. |
| OQ-3 | ~~What is the precise orchestration boundary and port shape?~~ **Design attached 2026-09-24:** [OQ-3 Phase B cut design](attachments/0001-oq3-phase-b-cut-design.md) — lift the 5-module agent facade subtree (run/resume/fork/utils + `cc.tools.agent`) to a new `cc.orchestration.agent` target (the 7 store/display/memory agent modules stay in tools); per-edge cuts for the other 7 edge families; 3 of the hooks->state edges proven to be dead imports. Code-level port modules and the graph-prediction gate remain to be produced when the phase starts. | @Zzzode | Phase B | The attached doc exists (done). Before bodies move: port modules compile and graph_check predicts Core8 -> 8 singleton SCCs. |
| OQ-4 | What are the numeric graduation targets and the attached baselines for Phase C (inline-defn thresholds) and Phase D (full module->area mapping table)? | @Zzzode | implementable gate | The phase table carries measured baselines and concrete thresholds; the utils mapping table exists as an RFC-attached artifact. |
| OQ-5 | Does sccache (the only such tool installed locally) demonstrably cache named-module BMI/object output with CMake 3.31 and clang 22, including on macos? | @Zzzode | Phase E cache sub-goal / G6 | A warmed second build reuses outputs for unchanged interfaces with measured wall-time saving; otherwise the cache goal is deferred, not faked. |

## 11. Implementation History

Append-only.

| Date | Event | Outcome / evidence |
|---|---|---|
| 2026-09-23 | RFC opened (provisional), graph audit of 849 modules | PSS and SCC measurements recorded in section 2; rfc_lint green |
| 2026-09-23 | First design review against the `accepted` checklist | REQUEST CHANGES: FTXUI wrapper sketch invalid (GMF includes are not exported), import-std per-TU constraint missing, Phase B port design unspecified, Phase F lacked a completion invariant, sccache assumed; G1/G3 not falsifiable. Logged as OQ-1..OQ-5 and sections 4.1-4.6 rewritten. Status deliberately remains `provisional`. |
| 2026-09-24 | Branch `rfc-0001-spike-import-std-ftxui` spikes OQ-1/OQ-2 (no `src/` changes on master) | Both mechanisms proven on clang 22/cmake 3.31: vendored std.cppm FILE_SET target works at c++23 (consumer 1.71s->0.13s, mixed TU tolerated); FTXUI headers compile to header units; end-to-end header-unit consumption deferred to CI cmake 4. OQ-1/OQ-2 updated in place. OQ-3/OQ-4/OQ-5 still open. |
| 2026-09-24 | OQ-3 design produced from the live graph (`attachments/0001-oq3-phase-b-cut-design.md`) | Agent cluster mapped by real symbols: 5 upward-importing modules (run/resume/fork/utils + the `cc.tools.agent` facade) have zero external importers -> promote to a new cc_orchestration target; 7 store/display/memory agent modules stay in tools. Other 7 edge families given per-edge cuts; 3 hooks->state imports proven DEAD (removed, full build green, then reverted pending Phase B). OQ-4/OQ-5 still open; status remains provisional. |
