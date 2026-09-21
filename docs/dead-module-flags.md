# Dead modules that are actually wiring bugs

Fifteen modules have zero importers but are **not** dead in the cleanup sense: a
live sibling already does their job, and the dead module was the intended
source. Deleting them would have quietly ratified the bug — so they were kept
and are listed here instead.

This is the residue of a large dead-module sweep (278 other modules were
deleted; see commit `f9f69eb`). The distinction that matters:

- **Feature removed / never ported** → delete. No registry entry point, no
  registered sibling, nothing expecting it. That was the other 278.
- **Wiring forgotten** → this file. There is a live artefact shaped exactly like
  the socket this module plugs into.

Each entry states the evidence and a suggested fix. Verify
the evidence before acting — line numbers drift.

**Three entries have been re-verified and found wrong** (§6, §7, §9-10). The
pattern is consistent enough to state as a rule: the *evidence* in this file is
sound, but the *recommendations* repeatedly inverted on inspection.

- §6 recommended deleting the un-imported figures module. It turned out to be
  one of only two faithful copies of a glyph that has **drifted** in the
  heavily-imported copy — so the import count pointed at the wrong survivor.
- §7 claimed a live module held "the same" constants. The two modules share
  exactly two, differently-spelled names and have different shapes entirely.
- §9-10 called both modules "shadowed names". One is (and was deleted); the
  other, `cc.vim.vim_commands`, is a complete ex-mode registry that is merely
  unreachable because a live hook carries its own weaker stub.

Treat the remaining unchecked entries' recommendations as hypotheses, not
instructions. Two habits would have prevented all three errors: **an import
count is not evidence of correctness**, and **"same name" is not "same thing"** —
read what the module actually does, not what it is called.

## Disposition record (2026-09-21) — adversarial verification

The candidate set (the `cc.utils` aggregator, the sixteen modules it alone
reached, and four other zero-importer modules) was run through a **two-stage
adversarial verification**: 23 modules × one skeptic each, each checking both
reachability (declared module name, `export import` chains, `extern "C"`,
template registration, tests, CMake) and capability preservation (is the live
counterpart equally strong, a stub, or a drifted duplicate?), then an
independent **refuter** for every SAFE verdict that tried to construct a reason
to keep it. 46 agents, zero failures. A module was deleted only when both the
skeptic and the refuter agreed it was safe. The per-module evidence is in the
workflow journal; this table is the ruling.

**Deleted (16)** — both agents agreed: unreachable AND no capability lost.

| module | why nothing is lost |
|---|---|
| `cc.utils` | the aggregator itself; convention is direct imports (`json` ×144), 0 importers, every member independently CMake-listed |
| `cc.utils.bash_parser` | `bash_execution` (45 importers) covers `is_dangerous_command`/`split_shell_command` |
| `cc.utils.memory` | `cc.memdir.paths` `MemoryType`; `AutoMem` consumed by `query_engine.cppm:971` |
| `cc.utils.memdir` | live `src/memdir/` target (`cc.memdir.*`) |
| `cc.utils.session` | live `src/session/` target (`cc.session.*`) |
| `cc.utils.terminal` | `cc.utils.hyperlink`, `cc.hooks.terminal_size`, `ui/rendering/ink_utils.cppm` |
| `cc.utils.input_router` | `command_registry` slash parsing + `parse_references` + `bash_execution` |
| `cc.utils.sandbox` | `tools/should_use_sandbox.cppm` + `bash/impl_bash.cppm` enforce it; only the Docker/nsjail adapters were unhoused (unused) |
| `cc.utils.query_helpers` | `token_budget.cppm:259` `should_compact` + `query_engine` utilization math |
| `cc.utils.suggestions` | live autocomplete pipeline (`autocomplete_sources`, `app_autocomplete`, `fuzzy_rank_nucleo`) |
| `cc.utils.native_utils` | the one real capability (background-task tracking) has a strictly stronger live home in `cc.tasks.task_graph` (libuv, timeout, cancel); installer/DXT halves were non-functional stubs |
| `cc.constants.tools_constants` | all three agent tool-access sets are strict supersets in live `agent_sub_utils.cppm` (called from `agent_tool.cppm`); the 30 string values exist as live per-module `kToolName`/literals; the one unmatched set (`coordinator_mode_allowed_tools`) never took effect in C++ |
| `cc.skills.bundled.remember` | in-memory KV store + manifest; shipped skill text comes from root `cc.skills.remember` |
| `cc.skills.bundled.simplify` | shipped skill comes from root `cc.skills.simplify` |
| `cc.skills.bundled.batch` | bundled.cppm ships its local `make_batch_skill()` |
| `cc.hooks.notifs.rate_limit_warning` | one notification nothing emits |

**Kept (7) — all `KEEP_CAPABILITY_LOSS`.** Unreachable, but each holds a real
capability the live tree does **not** match in strength. These are consolidation
candidates, not cleanup; deleting them would silently ratify a regression.

| module | the capability the live tree lacks |
|---|---|
| `cc.utils.pdf` | the tree's only real PDF **text extraction**; live `file_read_tool::read_pdf` merely base64s the file (no extraction, no page count, no markdown), and the OpenAI wire drops that block |
| `cc.utils.task_output` | generic **8 MB-then-disk spill** with 5 GB retention, tail, progress polling, tmp lifecycle; live paths are unbounded in-memory or a last-30 KB trim |
| `cc.utils.agent_model` | `LOOM_SUBAGENT_MODEL` override, same-tier collapse to the parent model, true inherit-to-parent; live resolver hardcodes per-alias IDs and a fixed sonnet default |
| `cc.utils.code_indexing` | zero-dependency offline regex symbol index over 7 languages; the live LSP tool returns `ServerNotConnected` with no fallback when no server is installed |
| `cc.utils.cache` | generic TTL cache with proactive cleanup and `SimpleCache`; the three live copies are strictly narrower (path→string lazy-TTL, no-TTL token cache, single TTL snapshot) |
| `cc.utils.swarm` | provider/proxy env forwarding to tmux teammates and per-PID `-L` swarm-socket isolation (an explicit TODO at `swarm_backends.cppm:1341` admits it is unported); also its `HIDDEN_SESSION_NAME` has **drifted** from both the kept value and TS — resolve the constant before consolidating |
| `cc.skills.bundled.verify` | a 348-line working multi-language check/auto-fix engine; its only live counterpart is static LLM prompt text with no executable hook — an unfinished never-wired feature |

The recurring shape across the seven is the one §6 and §9-10 first exposed:
**the reachable implementation is the weaker one.** Resolve these by deciding
whether the capability should ship (wire it / port the body into the live
module) or be deliberately abandoned — not by an import-count sweep.

## Confirmed wiring bugs

### 1. `cc.ui.dialogs.elicitation` (224 LOC) — a registered dialog with no renderer

`DialogType::Elicitation` exists and is handled, but this module's
`RegisterElicitationDialog(DialogRendererRegistry&)` — defined at
`ui/dialogs/elicitation_dialog.cppm:204` — is **never called**. Meanwhile
`ui/dialogs/dialog_default_renderers.cppm:398-426` hand-inlines
`RenderElicitation` and `HandleElicitationEvent` for the same
`dsys::ElicitationPayload`.

**Fix:** call `RegisterElicitationDialog` from the default-renderer
registration and drop the inline pair, or the reverse — but not both. There are
two implementations of one renderer and only one is reachable.

**These two are not equivalent, so this is a behaviour decision, not a swap.**
Verified against both sources:

| | orphan `elicitation_dialog.cppm` | live `HandleElicitationEvent` |
|---|---|---|
| keys | `y`/`Y`, `n`/`N`, Enter, Esc | Enter, Esc only |
| Esc fires | `on_cancel()` | `on_response(false)` |
| footer | `[y] Allow  [n] Deny  [Esc] Cancel` | `[Enter] Approve  [Esc] Deny` |

The orphan is a strict superset: it honours the two single-key shortcuts and it
uses the distinct `on_cancel` path that `ElicitationPayload` documents
(`dialog_system.cppm:390` — "Esc fires on_cancel() instead of falling back to
on_response(false)"). The live version conflates Cancel with Deny. Adopting the
orphan therefore *changes what the user sees* — do not file this as a mechanical
wiring fix.

Producer side, for whoever resolves this: `PushElicitation`
(`ui/dialogs/triggers.cppm`) is the only thing that populates `on_cancel`, and
until recently it captured a moved-from `std::function`, so `on_cancel` was
always empty — the two renderers behaved identically on Esc only because the
orphan kept falling through to its `on_response` branch. That is fixed; if the
orphan is adopted, its `on_cancel` branch is now live for the first time.

### 2-5. Four `cc.skills.bundled.*` orphans — shadowed by inline copies

`skills/bundled.cppm` is the live bundled-skill registry. For four skills it has
a **local** definition that shadows the submodule:

| module | LOC | what `bundled.cppm` does instead |
|---|---:|---|
| `cc.skills.bundled.verify` | 348 | local `inline make_verify_skill()` at `:185`, pushed at `:452` |
| `cc.skills.bundled.batch` | 139 | local `inline make_batch_skill()` at `:220`, pushed at `:469` |
| `cc.skills.bundled.remember` | 77 | calls `cc::skills::remember::make_remember_skill()` (`:449`) — the **root** module, not this one |
| `cc.skills.bundled.simplify` | 129 | calls `cc::skills::simplify::make_simplify_skill()` (`:459`) — the root module |

Note these are **not drop-in replacements**: the submodules export
`get_*_skill_manifest()` and carry richer config structs (`VerifyConfig` etc.),
while the inline copies build a `SkillDefinition` directly. And the header
comment at `bundled.cppm:9,32` claims these are "OK migrated (Phase 0)" — so the
intent was to use the submodules and the local copies are leftovers.

**Verified 2026-09-21 — the framing above is wrong, and the four split 2-2.**
The core claim ("the inline copies shadow the submodules, the intent was to use
the submodules") does not survive inspection:

- **None of the four submodules exports a `SkillDefinition` builder at all.**
  So they were never drop-in replacements for the inline `make_*_skill()`
  functions, and the inline copies cannot be "leftovers" of them.
- The submodules are **different kinds of thing** that merely share a name:
  `bundled/verify.cppm` is a 348-line verification *engine* (`VerifyConfig`,
  `run_verification`); `bundled/remember.cppm` is a 77-line in-memory
  key-value *store* (`remember`/`recall`/`forget`/`list_memories`) plus a
  `SkillManifest`. The inline copies are prompt-text `SkillDefinition`s.
- `run_verification` and `VerifyConfig` are referenced **nowhere** outside
  their own file — not even by a test.
- `bundled.cppm` imports `cc.skills.remember` and `cc.skills.simplify`, the
  **root** modules (`:83-84`) — it does not import `cc.skills.bundled.*` for
  those two at all. So the `remember`/`simplify` rows are not "shadowed"; the
  bundled copies are simply never referenced under any name.

**Revised fix — two distinct calls, neither of them "swap the pair":**
- `bundled/verify` and `bundled/batch`: unrelated engines nothing calls. Delete
  both, or keep `verify` only if the verification engine is a feature you intend
  to wire up (it is the most substantial thing here, and the only one that looks
  like an unfinished feature rather than a duplicate).
- `bundled/remember` and `bundled/simplify`: delete. The live skill text comes
  from the root modules, and the bundled copies are a parallel store/manifest
  with no importer and no call site.

### 6. `cc.constants.figures` (62 LOC) — one glyph set, three copies

`LIGHTNING_BOLT` is declared in three places:

| file | form | imported |
|---|---|---:|
| `constants/constants.cppm:196` (`namespace figures`, `k`-prefixed) | `kLightningBolt = "↯"` | 6× |
| `constants/figures.cppm:24` | `LIGHTNING_BOLT = "↯"` (escape) | **0×** |
| `ui/components/figures.cppm:15` | `LIGHTNING_BOLT = "↯"` (literal) | 19× |

**The suggested fix here was wrong — do not delete `constants/figures.cppm` on
the strength of the importer count.** The escape form is *not* a difference: all
24 glyphs shared between `constants/figures.cppm` and `ui/components/figures.cppm`
decode to identical code points, so the escape notation was purely cosmetic.

But comparing them turned up a **real, unreported divergence**, and it inverts
which copy is authoritative. `BRIDGE_READY_INDICATOR` exists in **four** places,
in two spellings:

| file | value | code points |
|---|---|---|
| `ui/design/figures.cppm:192` | `·✔︎·` | U+00B7 U+2714 U+FE0E U+00B7 |
| `constants/figures.cppm:59` | `·✔︎·` | same, via `✔︎` |
| `ui/components/figures.cppm:44` | `·✓·` | U+2713 — **drifted** |
| `constants/constants.cppm:219` | `·✓·` | U+2713 — **drifted** |

The first two are the TypeScript-faithful value, and a **passing test pins it**:
`tests/test_ui_light.cpp:1471` `Figures.BridgeReadyIndicatorIsTsFaithful` asserts
the 10-byte `·✔︎·` sequence against `cc::ui::design::figures`, with a comment
recording that the C++ previously held emoji `✅︎` and was corrected. The two
drifted copies hold the *pre-correction* shape minus the emoji.

So `constants/figures.cppm`, the module nothing imports, is one of only two
copies that agree with the test; the copy with 19 importers is wrong. Nothing
renders the indicator today (no consumer references it outside these modules),
which is why the drift is invisible.

**Fix:** this is not a deletion — it is a **consolidation with a correctness
bug inside**. Pick `design/figures.cppm` as the survivor (it is the tested one),
point the other three at it, and let the existing test guard the value. Deleting
`constants/figures.cppm` as originally suggested would discard a faithful copy
and leave the drifted one in place.

### 7. `cc.constants.tools_constants` (101 LOC) — duplicate tool-name constants

**The stated comparison was wrong: these two modules are not duplicates.** They
have different shapes and different jobs.

| module | what it actually exports |
|---|---|
| `constants/tools_constants.cppm` | **30 exported string constants** — `bash_tool_name = "Bash"`, `grep_tool_name = "Grep"`, … No function. |
| `tools/tool_display_names.cppm` | a **lookup function** `display_tool_name(id)`, built on an internal 57-entry table of string *literals*, plus exactly **two** exported constants (`BASH_TOOL_NAME`, `SCRIPT_TOOL_NAME`). |

They overlap on only two names, and those two are spelled differently
(`bash_tool_name` vs `BASH_TOOL_NAME`). `tool_display_names` is also far less
"the one imported" than it sounds: it has **one** importer,
`tools/script_tool.cppm:23`.

**Fix — delete `tools_constants`, but for a different reason than given.**
Nothing consumes it: grep for its constants outside its own file returns zero
real uses (`agent_tool_name` matches only a *function parameter* of the same
name in `utils/message_mappers.cppm:386`, which is unrelated). The live codebase
does not centralize tool names this way — each tool module declares its own
`inline constexpr std::string_view kToolName = "Edit"` (e.g.
`tools/file_edit_types.cppm:21`) and the rest uses raw literals. So the module is
a never-adopted idea, not a shadowed implementation.

### 8. `cc.utils` (69 LOC) — the aggregator everything was supposed to go through

`utils/utils.cppm` is an `export import` index for 34 utilities and **nothing
imports it**. This one is structural: because it is dead, the utilities whose
*only* importer is this module become transitively unreachable too. Sixteen
modules are in that position (`bash_parser`, `cache`, `agent_model`, `memory`,
`memdir`, `session`, `terminal`, `input_router`, `code_indexing`, `pdf`,
`sandbox`, `query_helpers`, `task_output`, `suggestions`, `swarm`,
`native_utils`).

**Fix — RESOLVED 2026-09-21: delete the aggregator. It is not the intended
pattern.**

Two checks settle it:
1. **The codebase's convention is direct imports, by a wide margin.**
   `import cc.utils.json;` appears 144×, `cc.utils.error` 65×,
   `cc.utils.bash_execution` 45×, and so on down the list. The aggregator itself
   is imported **0** times. Nobody was ever going to route through it.
2. **The 16 are not mechanically dependent on it.** Each is listed individually
   in `src/CMakeLists.txt`, so they compile whether or not the aggregator
   imports them. Deleting the aggregator does not unbuild anything.

So "point callers at it" would mean inventing a convention the tree has
consistently rejected. Delete `utils/utils.cppm`.

**The 16 classified** (2026-09-21; all confirmed to have exactly one importer —
the aggregator — and none imported anywhere else):

| module | verdict | live counterpart |
|---|---|---|
| `bash_parser` | SHADOWED | `cc.utils.bash_execution` (45 importers) for `is_dangerous_command`/`split_shell_command`; `cc.utils.shell_parser` holds the same grammar but is **test-only** reachable |
| `cache` | SHADOWED | `cc.utils.file_read_cache` (live) — and a hand-rolled LRU exists in `ui/markdown.cppm` + `plugins/marketplace.cppm`, i.e. the codebase needs LRU and has written it 3× |
| `agent_model` | SHADOWED | `app.cppm`/`repl_screen.cppm` use the `"inherit"` vocabulary; `model_aliases` resolves live |
| `memory` | SHADOWED | `cc.memdir.paths` (`MemoryType`); `AutoMem` is consumed by the live `query_engine.cppm:971` |
| `memdir`, `session` | SHADOWED | have live sibling directories (`src/memdir/`, `src/session/`) with their own targets |
| `terminal` | SHADOWED | `cc.utils.hyperlink` (7), `cc.hooks.terminal_size`, `ink_utils.cppm` |
| `input_router` | SHADOWED | `command_registry.cppm`'s slash parsing + `parse_references` (10) + `bash_execution` |
| `code_indexing` | SHADOWED | LSP symbol tool (`cc.tools.lsp`, live-registered) — but see the caveat below |
| `pdf` | SHADOWED | `file_read_tool.cppm:216` `is_pdf_file`... and the live one does **no text extraction**. See below. |
| `sandbox` | SHADOWED | `should_use_sandbox.cppm`, `bash/impl_bash.cppm` enforced live via bash tool |
| `query_helpers` | SHADOWED | `token_budget.cppm:259` `should_compact` (80% threshold, identical) |
| `task_output` | SHADOWED | `tools/task_tool.cppm:521` `TaskOutputTool` — live |
| `suggestions` | SHADOWED | live autocomplete pipeline: `ui/autocomplete_sources.cppm`, `app_autocomplete.cpp`, `fuzzy_rank_nucleo` |
| `swarm` | SHADOWED | `cc.utils.swarm_backends` (17), `swarm_helpers` (6) — constants verbatim-identical |
| `native_utils` | **AMBIGUOUS** | no confirmed live home; `cc.utils.native_installer` also has 0 importers, so it does not "shadow" it. The DXT half has zero references tree-wide and the installer half is itself a stub. |

**Read this before deleting the whole table** — four of the SHADOWED verdicts
are the *weaker-live-implementation* pattern this file documents elsewhere (§6,
§9-10), not clean duplicates:

- `pdf` — the live `read_pdf` is a stub that base64s the file and reports a byte
  count; it extracts **no text**. The dead module implements real extraction.
- `task_output` — the live path buffers in memory with **no disk spill**
  (8 MB cap, then truncation); the dead module has `DiskTaskOutput`/`spill_to_disk`.
- `agent_model` — the live `resolve_agent_model` hardcodes per-alias model IDs;
  the dead one calls `resolve_alias`. The collapsing behaviour may be lost.
- `code_indexing` — the live counterpart is an LSP client requiring an installed
  server. The dead one is a zero-dependency regex index. If the intent was
  always-available indexing, that is a capability loss, not a duplicate.

So the honest instruction is: **delete the aggregator** (unambiguous), then treat
the 16 as a batch to review, deleting the clean duplicates and raising the four
above as consolidation decisions. Deleting them in one sweep would silently
ratify four capability losses.

**Correction to one datum a reviewer would otherwise trust:** `cc.utils.shell_parser`
is *not* on a live production path. The earlier classification said
`utils/permissions.cppm:130` imports it — true, but `cc.utils.permissions` is
itself imported **only** by this dead aggregator (`utils.cppm:63`) and
`tests/utils/permissions_test.cpp`. So `shell_parser` is test-reachable, not
live; the genuinely live counterpart for `bash_parser`'s capabilities is
`cc.utils.bash_execution` (45 importers). Three other thin chains verified:
`token_budget` is imported only by `tests/test_utils.cpp:27`, and `ink_utils`
by exactly one live file (`repl_screen.cppm:104`).

### 9-10. `cc.services.plugins.cli_commands` and `cc.vim.vim_commands` — one shadowed, one not

Both were filed as "a class whose name is registered from a *different*
module", and both do declare a type whose name is also declared and registered
elsewhere. But that shared symptom hides **opposite** conclusions — one is a
shadowed duplicate to delete, the other is a better implementation that is
merely unreachable. Read the per-module notes below before acting.

Originally recorded as:

- `command_registry_init_d.cpp:25` registers `PluginCommand`, importing
  `cc.commands.plugin_cmd`.
- `command_registry_init_c.cpp:40` registers `VimCommand`, importing
  `cc.commands.vim`. The two `VimCommand` types are structurally incompatible
  (`{name, handler, description}` vs a slash-command class with
  `definition()`/`validate()`), which is how we know which one is bound.

Both have **zero** importers. Verified 2026-09-21 — see the revised fix below.

**Fix (revised 2026-09-21): the two halves have opposite verdicts.**

- **`cc.services.plugins.cli_commands` — delete. DELETED 2026-09-21.** The
  original note called it a shadowed `struct PluginCommand`; the struct is real
  (`:32`) but incidental — the module's actual body is a
  `handle_plugin_command` **superseded stub**, and its own comments say so:
  `list` delegates to the marketplace backend while `install`/`uninstall`/
  `update` return an explicit "superseded, use `cc.utils.plugin_manager` via
  `commands/plugin/plugin_manage`" error rather than faking success. Never
  called anywhere. The named replacement `commands/plugin/plugin_manage` exists
  as a 10-file subsystem, and `get_all_available_plugins` is live in
  `utils/plugin_marketplace.cppm:142`, so no capability is lost. The only other
  `PluginCommand` is an unrelated data struct in `utils/plugin_loader.cppm:301`.
- **`cc.vim.vim_commands` — do NOT simply delete.** The original note called it
  a shadowed duplicate of `commands/vim.cppm`'s `VimCommand` class. That is
  true of the *name* and false of the *module*: the file is a complete 180-line
  ex-mode registry (`:w` `:q` `:wq` `:set` `:map` `:help` `:noh` `:number`)
  with a working `execute_ex_command` that parses `!` force variants. The
  `struct VimCommand` at line 14 is just its entry type.

  It is unreachable because the live vim hook does not use it. `hooks/vim_input.cppm`
  carries its **own local** `execute_ex_command` (line 420) and calls *that* at
  line 398 — and the live one is a near-stub that handles only all-digit input
  (`:42` jumps to line 42) and ignores `:w`, `:q`, `:help` entirely. So the dead
  module holds the *better* implementation. This is entry 1's pattern again: two
  implementations of one behaviour, the reachable one weaker, which means the
  fix is a decision (adopt the registry, or drop ex-mode) rather than a deletion.

## Sibling gap

### 11. The `cc.ui.design` component layer (5 modules, 500 LOC)

The design system is half-live. Tokens and theme are imported heavily, the
component layer is not:

    LIVE      cc.ui.design.tokens    34 importers
              cc.ui.design.theme     31
              cc.ui.design.figures   19
              cc.ui.design.primitives 7
              cc.ui.design.themed_box / themed_text  2 each
    DEAD      cc.ui.design.dialog, .divider, .list_item, .progress_bar, .tabs   0

A design system with a live token layer and a dead component layer is a wiring
gap, not a removal — the primitives were presumably written to be used.

**Verified 2026-09-21, and the gap is larger than stated — the live UI does not
merely ignore these five, it reimplements them locally.** Counted:

| capability | dead design module | local live reimplementations |
|---|---|---|
| progress bar | `design/progress_bar.cppm` (113) | **five** — `ui/components.cppm:287`, `permissions/permission_batch_panel.cppm:212`, `dialogs/wizard_dialog.cppm:262`, `tasks/task_components.cppm:297` |
| divider | `design/divider.cppm` (63) | **three** — `design/component_primitives.cppm:127`, `permissions/permissions_components.cppm:295`, `dialogs/dialog_frame.cppm:273` |
| tabs | `design/tabs.cppm` (102) | `components/tag_tabs.cppm:28`, `agents/agent_editor.cppm:247` |

(Import counts re-checked: `tokens` 34, `theme` 31, `figures` 18, `primitives`
7, `themed_box`/`themed_text` 2 each; the five dead ones 0 each. Note `figures`
is 18, not the 19 in the original table — 19 is `ui.components.figures`, a
different module.)

So the decision is sharper than "adopt them or not": the UI has **already** made
this choice, differently, four to five times over. The honest options are (a)
consolidate every local copy onto the design-system version, which is a real
refactor with visible-risk to existing layouts, or (b) delete all five and accept
that this codebase styles its components locally. Option (b) is cheaper and
matches what the code does today; option (a) is what the design system was built
for. Either is defensible — but leaving five dead modules beside five live
near-duplicates is the one outcome that helps nobody.

**`design/dialog.cppm` resolved (134 LOC): not a third renderer path — it cannot
be adopted.** Checked as the original note asked. It is a `std::cout` box-drawing
console prompt: it formats a `╭─╮` frame with `ostringstream`, prints it, and
returns `DialogResult{default_button.value_or(0), false}` **without reading any
input** (`show_dialog`, `:35-77`). It never touches FTXUI, so it cannot render in
this UI and is not in competition with the live `dialogs/` subsystem. It is a
console-mode prototype from before the interface was FTXUI. Delete it with the
other four if the decision is (b); if the decision is (a), exclude it — there is
nothing here to consolidate *onto*.

## Related, but not bugs

These were in the same sweep and are recorded here so the reasoning is not lost.

**`cc.hooks.notifs` — only two modules, and the directory was not a cluster.**
The dead-module report described `notifs` as having eight siblings with seven
dead. That is wrong: `src/hooks/notifs/` contains exactly two files.
`remaining_notifs.cppm` (29 KB) is live — 4 importers — and
`rate_limit_warning.cppm` has none. The latter is a plain deletion candidate
(one notification that nothing emits); it was **not** deleted in this sweep
because it sits under a "mixed verdict" flag.

**Re-verified 2026-09-21 — counts confirmed, with a naming trap worth recording.**
`remaining_notifs` really does have 4 importers
(`tools/mcp_tool.cppm:33`, `tasks/in_process_teammate_task.cppm:16`,
`tests/test_fix_notifs.cpp`, `tests/test_hooks.cpp`) and `rate_limit_warning`
really has 0. But note **the two files in this one directory declare modules in
two different namespaces**:

    hooks/notifs/remaining_notifs.cppm  ->  cc.hooks.remaining_notifs   (drops "notifs")
    hooks/notifs/rate_limit_warning.cppm -> cc.hooks.notifs.rate_limit_warning

So grepping `import cc.hooks.notifs.remaining_notifs;` — the name the path
suggests — returns **0** and would have "confirmed" this module dead. This is
the module-name/path decoupling CLAUDE.md describes, and it is the single
easiest way to produce a false dead-module verdict in this tree: always resolve
the declared name (`grep -rn "^export module" <file>`) before counting.

**`src/buddy/` is a self-contained island, not a directory of dead modules.**
Only `buddy_sprites` (626 LOC of ASCII art) was deleted. What remains:

    cc.buddy.buddy_types        2 importers   } both from within this directory
    cc.buddy.buddy_companion    1 importer    } (the only live path is the one
    cc.buddy.buddy_prompt       0 importers   }  below, and it goes nowhere)
    cc.buddy.buddy_hooks        0 importers   }  <- omitted from the original note

The imports form a closed loop — `buddy_prompt` imports `buddy_companion` and
`buddy_types`, and `buddy_companion` imports `buddy_types` — with nothing
outside the directory importing any of them. So the whole island is
unreachable, but each module inside it has an "importer", which is why a
per-module importer count alone does not see it.

**Verified 2026-09-21, with one correction and one addition.**

*Addition:* there is a **fourth** file, `buddy_hooks.cppm` (also 0 importers).
It holds a date-gated easter egg — `is_buddy_teaser_window(year, month, day)`,
`is_buddy_live(year, month)`, `find_buddy_trigger_positions(text)` — and none of
the three is called from anywhere. Note the file list in the original note was
incomplete, which is why "only two modules" (§ the `notifs` entry) was checked.

*Correction — the island is NOT closed, and "nothing outside imports it" is too
strong.* The claim is true of **imports**, but the buddy concept is referenced
from four live modules, and one of them is a live consumer:

| file | what it is |
|---|---|
| `ui/prompt/combined_highlights.cppm:83-85,557-574` | **live** — a `buddy_enabled` flag that applies `/\/buddy\b/` rainbow-shimmer highlighting, reimplementing the TS `findBuddyTriggerPositions` inline rather than calling `buddy_hooks`'s version |
| `config/settings.cppm:232` | a `BUDDY_ENABLED` ("buddyEnabled") setting key — **declared, never read** |
| `ui/rendering/fullscreen_layout.cppm:190` | a `bottom_float` companion-bubble slot, self-documented "can be a stub" |
| `utils/text_highlighting.cppm:225` | a `RainbowShimmer` style ordinal comment mentioning buddy |

The `app.cppm:1812` trace the original note cited does not exist — grep `buddy`
in `ui/app.cppm` returns nothing. The real surviving trace of intent is the
highlighting path, not a comment.

**Decision needed — but it is narrower than "does the companion ship?"** The
`/buddy` keyword highlighting is live *today* and independent of these modules
(it reimplements the trigger scan). So the question is only whether the
companion *module* set gets wired to the `bottom_float` slot and the
`buddyEnabled` key. If yes, `buddy_hooks` is the piece to wire (nothing uses
its date gate); if no, all four go, and the highlighting stays. Left in place
pending that call rather than deleted on a per-module basis that would have
looked safe and been wrong.
