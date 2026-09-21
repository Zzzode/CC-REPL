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

Each entry states the evidence and a suggested fix. None is fixed yet. Verify
the evidence before acting — line numbers drift.

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

**Fix:** decide per skill whether the submodule's manifest is what should ship.
If yes, call it and delete the inline copy; if no, delete the submodule. The
current state ships the inline copy while carrying 693 LOC of the intended one.

### 6. `cc.constants.figures` (62 LOC) — one glyph set, three copies

`LIGHTNING_BOLT` is declared in three places:

| file | form | imported |
|---|---|---:|
| `constants/constants.cppm:196` (`namespace figures`, `k`-prefixed) | `kLightningBolt = "↯"` | 6× |
| `constants/figures.cppm:24` | `LIGHTNING_BOLT = "↯"` (escape) | **0×** |
| `ui/components/figures.cppm:15` | `LIGHTNING_BOLT = "↯"` (literal) | 19× |

**Fix:** keep one. `constants/figures.cppm` is the one nothing imports, so it is
the one to delete — but the escape form is the only substantive difference from
the live copies, so check whether it was deliberate (e.g. for a compiler or
platform where the literal did not survive) before removing it.

### 7. `cc.constants.tools_constants` (101 LOC) — duplicate tool-name constants

`tools/tool_display_names.cppm` declares the same `BASH_TOOL_NAME` /
`AGENT_TOOL_NAME` constants and is the one imported.

**Fix:** delete, unless it holds names the live module lacks.

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

### 9-10. `cc.services.plugins.cli_commands` and `cc.vim.vim_commands` — shadowed names

Both declare a class whose name is registered from a *different* module:

- `command_registry_init_d.cpp:25` registers `PluginCommand`, importing
  `cc.commands.plugin_cmd`.
- `command_registry_init_c.cpp:40` registers `VimCommand`, importing
  `cc.commands.vim`. The two `VimCommand` types are structurally incompatible
  (`{name, handler, description}` vs a slash-command class with
  `definition()`/`validate()`), which is how we know which one is bound.

**Fix:** delete both. The registration binds to the imported class, so these are
inert — but delete only after confirming no third definition appears.

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
