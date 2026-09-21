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

**Fix:** the decision is whether this aggregator is wanted at all. If yes, point
callers at it (and the 16 become reachable). If no, delete it **and** decide the
16 individually — they are currently excluded from the dead list only because
this module nominally reaches them. This is the one flag whose resolution
changes whether ~16 other modules live or die, so resolve it first.

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

**Fix:** decide whether these five are the component layer going forward. If
yes, the UI should adopt them. If no, delete — but note `design/dialog.cppm`
sits next to a live `dialogs/` subsystem, so check it is not a third renderer
path.

## Related, but not bugs

These were in the same sweep and are recorded here so the reasoning is not lost.

**`cc.hooks.notifs` — only two modules, and the directory was not a cluster.**
The dead-module report described `notifs` as having eight siblings with seven
dead. That is wrong: `src/hooks/notifs/` contains exactly two files.
`remaining_notifs.cppm` (29 KB) is live — 4 importers — and
`rate_limit_warning.cppm` has none. The latter is a plain deletion candidate
(one notification that nothing emits); it was **not** deleted in this sweep
because it sits under a "mixed verdict" flag.

**`src/buddy/` is a self-contained island, not a directory of dead modules.**
Only `buddy_sprites` (626 LOC of ASCII art) was deleted. What remains:

    cc.buddy.buddy_types        2 importers   } both from within this directory
    cc.buddy.buddy_companion    1 importer    } (the only live path is the one
    cc.buddy.buddy_prompt       0 importers   }  below, and it goes nowhere)

The imports form a closed loop — `buddy_prompt` imports `buddy_companion` and
`buddy_types`, and `buddy_companion` imports `buddy_types` — with nothing
outside the directory importing any of them. So the whole island is
unreachable, but each module inside it has an "importer", which is why a
per-module importer count alone does not see it.

**Decision needed:** is the companion feature intended to ship? If not, the
remaining three go together (and `ui/app.cppm:1812`, which alludes to
`buddy_prompt` in a comment, is the only trace of intent). If yes, something
must import it — nothing does today. Left in place pending that call rather
than deleted on a per-module basis that would have looked safe and been wrong.
