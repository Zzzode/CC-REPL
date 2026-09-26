# RFC 0002 — UI9 edge inventory (measured 2026-09-26)

Read-only measurement. Built from the `cc.ui.*` module import graph
(globbed from `src/**/*.cppm`), mapped each module to its 2nd-level area
(`cc.ui.<area>.<leaf>`), and ran Tarjan over the area graph.

- **218** distinct `cc.ui.*` named modules (219 interface declarations if
  the single `:private`/`:impl` partition is counted); **12** second-level
  areas total (9 in the SCC + `app`, `tools`, `visual`).
- **One SCC of 9 areas**:
  `cc.ui.chrome, cc.ui.dialogs, cc.ui.features, cc.ui.foundation,
  cc.ui.messages, cc.ui.permissions, cc.ui.prompt, cc.ui.screens,
  cc.ui.widgets`.
- Already singleton areas: `cc.ui.app`, `cc.ui.tools`, `cc.ui.visual`.
- **34** same-SCC area back-edge *directions* when module-impl `.cpp` units
  are included (the F0 lint globs `*.cppm + *.cpp`); **32** from interfaces
  alone (the extra 2 are `screens→permissions` from
  `repl_screen_dialog_panels.cpp` and `screens→widgets` from
  `repl_screen_prompt_render.cpp`, both downward after the cut). These
  collapse from 172 all-TU module-edge pairs (132 cppm-only).
- Module-level graph is a DAG (zero module cycles); the cycle exists only
  when modules are grouped by responsibility directory — exactly the
  RFC 0001 §3.1 / CLAUDE.md UI9 condition.
- Exhaustive 9! feedback-arc-set search: **minimum cut = 7 area directions /
  10 module edges** (RFC rows {1,2,3,4,6,7,8}); rows 5 and 9 are optional
  decoupling, not required for acyclicity.

## Area→area edges inside the SCC (32)

| # | From → To | Cut row (RFC §Detailed design) |
|---|---|---|
| 1 | chrome → foundation | (becomes legal downward once foundation/chrome order set; check residual after F1) |
| 2 | dialogs → features | 5/6 (registry inversion) |
| 3 | dialogs → foundation | legal after foundation sinks to the bottom |
| 4 | dialogs → permissions | 7 (RiskLevel sink) |
| 5 | dialogs → screens | 4 (doctor renderer registration) |
| 6 | dialogs → widgets | 3/9 (umbrella + type sinks) |
| 7 | features → chrome | chrome leaf ordering |
| 8 | features → dialogs | 6 (registration-driven wizard/trust) |
| 9 | features → foundation | downward after sinks |
| 10 | features → widgets | 9 |
| 11 | foundation → chrome | **1 (delete dead import, logo.cppm:11)** |
| 12 | foundation → widgets | **2 (custom_select sink, design_extras.cppm:26)** |
| 13 | messages → chrome | chrome ordering |
| 14 | messages → dialogs | **7 (RiskLevel/trust_utils sink)** |
| 15 | messages → foundation | downward after sinks |
| 16 | messages → widgets | 9 |
| 17 | permissions → chrome | chrome ordering |
| 18 | permissions → dialogs | **7** |
| 19 | permissions → foundation | downward |
| 20 | permissions → widgets | 9 |
| 21 | prompt → chrome | chrome ordering |
| 22 | prompt → foundation | downward |
| 23 | prompt → messages | **8 (shared footer/message helper sink)** |
| 24 | screens → chrome | chrome ordering |
| 25 | screens → dialogs | 4/6 |
| 26 | screens → features | features ordered below screens |
| 27 | screens → foundation | downward |
| 28 | screens → messages | messages below screens |
| 29 | screens → prompt | **9 cross-cutting (PastePreview/placeholder sink)** |
| 30 | widgets → dialogs | **3 (all_components.cppm:19 umbrella export import)** |
| 31 | widgets → foundation | downward |
| 32 | widgets → prompt | **9 (text_input.cppm:30 PastePreview; text_input_widget placeholder)** |

Cross-area edges OUTSIDE the SCC that constrain the final ordering:
- messages → `cc.ui.tools.registry` / `cc.ui.tools.generic` (2): rank
  `cc.ui.tools` below messages (the registry is a zero-import leaf today).
- widgets → `cc.ui.visual.markdown` (1): order `cc.ui.visual` as a pure
  leaf below widgets.

## Verified representative source sites (2026-09-26)

| Edge | File:line | Import |
|---|---|---|
| foundation→chrome | `ui/foundation/logo.cppm:11` | `import cc.ui.chrome.layout;` (no body reference — delete candidate) |
| foundation→widgets | `ui/foundation/design_extras.cppm:26` | `import cc.ui.widgets.custom_select;` (used `:143`) |
| widgets→dialogs | `ui/widgets/all_components.cppm:19` | `export import cc.ui.dialogs.feature_dialogs;` (umbrella) |
| dialogs→screens | `ui/dialogs/dialog_default_renderers.cppm:40` | `import cc.ui.screens.doctor_screen;` |
| features→dialogs | `ui/features/agents/agent_wizard.cppm:49` | `import cc.ui.dialogs.wizard_dialog;` |
| messages→dialogs | `ui/messages/messages_interactions.cppm:52` | `import cc.ui.dialogs.trust_utils;` // RiskLevel for bulk delete |
| widgets→prompt | `ui/widgets/text_input.cppm:30` | `import cc.ui.prompt.prompt_paste_handler;` // PastePreview (GAP 1) |

## Cut target

The discovery cut analysis found a global minimum sever set of **10**
module edges that makes all 9 areas singletons, vs 13–17 under the order
pre-declared in RFC 0001 §3.1. The ten RFC rows map to those cuts; the
other 22 area directions then point downward in the resulting total
order. The exact module-edge minimum must be recomputed by the F0 lint
after each phase (moving types changes the graph).

## Reproduction

```python
# glob src/**/*.cppm; parse `export module` and `import cc.<x>;`;
# area(m) = '.'.join(m.split('.')[:3]) for cc.ui.*; Tarjan on area graph.
```

(The exact one-off is in the 2026-09-26 discovery workflow; F0 promotes
this computation into `tools/arch/graph_check.py --target-ui9` so the
inventory is reproducible in CI rather than session-only.)
