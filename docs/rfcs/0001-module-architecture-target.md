---
rfc: 1
title: Module Architecture Target Shape
status: accepted
owners: "@Zzzode"
reviewers: ["agent:design-review#1", "agent:design-review#2", "agent:design-review#3 (approved)"]
created: 2026-09-23
last-reviewed: 2026-09-24
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
   milestone E0). The ONLY permitted cross-rank edges are into a module in a
   named contract package (`*.port`, `*.contract`, `cc.types`,
   `cc.config.*_types`) listed in `tools/arch/port_allowlist.txt`; the lint
   enforces a total layer rank, not convention.
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
  (achieved 2026-09-23). A single-digit-minute warm-cache PR build is a
  CONDITIONAL goal only: it stands if a BMI-capable compiler cache is proven
  on CI per OQ-5 (ccache 4.x / newer sccache); otherwise it is explicitly
  dropped, never claimed.

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
- Incremental CI with warm cache in single-digit minutes (CONDITIONAL on the OQ-5 cache proof; removed if not achievable).
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

| Milestone | Scope | Risk | Expected build payoff |
|---|---|---|---|
| **E0** | `tools/arch/graph_check.py` + port allowlist + dead-import check in CI (lands FIRST; A/B graduate against it) | low | structural enabler |
| A | `import std;` (incremental per file) + FTXUI per OQ-1 | med (spike-gated) | high |
| B | break Core8 per the REV 3 cut design (14 families, 52 edges, hooks/services reordered, service-backed tools lifted, family-14 type sinks), split non-UI libs -> 9 singleton SCCs | med–high | medium + structural |
| C | bodies out of god interfaces | low (mechanical) | high, incremental |
| D | re-home `cc.utils` (mapping attached; one deletion candidate reconciled in ctest) | low | low–medium |
| E | PSS sampler + (conditional, OQ-5) compiler cache | low–med | PR latency only if a BMI-capable cache is proven |
| F | UI state sharding, break UI9, split cc_ui | high | structural (own RFC) |

Order: E0 -> (A, C, D may proceed) -> B (gated on the graph_check
singleton prediction) -> E cache sub-goal if proven -> F. F awaits a
dedicated RFC after measured results from A–E.

## 10. Open questions

These gate the `accepted` / `implementable` transitions. None is answerable
without a branch spike or an explicit design decision; they are recorded
here so the gate cannot be passed on assertion.

| ID | Question | Owner | Blocks | Acceptance of the answer |
|---|---|---|---|---|
| OQ-1 | ~~How does FTXUI enter module units?~~ **Spike 2026-09-24 (branch rfc-0001-spike-import-std-ftxui): the clang mechanism WORKS** — FTXUI v5.0.0 headers build cleanly as user header units (`-fmodule-header=user`): color.hpp 8.3 MB / 1.3 s one-off, dom/elements.hpp 12 MB, component/component.hpp 19 MB / 2.0 s; FTXUI headers are module-compatible. Manual consumer wiring needs the scan-deps-generated module-map/`-fmodule-file` set, which is exactly what **CMake 4 `FILE_SET CXX_MODULE_HEADERS`** generates; CMake 3.31 (offline dev box) rejects the file-set type, so the end-to-end CMake path is validated on CI (brew cmake is 4.x). Remaining decision at implementable: full header set enumeration strategy (list FTXUI public headers wholesale vs. the subset Loom imports) and the leaf-subarea golden pilot. Status: mechanism proven, CMake-4 integration CI-gated. | @Zzzode | Phase A | CI pilot: one leaf UI subarea built through CXX_MODULE_HEADERS; truecolor goldens byte-identical or reviewed; consumer PSS/wall-time recorded vs textual. |
| OQ-2 | ~~Exact CMake recipe for `import std;`?~~ **Spike 2026-09-24: RESOLVED on the local toolchain.** (1) CMake 3.31 `CXX_MODULE_STD` exists but is gated behind an experimental UUID and builds std with `-std=gnu++23`, mismatching this repo `CXX_EXTENSIONS=OFF` (c++23) — rejected. (2) The robust path is vendoring the shipped `std.cppm` as an ordinary FILE_SET CXX_MODULES target, compiled with `-fno-implicit-module-maps -Wno-reserved-module-identifier` and an include dir at the toolchain `share/libc++/v1` (for `std/*.inc`); works at c++23 with ext OFF, CMake 3.28+, and carries whatever flags the preset already sets (so macos isysroot/libc++ flags flow naturally). Consumer micro-benchmark: a 10-header heavy TU **1.71 s -> 0.13 s (13x)**; std BMI precompiled once (35 MB). (3) The claimed "cannot mix textual std headers with import std" rule does NOT hold on clang 22 — a mixed TU compiled exit 0 — so conversion is incremental per file, not atomic per target (still prefer per-target commits). macos build of the same vendored std.cppm to be confirmed on CI. | @Zzzode | Phase A | vendored std module target builds under both presets; one leaf target converts and dual-preset + macos CI pass; 13x micro result reproduced inside a real producer TU before sweep. |
| OQ-3 | ~~Orchestration boundary and port shape?~~ **Design REV 3, 2026-09-24** ([attachment](attachments/0001-oq3-phase-b-cut-design.md)). Two adversarial Tarjan reviews drove it from REV 1 to REV 3: hooks/services reordered (voice logic behind hooks-owned ports, notifs MCP bridge moved to orchestration, 4 dead hook imports deleted); all **52** live upward edges owned across 14 families; service-backed tools (Mcp/Lsp/image file/SkillLoader) and the 25-edge agent subtree lifted to orchestration; family 14 sinks AgentConfig/permission DTOs to agent_types and moves only spawn_multi_agent/runtime_team_shared up, so the reverse tools->orchestration edges are cut. Completion invariant: **9 singleton SCCs** including orchestration. McpServerConfig x6 unified as a persisted-data leaf; ToolInput split so its json helper stays in tools. | @Zzzode | Phase B | Before bodies move: port/type modules compile and graph_check predicts 9 singleton SCCs. |
| OQ-4 | ~~Numeric baselines and utils mapping?~~ **Artifact attached 2026-09-24:** [OQ-4 baselines + mapping](attachments/0001-oq4-baselines-and-utils-mapping.md). Phase C: all 55 >=1000-LOC interfaces measured (8,950 inline defs; C1 top-6 = 2,328); concrete exits C1 <30/interface, C2 none >100, C3 lint warn-40/error-80. Phase D: all 171 utils modules classified into ~60 destination areas, with the 5 ambiguous ones content-read and resolved (image_store -> cc.media.images; pdf retained leaf; prompt_category is a deletion candidate with zero source importers; system_theme -> cc.platform.terminal; theme -> cc.ui.theme.types data leaf). | @Zzzode | implementable gate | Artifact exists and mapping reviewed; lint freeze-list produced during Phase D execution. |
| OQ-5 | ~~Does sccache cache named-module output?~~ **Spike 2026-09-24: NEGATIVE with the available tool.** sccache 0.4.0-pre.6 (the only such tool on the offline box) reports **"unknown source language" and non-cacheable for `.cppm` BMI compiles**; module implementation units execute but are not stored (1 executed, 0 hits/0 misses). Plain `.cpp` files DO cache (warm hit confirmed). Since the expensive outputs are exactly the `.cppm` producers, sccache gives no benefit for the cost that matters. | @Zzzode | Phase E / G6 | Options for implementable: (a) verify a newer sccache or **ccache 4.x** (not installed offline; testable via brew on CI) caches `.cppm`; (b) until then ship the architecture lint only and DROP the single-digit warm-cache claim from G6 - do not fake it. BMI-level caching may instead come from a future compiler-native/CMake module cache. |

## 11. Implementation History

Append-only.

| Date | Event | Outcome / evidence |
|---|---|---|
| 2026-09-23 | RFC opened (provisional), graph audit of 849 modules | PSS and SCC measurements recorded in section 2; rfc_lint green |
| 2026-09-23 | First design review against the `accepted` checklist | REQUEST CHANGES: FTXUI wrapper sketch invalid (GMF includes are not exported), import-std per-TU constraint missing, Phase B port design unspecified, Phase F lacked a completion invariant, sccache assumed; G1/G3 not falsifiable. Logged as OQ-1..OQ-5 and sections 4.1-4.6 rewritten. Status deliberately remains `provisional`. |
| 2026-09-24 | Branch `rfc-0001-spike-import-std-ftxui` spikes OQ-1/OQ-2 (no `src/` changes on master) | Both mechanisms proven on clang 22/cmake 3.31: vendored std.cppm FILE_SET target works at c++23 (consumer 1.71s->0.13s, mixed TU tolerated); FTXUI headers compile to header units; end-to-end header-unit consumption deferred to CI cmake 4. OQ-1/OQ-2 updated in place. OQ-3/OQ-4/OQ-5 still open. |
| 2026-09-24 | OQ-3 design produced from the live graph (`attachments/0001-oq3-phase-b-cut-design.md`) | Agent cluster mapped by real symbols: 5 upward-importing modules (run/resume/fork/utils + the `cc.tools.agent` facade) have zero external importers -> promote to a new cc_orchestration target; 7 store/display/memory agent modules stay in tools. Other 7 edge families given per-edge cuts; 3 hooks->state imports proven DEAD (removed, full build green, then reverted pending Phase B). OQ-4/OQ-5 still open; status remains provisional. |
| 2026-09-24 | Independent agent design reviews #1 and #2 | request-changes. #1 found a hooks↔services cycle from the voice port + uncut notifs→mcp bridge, 4 dead hook imports, 3 voice edges, and 11 unaddressed service-backed-tool edges. #2 (over 9 nodes incl. orchestration) found the REV-2 fix still left a tools↔orchestration SCC (4 team/spawn modules import the moved facade), family-12 count 5 not 1, total 52 not 48, McpServerConfig 6. Both verified by the reviewer’s own Tarjan run; REV 2 then REV 3 attached. |
| 2026-09-24 | Independent agent design review #3 (full re-graph, 850 .cppm + 34 .cpp, 1,825 internal edges) | **APPROVED.** Simulated all 14 families over 9 layered nodes = 9 singleton SCCs; live upward total exactly 52; all feasibility claims verified (agent DTOs std-only, ToolInput json split, voice ports leak no concrete types). Editorial non-blockers (stale REV-2/48/8 strings, runtime_team_shared optional move, test-seam note, config unique pair) applied. **Status -> accepted.** Implementable gate still requires: tracking issue, PRR fill, graph_check predicting 9 singleton SCCs, and CI confirmation of the FTXUI cmake-4 + macos import-std spikes. |
| 2026-09-24 | OQ-4 artifact attached (55-interface C baselines, 8,950 inline defs; all 171 utils modules mapped, 5 ambiguous ones resolved incl. one zero-importer deletion candidate). OQ-5 spiked: sccache 0.4.0-pre.6 cannot cache `.cppm` BMI ("unknown source language") - warm-cache goal contingent on ccache 4.x/newer sccache being verified on CI, else dropped from G6. All five OQs now have an answer/artifact; the design (accepted) gate is reviewable, status still provisional pending reviewer approval and the CI-gated FTXUI/import-std confirmations. |
