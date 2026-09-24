# OQ-3 — Phase B cut design (Core8 SCC)

Attached evidence and the decided per-edge design. Produced 2026-09-24 from
the actual import graph and symbol usage, not edge counts alone.

## 1. Module-name vs file-name reality in the agent cluster

Confusing but important (module names are decoupled from paths):

| File | Module name | Upward imports |
|---|---|---|
| `src/tools/agent_run.cppm` | `cc.tools.agent.run` | skills.skill, services.api.{client,streaming,bootstrap}, services.mcp.types |
| `src/tools/agent_resume.cppm` | `cc.tools.agent.resume` | same |
| `src/tools/agent_fork.cppm` | `cc.tools.agent.fork` | same |
| `src/tools/agent_tool.cppm` | **`cc.tools.agent`** (the facade; 1422 LOC, `class AgentTool`) | same |
| `src/tools/agent_sub_utils.cppm` | `cc.tools.agent.utils` | same |
| `agent_runtime.cppm` | `cc.tools.agent_runtime` | none upward |
| constants / display / color_manager / memory / memory_snapshot / types | `cc.tools.agent_*` / `cc.tools.agent.*` | none upward |

Encapsulation finding: `agent.run / .resume / .fork` have **zero external
importers**; they are reached only through the `cc.tools.agent` facade
(`AgentTool`), which is imported by 6 tools/team modules + tests. Everything
outside the cluster talks to `agent_runtime` (store/definitions) and
`cc.tools.agent` (the spawning tool). The five upward-importing modules are a
leaf-facing subtree behind one facade — ideal to lift wholesale.

`cc.services.api.*` and `cc.skills.skill` are clean upper layers (they never
import back into tools), so lifting the subtree above them introduces no new
cycle.

## 2. Decision: promote the 5-module subtree to `cc.orchestration.agent`

New target `cc_orchestration` (static library), above services and skills:

| New module (path TBD, names can stay or move — importers only see the facade) | From |
|---|---|
| `cc.orchestration.agent` (facade, formerly `cc.tools.agent`, keeps `class AgentTool`) | agent_tool.cppm |
| `cc.orchestration.agent.run / .resume / .fork / .utils` | agent_{run,resume,fork,sub_utils}.cppm |

Stays in `cc_tools` (no upward edges): `agent_runtime`, `agent_constants`,
`agent_display`, `agent_color_manager`, `agent_memory`,
`agent_memory_snapshot`, `agent_types` — these are the store, display
projection and memory primitives that the rest of tools legitimately shares.

Two consequences to design during implementation:

1. The lifted modules currently read 7 sibling tool modules
   (runtime/constants/memory/memory_snapshot/color_manager/display/utils).
   They may keep importing those **downward** (orchestration is above tools)
   — this is legal, but to keep orchestration testable the store access it
   needs is the existing `agent_runtime` surface (records/definitions/env
   helpers); no new port is required for this direction.
2. `class AgentTool` implements the tool contract (`ToolInput`/`ToolResult`,
   defined in `cc.tools.tool`). A class in orchestration implementing an
   interface in tools is fine (downward interface use); the registry
   registers it at the composition root. No inversion needed here.

External importers change one import (`cc.tools.agent` ->
`cc.orchestration.agent`) in: runtime_team_shared, spawn_multi_agent,
team_delete, team_create, runtime_registry, tests; plus color.cppm for
`agent.utils` (moves to orchestration only if it truly belongs to the spawn
path — verify at implementation; if color only needs one helper, sink that
helper to tools instead and keep `agent.utils` split).

## 3. Per-edge cut table

| # | Edge | Measured usage | Decision | Port / action |
|---|---|---|---|---|
| 1 | `hooks.{turn_diffs,prompt_suggestion,assistant_history} -> state.app_state` (3 of the 9 edges) | **Dead imports — zero `AppState` symbols named** (verified: removed, full debug build green 2026-09-24) | Delete imports | none; -3 edges for free, no code change |
| 2 | `hooks.voice_hooks -> services.voice.*` (5 edges) | `VoiceRecorder`/`VoicePlayback` own types (1257 LOC), use `AudioCapture`, `VoiceStreamSTTService`, `get_voice_keyterms` | Invert: hooks keeps the contract | Define `cc.hooks.voice.port` holding the abstract `AudioCapturePort`/`SttPort` interfaces + keyterm provider signature that VoiceRecorder/Playback call. Implementations (`AudioCapture`, `VoiceStreamSTTService`) stay in services and are constructed/injected in `AppAdapter`'s composition root. |
| 3 | `services.streaming_executor -> tools.tool` (1) | names only `ToolInput` / `ToolResult` (6 + 11 uses) | Sink the DTOs | `ToolInput`/`ToolResult` are plain data; move (or re-export) them from `cc.types.types` (the canonical leaf, already holds ToolResultContentItem). streaming_executor then imports types, not tools. `cc.tools.tool` re-exports for compatibility. |
| 4 | `utils.ide_integration -> services.mcp.{client,types}` (2) | `McpClient`, `McpClient::Config`, `ContentItem`, `ToolCallRequest` | Relocate | Move the module to `cc.services.ide_integration` (misfiled; it is an MCP client consumer). Zero abstraction. |
| 5 | `skills.bundled.debug -> tools.tool` (1) | uses `ToolRegistry` callbacks for a verify/fix loop | Invert to a callback | Change the debug skill's entry to accept a pre-injected `std::function` / narrow callback interface defined in skills (the caller at the composition root binds ToolRegistry). If the only use is test-only, move it to a test seam instead — decide by reading the call site during implementation. |
| 6 | `state.teammate_view_helpers -> task_types` (1) | team view projection references task DTO | Sink or merge | `cc.task_types` is already a low leaf (rank 5); move the referenced DTO down to types/task_types so state imports downward, or fold the helper into task_types. Mechanical. |
| 7 | `tools.mcp_tool -> hooks.remaining_notifs` (1) | one call `hooks::notifs::inject_mcp_connectivity_from_manager(*manager_)` in a status path | Invert via callback | mcp_tool exposes a setter `set_connectivity_provider(std::function<...>)`; hooks (above tools) registers the provider at composition root. tools never imports hooks. |
| 8 | `tools.mcp_tool -> config.config` (1) | reads `McpServerConfig` / `McpOAuthConfig` | Unify, then sink config DTOs | Two `McpServerConfig` structs exist (config.cppm: full file/url/oauth settings; services/mcp/types: protocol subset). Consolidate into one definition in the lower layer (types or a new `cc.config.mcp_types` leaf with no utils dependency) re-exported upward; tools imports the leaf type. config keeps only loading/validation. |
| 9 | agent subtree 36 edges -> services.api / skills | (section 2) | Promote | new `cc_orchestration` target. |
| – | `config -> utils.json`, `state -> utils` | downward | keep | none |

## 4. Lint semantics (so the structural exception is honest)

`graph_check.py` allows an upward edge only when the destination module's
name is under an explicitly declared contract package:
`cc.hooks.*.port` and (for config DTOs) `cc.types` / `cc.config.*_types`
leaves. Anything else pointing up is a failure. This is checked on the
directory graph plus an allowlist file (`tools/arch/port_allowlist.txt`)
listing each contract module — no free-form per-edge whitelist.

## 5. Composition-root injection sites (verified existing, no new wiring hub)

- Voice ports, MCP connectivity provider, skills debug callback: registered
  in `AppAdapter`'s constructor (already the single composition root;
  services/hooks are constructed there today).
- `AgentTool` is already registered into the tool registry there, so the
  orchestration facade needs no new bootstrap.

## 6. Predicted post-Phase-B graph

Core8 (config/hooks/services/skills/state/task_types/tools/utils) becomes 8
singleton SCCs. `cc_utils`, `cc_tools`, `cc_services` (and the new
`cc_orchestration`, `cc_skills` if split) link with strictly downward
dependencies. The prediction is confirmed by running graph_check on the
module graph AFTER the port modules exist but BEFORE bodies move — if the
SCC is still present, code does not start.

## 7. Implementation order (each independently green)

1. Delete 3 dead hook state imports (edge family 1) — immediate.
2. Sink ToolInput/ToolResult; sink/unify MCP config DTOs (families 3, 8).
3. Relocate ide_integration (4); teammate helper (6).
4. Define hooks voice port + mcp connectivity setter + skills callback;
   inject at composition root (2, 5, 7) — behavior-preserving.
5. Lift the 5-module agent subtree to cc_orchestration; retarget 7
   importers (9).
6. Add graph_check structural rule + port allowlist; require singleton SCCs.

Every step: dual preset -Werror + serial ctest, with the ctest total
reconciled (no test deletions expected).
