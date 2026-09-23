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

### 4.1 Phase A — std / FTXUI entry points (largest remaining compile lever)

- Build the `import std;` BMI once in CMake; replace the 8,768 textual std
  includes in module units.
- Introduce `cc.third_party.ftxui`:
  ```cpp
  module;
  #include <ftxui/component/component.hpp>
  // …the complete FTXUI surface Loom uses…
  export module cc.third_party.ftxui;
  ```
  Convert the 165 interfaces that textually include FTXUI to
  `import cc.third_party.ftxui;`.
- **Pilot first** in one UI sub-area; measure producer PSS and wall time
  before the tree-wide sweep.
- Acceptance: zero textual `<ftxui...>` / std-header includes in module units
  (enforced), app-closure PSS non-increase and expected decrease.

### 4.2 Phase B — break the Core8 SCC (small, sharp, newly found)

The non-UI SCC is held together by about eight concrete backward edges, not a
general tangle. Cut list:

| Backward edge | Edges | Fix |
|---|---|---|
| `services.streaming_executor → tools.tool` | 1 | move the executor up, or define the tool port in `tools` |
| `utils.ide_integration → services.mcp` | 2 | relocate module into `services` (misfiled) |
| `skills.bundled.debug → tools.tool` | 1 | invert / isolate behind a port |
| `state.teammate_view_helpers → task_types` | 1 | sink type or merge modules |
| `tools.mcp → hooks / config` | 2 | callback inversion |
| `hooks.voice, prompt_suggestion, turn_diffs, assistant_history → services/state` | 9 | port in `hooks`, implementation in services, injected |
| `tools.agent.{run,resume,fork} → services.api / skills` | 36 | **promote agent orchestration to its own layer** (`cc.orchestration`) |
| `config → utils.json` / `state → utils` | benign downward | none |

Acceptance: Tarjan over directory nodes reports one component per directory;
`cc_utils`/`cc_tools`/`cc_services` split into real static libraries; CI fails
on any new cross-area back edge.

### 4.3 Phase C — move bodies out of god interfaces

Attack the 55 interfaces over 1,000 LOC in descending cost order
(agent_runtime, agent.utils, query_engine, runtime_registry, repl_screen
renderers, messages_list, text_input, …). Same mechanical pattern proven by
the `AppImpl` work: declarations stay, bodies move to module implementation
units, hidden state goes behind internal partitions or type erasure.

Acceptance per change: debug + release `-Werror` green, ctest green, and BMI
PSS / importer fan-out recorded. No behaviour change.

### 4.4 Phase D — dissolve the `cc.utils` junk drawer

171 modules, 163 flat. Re-home by domain:

- `cc.json` / `cc.yaml` / `cc.text` / `cc.fs` — serialization and text/file
- `cc.process` — bash execution, abort controller, async primitives
- `cc.crypto` — hashing / encoding
- `cc.platform` — clipboard, terminal helpers, env, paths
- misplaced modules move to their real area (ide_integration → services;
  swarm_* → the teams domain)

`json` (fan-in 121) and `error` (fan-in 59) stay pure leaves.

Acceptance: no new top-level module under `cc.utils.*`; every former module
reachable under a sub-domain namespace; no importer churn beyond the
path/name moves (module names decouple from paths, so most moves are
CMake-only).

### 4.5 Phase E — build-system hardening

- Enable **ccache/sccache with CMake named-module BMI support** and GHA cache
  so non-cold CI is minutes, not 80.
- Add the **architecture linter**: a small graph script (the same Tarjan
  analysis used for this RFC) that fails CI on (a) a new directory-level
  cycle, (b) textual FTXUI/std includes in a module unit, (c) new god
  interfaces above a body-count threshold.
- Keep the default Ninja parallelism. Memory is solved by splitting TUs, not
  throttling jobs (standing project rule).

### 4.6 Phase F — UI state sharding and the UI9 SCC (last, optional)

Only after A–E have landed and their numbers are in. Split `ReplScreenState`
into domain-owned stores and reduce `AppAdapter` to a composition root. When
the UI9 directory SCC dissolves, split `cc_ui` along the §3.1 lines. This is
the only phase with user-visible behavioural risk and should be designed in
its own follow-up RFC.

## 5. Goals

- G1. Producer BMI PSS for the app/UI closure drops materially again (target
  recorded at the implementable gate) and editing a function body recompiles
  one object file, not an import fan-out.
- G2. The module graph contains no directory-level SCC larger than one
  responsibility area; the invariant is enforced in CI.
- G3. Standard library and FTXUI enter module units only via `import std;`
  and a single `cc.third_party.ftxui` wrapper.
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
| `import std;` / FTXUI wrapper exposes a Clang 22 module bug | Phase A pilots one sub-area; keep textual fallback branch until PSS/wall-time win is confirmed |
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
| A | `import std;` + FTXUI wrapper | low–med | high |
| B | break Core8 SCC, split non-UI libs, add graph lint | med | medium + structural |
| C | bodies out of god interfaces | low (mechanical) | high, incremental |
| D | re-home `cc.utils` | low | low–medium |
| E | ccache + architecture CI | low | very high for PR latency |
| F | UI state sharding, break UI9, split cc_ui | high | structural |

A–E are proposed for immediate scheduling as independent commits; F awaits a
dedicated RFC after measured results from A–E.
