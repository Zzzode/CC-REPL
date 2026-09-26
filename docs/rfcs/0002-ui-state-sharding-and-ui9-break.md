---
rfc: 2
title: UI state sharding and breaking the UI9 SCC
status: provisional
owners: "@Zzzode"
reviewers: ["agent:design-review#1 (SOUND with required changes - numbers regenerated, minimum-cut corrected to 7 dirs/10 edges, row 8 extraction, F3 threading model; all applied 2026-09-26)"]
created: 2026-09-26
last-reviewed: 2026-09-26
---

# RFC 0002 — UI state sharding and breaking the UI9 SCC

## Summary

The 218 `cc.ui.*` named modules (219 interface declarations if the single
`:private` / `:impl` partition is counted) compile to a module-level DAG but
collapse at the **directory responsibility** level into one 9-area strongly
connected component (the "UI9 SCC": `foundation, chrome, dialogs, widgets,
messages, prompt, screens, features, permissions`). This RFC severs that SCC
by moving shared **types downward** into foundation/leaf modules, extracting
cross-cutting render helpers into lower leaves, inverting
framework→consumer dependencies via registries bound at the composition
root, and sharding the monolithic `ReplScreenState` (plain, UI-thread
affined data) into domain-owned stores. Only after the import graph and
`target_link_libraries` are both provably acyclic do we split the
deliberately-single `cc_ui` static library into ~12 area libraries. No UI
behaviour changes; the 56 truecolor golden suites gate every commit.

This is the RFC 0001 Phase F follow-up RFC. **Implementation does not
start until this RFC is accepted and RFC 0001 Phase B lands.**

## Motivation

Module-level acyclicity is necessary but not sufficient: the named module
graph is a DAG, yet nine UI directories are mutually reachable, so they
cannot be separate libraries and a change in one UI area forces a BMI
recompile across the UI closure. Measured 2026-09-26 (read-only from
`tools/arch/graph_check.py` data; the 32 edge inventory is in
[attachments/0002-ui9-edge-inventory.md](attachments/0002-ui9-edge-inventory.md)):

### Evidence

| Metric | Current | Target | How measured |
|---|---|---|---|
| `cc.ui.*` modules | 218 (219 declarations incl. 1 `:private` partition) | unchanged (moves, no deletes) | `export module cc.ui` count |
| UI areas (2nd-level) | 12 (9 in the SCC + app/tools/visual) | 12 singleton areas | area-level Tarjan over cc.ui imports |
| UI area SCCs > 1 | **1 SCC of 9 areas** | all 12 singleton | Tarjan (independent 9! FAS search) |
| Back edges inside the SCC | **34** area-directions incl. module-impl `.cpp` (32 interface-only); 172/132 module-edge pairs (all-TU / cppm-only) | 0 | inventory attachment; F0 lints `*.cppm + *.cpp` |
| Minimum feedback arc set | **7 area directions (10 module edges)** sever the SCC | 0 | exhaustive 9! order search |
| Already-singleton UI areas | 3 (`app`, `tools`, `visual`) | all 12 | same SCC run |
| `ReplScreenState` | 739 LOC, ~100–114 fields, 12 cc-imports | UI-thread-affined domain stores | wc / field count |
| `target_link_libraries` graph | **already acyclic (0 TLL SCCs, no --start-group)** today | stays acyclic, asserted by lint | parsed TLL graph |
| `cc_ui` static libraries | 1 (intentionally; name-only SCC via cross-target file ownership) | ~12, after graph+TLL acyclic | file→lib grouped by module-name area |
| Edit fan-out / PSS | large UI closure BMI | per-area, measured: PSS/PCM/wall via `measure_bmi.py`; fan-out via import-closure/ninja | both tools; numeric threshold set at implementable |

The link rationale in earlier notes is corrected here: ld.lld / ld64
resolve cyclic static archives to a fixpoint (the cyclic Core8 archives
already link once, without `--start-group`). Therefore the F invariant is
**graph acyclicity plus acyclic `target_link_libraries`, both asserted by
lint** — not "the linker rejects cycles". Linking today does not make the
SCC harmless: the cost is BMI fan-out, recompilation blast radius, and the
inability to ship independently-built area libraries.

## Goals

- G1. `tools/arch/graph_check.py --target-ui9` passes: the 9 SCC areas
  plus `app/tools/visual` are pairwise singleton SCCs (12 total).
- G2. A new lint asserts the corresponding area `target_link_libraries`
  graph is acyclic.
- G3. `ReplScreenState` is sharded into UI-thread-affined domain stores
  named `cc.ui.<area>.state_store` (or registered in a F0 allowlist);
  `--target-ui9` + a store-naming/import lint proves no UI area reaches
  "up" for state and the composition root wires, not owns, the stores.
- G4. `cc_ui` splits into ~12 static libraries (grouped by **module-name
  area**, the same key F0 uses — name/path decoupling means the grouping
  rule must be explicit) only after G1–G2; each area library builds with
  an acyclic dependency set. The `target_link_libraries` graph is already
  acyclic today; the split must keep it that way.
- G5. Producer PSS / BMI / wall measured with `measure_bmi.py` and edit
  fan-out measured as the recompiled-closure on a one-body vs one-interface
  edit (ninja/`-n`), before/after per phase; concrete pass thresholds set
  at the implementable gate from the F0 baseline.

## Non-Goals

- No user-visible behaviour, render output, colour, spacing or event change.
- No rewrite of the FTXUI event model or the component-state convention
  ("FTXUI components must be held by state").
- No re-architecture of non-UI code (that is RFC 0001 Phase B).
- No textual FTXUI header adoption (revisit at clang ≥ 23 + cmake 4 per
  RFC 0001 Phase A).
- No C++ namespace churn beyond what a module/type move strictly needs.

## Proposal

Apply, per back edge, exactly one of three mechanisms, chosen by what the
import actually carries:

1. **Type sink.** When area A imports area B only for a plain data type /
   enum (`RiskLevel`, `PastePreview`, a primitive), move that type to a
   lower leaf both already depend on (foundation or a new shared-types
   module). The producer keeps behaviour; consumers import the leaf.
2. **Registry inversion.** When A imports B to *invoke* B concrete UI
   (a dialog renderer, a wizard, feature panels), define a type-erased
   registration point (`std::function` / `shared_ptr<void>` slots — the
   pattern already proven by `repl_state.cppm` opaque handles) on the lower
   side; the concrete implementation registers from the composition root.
3. **Delete the edge.** Some back edges are dead `import`s or umbrella
   `export import`s kept only for include convenience; after a full
   dual-preset build proves no use, remove them (the RFC 0001 Phase C/B
   pattern; watch LLVM #184957 keep-imports — never text-only deletion).

```
after F (area edges point downward only):

  app ──▶ screens ──▶ features ──▶ dialogs ──▶ widgets ──▶ foundation
   │           │            │             │
   └──────────▶┴──▶ prompt ──┴──▶ messages ─┴──▶ permissions ──▶ shared-types
        chrome / visual / tools are leaves or independently ordered
```

The exact 34 area-direction edges (32 from `.cppm`, +2 contributed by
module-impl `.cpp` units) are in the attachment. An exhaustive 9! feedback-
arc-set search gives a **minimum cut of 7 area directions / 10 module
edges** — RFC rows **{1,2,3,4,6,7,8}** sever the SCC under the total order
`screens > dialogs > features > messages > permissions > widgets > prompt
> chrome > foundation`. Rows **5 and 9 are NOT required for acyclicity**:
they are deliberate *decoupling* (stop a framework importing concrete
feature panels; re-home leaf modules) and can either be included for
independence or left as legal downward edges. The FAS is recomputed by the
F0 lint after every phase since moving types changes the graph.

### Detailed design — back edge → ownership change

| # | Edge (verified site) | Carries | Mechanism |
|---|---|---|---|
| 1 | foundation→chrome `foundation/logo.cppm:11` | import with no body reference | **delete** after dual-preset + mac verify (possible #184957 keep-import) |
| 2 | foundation→widgets `foundation/design_extras.cppm:26` (`custom_select`, used at `:143`) | a custom widget primitive | **sink** the primitive into foundation, or move the helper up |
| 3 | widgets→dialogs `widgets/all_components.cppm:19` (`export import feature_dialogs`) | umbrella re-export | **delete** umbrella; point importers at the app composition layer |
| 4 | dialogs→screens `dialogs/dialog_default_renderers.cppm:40` (`doctor_screen`) | concrete screen renderer | **registry inversion**: doctor registers its renderer from the screens side into `DialogRendererRegistry` |
| 5 *(decoupling, not in the 7-dir minimum)* | dialogs→features `plugin_dialog.cppm:41-44` (4 panels), `hooks_dialog_renderer_impl.cpp:23` | concrete feature panels | **registry inversion** via a registration *protocol leaf BELOW both areas* (ViewKind enum keys + typed `std::function` descriptors/factories); concrete `static_pointer_cast` confined to composition-root TUs. Not needed for G1; do it to stop the framework importing concrete panels |
| 6 | features→dialogs `agent_wizard.cppm:49`, `task_wizard.cppm:23`, `plugin_install_flow.cppm:25-26` | generic wizard/trust framework | framework stays dialogs-side (`wizard_dialog` is already a zero-cc.ui-import leaf); make consumers **registration-driven** through the row-5 protocol leaf |
| 7 | messages/permissions→dialogs `messages_interactions.cppm:52,58`, `permission_advanced_prompts.cppm:39,46` | **only** `RiskLevel` | **extract the `RiskLevel` enum** into a shared-types leaf below both. Do NOT move `trust_utils` (it imports commands/plugins/services/bash_security) |
| 8 | prompt→messages `prompt_input_footer.cppm:491` calls `msgs::ansi_to_ftxui_elements` from `message_tool_result.cppm` | a *render converter* that pulls in chrome.terminal_io + messages.message_components + visual.markdown | **EXTRACT, do not move the module**: lift `ansi_to_ftxui_elements` + `apply_sgr_run` (which need only `SgrAttr` + ftxui) into a new leaf allowed to depend on chrome; moving `message_tool_result` wholesale would recreate the cycle |
| 9 *(decoupling, not in the 7-dir minimum)* | widgets→prompt: `text_input.cppm:30` uses `PastePreview` (prompt_paste_handler), `text_input_widget.cppm:26-27` placeholder + it CALLS `build_combined_highlights()` (`:493`; the prompt side of that edge also originates in `text_input_render.cpp:25`) | type **and behaviour** | **move the whole leaf modules** (they import only foundation/cc.utils, so already lower-able), not type-only pieces. Legal to leave under the minimum total order |
| 10 | messages→ui.tools.registry/generic (2 edges); widgets→visual.markdown (1; markdown also imported by dialogs 3, messages 5, permissions 3, screens 2, features 1) | registry is a zero-import leaf; markdown is a pure visual leaf | **rank** ui.tools below messages; order visual as a pure leaf below all six importer areas |

The attachment lists all 34 area-directions / module-edge pairs; the **rows
{1,2,3,4,6,7,8} are the minimum cut** and rows 5/9 are optional decoupling.
The remaining same-SCC edges become legal downward edges under the verified
total order.

### State sharding (F3)

`ReplScreenState` (739 LOC, ~100 top-level fields) currently co-locates
fields for messages, prompt, tasks, permissions, dialogs, MCP and shell in
one plain struct. **Threading model (corrected after review):** the state
struct itself holds only one mutex (`pending_at_mention_mutex`) and no
jthreads/condition variables. The concurrency lives in **AppImpl (the app
composition area)**: `query_thread_`, `spinner_thread_`, `bash_thread_`,
`leader_inbox_thread_`, `statusline_thread_` plus ~7 mutex/CV pairs
(`result_mutex_`, `paste_mutex_`, `bash_result_mutex_`, `permission_mutex_`/
cv, `elicitation_*`, `ask_user_*`, `statusline_*`). The invariant today is
**plain stores mutated only on the UI thread; worker threads stage results
into composition-owned queues and surface them with `PostRenderEvent`**.
State sharding must preserve, not relocate, that model:

1. Stores are **UI-thread-affined plain data**; do NOT retrofit per-field
   mutexes into them. Cross-store selectors/accessors are UI-thread-only.
2. The ~12 typed cross-area state fields (`AgentCardData`, `WizardDraft`,
   `LiveTeammate`, footer/voice footer types, `DialogQueue` (dialogs.system),
   `StreamingMarkdown`, …) are **classified up front** per F1's mechanisms
   — opaque `shared_ptr<void>` handle vs sink-to-shared-type vs same-store.
   Mis-classifying one silently recreates an area up-edge.
3. All worker threads, staged queues, mutexes and CVs stay in the AppImpl
   composition layer; they never move into a store.

Split incrementally into domain stores (`MessagesStore`, `PromptStore`,
`TaskViewStore`, `PermissionStore`, `DialogStore`, `McpStatusStore`, …);
`AppAdapter` constructs and connects them. Cross-store reads go through
selectors, never a direct field reach-up. A **re-export shim** keeps call
sites compiling during the move; each shim has a declared removal and the
gate is one store landed per commit with all shims deleted by end of F3.

### Library split (F5, last)

Once `--target-ui9` and the TLL lint pass, split `cc_ui` into ~12 area
static libraries (`cc_ui_foundation`, `cc_ui_chrome`, `cc_ui_widgets`,
…) in dependency order, using the same `include()`-per-target /
one-scope CMake discipline as RFC 0001. **File→library grouping is by
module-name area (`cc.ui.<area>`), exactly the F0 mapping** — module names
are decoupled from paths, so the rule must be stated, not inferred from
directory. Note the `target_link_libraries` graph is **already acyclic
today** (0 TLL SCCs, no `--start-group`); the residual Core4/UI9 cycles
are module-*name*-level only because targets already own files across name
prefixes. F5 must keep TLL acyclic and add a lint asserting it.
CLAUDE.md's warning that the nine directories collapse SCC-wise
(foundation↔chrome, dialogs↔widgets, messages↔prompt) is exactly what
F1–F3 removes first.

## Phases and graduation criteria

| Phase | Title | Scope | Status | Graduation criteria (measured) |
|---|---|---|---|---|
| F0 | Lint + freeze | add `--target-ui9` gate (globs `*.cppm` + module-impl `*.cpp`) and `ui_back_edge_baseline.txt` (34 area-directions); no code yet | proposed | gate FAILS deterministically on the frozen edges and on any new edge |
| F1 | Type sinks + extracts | rows 1,2,3,7 + extract row 8 + delete row 1 | proposed | 7-direction minimum cut partly landed; SCC shrinks per cut; no new interface over 100 inline bodies; goldens identical |
| F2 | Registry inversion | rows 4,6 (+ optional decoupling row 5) via a below-both protocol leaf | proposed | no dialog↔features mutual reach; concrete casts confined to composition-root TUs |
| F3 | State sharding | UI-thread-affined domain stores; typed cross-area fields classified first | proposed | no UI area reaches up for state; one store per commit, all re-export shims removed by end-of-F3; mutex/jthreads stay in AppImpl; 1706 + goldens |
| F4 | Composition root | resolve the 58 frozen inline bodies in `AppAdapter`; wire stores/registrations once | proposed | AppAdapter is composition only |
| F5 | Split cc_ui | ~12 acyclic area libraries | proposed | `--target-ui9` + TLL lint pass (12 singletons); per-area PSS/fan-out measured |

Per change: dual-preset `-Werror` green, serial ctest -j1 green, producer
PSS/fan-out recorded with `measure_bmi.py`, independent adversarial
agent review, macos-14 gate.

## Production Readiness Review

Filled at the `implementable` gate (this is `provisional`). Known PRR
points: correctness tests (1706 baseline) and truecolor golden suites are
the behaviour net; rollback per phase is a revert; observability traces
(messages.jsonl / dump-prompts) and the `<task_notification>` /
`<status>` / `<summary>` tag shapes must remain byte-identical.

## Rollout and rollback

- Each phase is an independent, revertible commit sequence on master
  behind its own macos-14 gate; F0 ships a failing-frozen lint with no
  behaviour change.
- Type sinks and registry inversions keep a temporary re-export /
  registration shim so a phase can be reverted without a flag-day.
- F5 library split is the only CMake-structure change and lands last,
  after both graph and TLL are acyclic; reverting it restores the single
  `cc_ui` target without touching source.
- Ordering: F0 → F1 → F2 → F3 → F4 → F5. **F0, F1 and F2 are lint/type/
  registration work and bind at the existing app root (it already imports
  every UI area), so they do NOT wait on RFC 0001 Phase B.** **F3 and F4**
  (state stores and a single composition root) **gate on RFC 0001 Phase B**
  (`cc.orchestration`) to avoid reworking the composition seam; Phase E
  (the `measure_bmi.py` sampler) is already landed.

## Drawbacks

- Registries replace some direct typed calls with type-erased slots,
  adding a small indirection and moving some wiring errors from compile
  time to the composition root (mitigated by strong single-root tests).
- The F3 state split touches shared mutable state read by worker threads;
  it is the highest-risk phase and must move one store at a time.
- More libraries and a slightly larger CMake surface.

## Alternatives considered

- **Do nothing.** Leaves a 9-area SCC, full-UI BMI fan-out, and blocks
  any area-level library/incremental build. Loses on the measured metrics.
- **Split cc_ui across the real SCC now.** Explicitly rejected in CLAUDE.md
  and RFC 0001: cyclic libraries need link groups and hide the structural
  problem; graph acyclicity comes first.
- **Build `-j` throttling / textual-header revert.** Pre-decided losing
  moves (memory is solved by splitting TUs; FTXUI header units fail on
  clang 22).
- **Big-bang rewrite of UI state.** Too risky against 56 golden suites and
  threaded queues; the incremental shim plan wins.

## Testing and verification plan

- ctest baseline **1706 @ 2026-09-26** (reconcile every change); every
  phase serial `-j1` on debug and release, dual `-Werror`.
- The 56 truecolor / E2E golden suites must be byte-identical each commit
  (no UPDATE_GOLDENS in this RFC — output does not change).
- New lint: `--target-ui9` and the acyclic-TLL assertion run in the
  lightweight arch-check workflow, never behind the mac build.
- Concurrency: targeted tests for prompt/messages/permission/observer
  paths through the sharded stores; no new timing assumptions.

## Documentation impact

- [ ] `CLAUDE.md` — the `cc_ui` single-target / SCC note updates when F5 lands
- [ ] `docs/decisions/design-decisions.md` — registry-inversion and
  type-sink decisions, and the corrected link-cycle rationale
- [ ] New module/area doc headers for the shared-types leaf and stores

## Open questions

| Question | Owner | Resolved by |
|---|---|---|
| Row 6: invert features→dialogs via registry vs re-rank areas by dependency weight | @Zzzode | F2 design, measured edge weight |
| Name/home of the new shared-types leaf (foundation vs a new `cc.ui.shared`) | @Zzzode | F1 |
| Exact store boundaries for F3 (one per area vs fewer) | @Zzzode | F3 design read of repl_state field graph |
| Minimum sever set is 10 in the current graph — re-verify after F1/F2 change it | agent | F0 lint + re-run cut analysis per phase |

## Implementation History

| Date | Phase | Event | Commit / PR | Evidence (metrics, test totals) |
|---|---|---|---|---|
| 2026-09-26 | — | RFC opened (provisional) after RFC 0001 A/C/D; measured 219 modules, 9-area SCC, 32 back edges, repl_state 739 LOC; awaits RFC 0001 Phase B + E before any code | — | discovery workflow wvjr74s34; graph inventory |
