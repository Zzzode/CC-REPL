# OQ-3 — Phase B cut design (Core8 SCC), REV 3

Revised twice under adversarial Tarjan review.
- REV 2 fixed the REV 1 hooks↔services cycle and the self-contradictory
  layer order, but its own simulation over only 8 areas missed a
  tools↔orchestration SCC exposed once orchestration is a 9th node, and
  under-counted family 12 (1 vs the 5 edges it actually removes).
- REV 3 (this revision) adds family 14 to dispose of the reverse
  tools→orchestration edges, corrects every count to the re-derived
  total of **52**, and states the completion invariant over **9 nodes**.

## 0. Why REV 2 still failed (verified in code)

1. Moving the agent facade + Mcp/Lsp tools to orchestration creates edges
   back into tools because orchestration legitimately imports the pure
   tool layer (67 edges) AND four modules that stay in `cc_tools` import
   the moved facade today:
   `team_create`, `team_delete`, `spawn_multi_agent`,
   `runtime_team_shared` (`import cc.tools.agent;`). They use only
   `AgentConfig`, `AgentLivePermissionCheck(Fn)` (9+4+2 uses) and
   `cleanup_agent_worktree`, and have **zero** services/skills imports —
   they are NOT service-backed, so family 12 does not move them. Net:
   tools -> orchestration + orchestration -> tools = a 2-node SCC; cc_tools
   would not link independently (G4) and the E0 total-rank lint would fail
   on the RFC's own moves.
2. Family 12 removes **5** upward edges (lsp -> LSPServerManager 1;
   mcp_tool -> services.mcp.{config,connection_manager,auth,types} 4),
   not 1.
3. Re-derived upward pairs among the 8 areas (live graph):
   hooks→services 5, hooks→state 4, services→tools 1, utils→services 2,
   skills→tools 1, state→task_types 1, tools→hooks 1, tools→config 1,
   tools→services 27, tools→skills 9 = **52**. (Downward pairs such as
   tools→utils 129, services→utils 88, skills→utils 13, config→utils 1 (cc.config.config -> cc.utils.json),
   hooks→utils 3, state→utils 3, skills→config 3, task_types→utils 1 are
   legal and not counted.)
4. There are **6** `struct McpServerConfig` definitions (not 7):
   config/config.cppm, services/mcp/types.cppm, commands/mcp/add_command,
   cli/handlers/mcp_handler, entrypoints/mcp_entrypoint,
   ui/features/mcp/mcp_settings_panel.
5. `ToolInput::has_field` calls `cc::utils::json::parse` (tool.cppm:62):
   sinking ToolInput/ToolResult into cc.types requires splitting the DTO
   (data goes down; the json helper stays in tools) — cc.types cannot
   import utils.
6. The facade has 5 src importers; `agent.utils` is also imported by
   `cc.commands.color` (commands sits above orchestration, harmless).

## 0b. Family 14 — break the tools↔orchestration SCC (new in REV 3)

The four tools (team_create/team_delete/spawn_multi_agent/
runtime_team_shared) need pure types and one worktree helper, not the
spawning facade:

| Symbol used by the 4 tools | Home today | Disposition |
|---|---|---|
| `AgentConfig` (plain struct: model, allowed/denied tools, ids) | agent_sub_utils.cppm:91 | sink to `agent_types` (stays in tools, no services deps) |
| `AgentLivePermissionCheck` + `AgentLivePermissionCheckFn` | agent_sub_utils.cppm:102-108 | sink to `agent_types` |
| `cleanup_agent_worktree(path)` | agent_sub_utils | keep a tools-level declaration (it is filesystem cleanup, no services); the orchestration facade calls the same tools helper — downward legal |
| `AgentTool` itself (2 uses: spawn_multi_agent, runtime_team_shared) | agent_tool facade | these two call sites are spawning orchestration: move `spawn_multi_agent` to `cc.orchestration.agent` (the sole
  AgentTool constructor among the four); `runtime_team_shared` may stay in
  tools (it uses only the retained cleanup helper); team_create/team_delete
  stay in tools (sunk types only). |

Concretely:
- Sink `AgentConfig` / `AgentLivePermissionCheck(Fn)` into
  `cc.tools.agent_types` (already a pure-data module the four import).
- `spawn_multi_agent` moves to orchestration (it constructs `AgentTool`).
  `runtime_team_shared` uses only the tools-retained `cleanup_agent_worktree`
  and other tools/utils modules, so it MAY stay in tools; the 9-singleton
  result holds either way (decided at implementation).
- `team_create` / `team_delete` remain in tools and now import only
  agent_types + the cleanup helper — no orchestration edge.
- `runtime_registry` (3 reverse edges to the moved facade/mcp/lsp) is split
  per §3: registry mechanics + contract stay in tools; concrete tool
  construction (AgentTool/McpTool/LspTool registration) moves to an
  orchestration composition function.

Result including orchestration as a node: the 9-node directed graph
(leaves < hooks < skills < services < tools < orchestration < query/commands
< ui < server/cli) has **9 singleton SCCs**. That 9-of-9 result, not the
8-area count, is the Phase B completion invariant.

## 0c. Why REV 1 originally failed (kept for history) (the review findings, all verified in code)

1. A port cut in one direction is an edge in the other. Family 2 of REV 1
   put `AudioCapturePort` in hooks with the implementation in services —
   that ADDS `services -> hooks`. Meanwhile `hooks.notifs.remaining_notifs`
   still imports `cc.services.mcp.{types,connection_manager}`
   (`src/hooks/notifs/remaining_notifs.cppm:72-73`, bridge takes
   `McpConnectionManager&` at :507, names `ConnectionStatus` at :489).
   hooks -> services + services -> hooks = a 2-node SCC.
2. The hook dead-import count is 4, not 3:
   `src/hooks/background_task_navigation.cppm:9` is an unused
   `cc.state.app_state` import (zero `AppState` symbols), same shape as the
   other three.
3. voice has exactly **3** upward imports (voice_hooks.cppm:46-48), not 5.
4. The 5 lifted agent modules contribute **25** direct upward edges
   (5 modules x 5), not 36. The other **11** of the 36 come from modules
   REV 1 left in `cc_tools`, which therefore still point up:
   - `cc.tools.mcp` (mcp_tool.cppm) -> services.mcp.{config,connection_manager,auth,types} (4)
   - `cc.tools.lsp` (lsp_tool.cppm) -> services.lsp.LSPServerManager (1; uses `file_uri_for_path`)
   - file_read_tool -> services.image (`ImageService::get_info/to_base64/from_base64`) + skills.skill (2)
   - file_edit_tool -> skills.skill (`notify_file_access`) (1)
   - file_write_tool -> skills.skill (`notify_file_access`) (1)
   - runtime_registry -> services.image + skills.skill (`SkillLoader`) (2)
5. Facade `cc.tools.agent` has **5** src importers (team_create, team_delete,
   runtime_team_shared, spawn_multi_agent, runtime_registry), not 6;
   tests/test_tools.cpp imports the facade AND agent.utils.
6. There are **7** `struct McpServerConfig` definitions, not 2 (config,
   services/mcp/types, commands/mcp/add_command, cli/handlers/mcp_handler,
   entrypoints/mcp_entrypoint, ui/features/mcp/mcp_settings_panel — plus the
   earlier count). MCP server config is user-persisted data.

## 1. The single consistent target layering

The REV 1 diagram ("tools above services above hooks") is the source of the
contradiction: it assumed tools is a pure domain layer, but mcp/lsp/file
tools are *service-backed* — they are orchestration over concrete services.
The consistent layering is:

```
types / constants / config-types / *.port contracts        (leaves)
        ^
utils / state / task_types / vim
        ^
hooks  — event + callback CONTRACTS and pure hook LOGIC only.
         A hook module never imports a concrete service/state implementation.
         (voice_hooks becomes pure logic over injected ports.)
        ^
skills — skill definitions/loading; notifies via a callback sink, not via
         importing tools (today skills.bundled.debug -> tools is the offender)
        ^
services — concrete API/MCP/LSP/voice/image implementations.
           services MAY implement hook/skill contracts (downward to *.port).
        ^
tools — PURE domain tools only (bash, read/write primitives, math, registry
         of tool contracts). A tool that needs a concrete service is NOT here.
        ^
orchestration — service-backed tools and multi-service flows:
                the agent run/resume/fork subtree, McpTool, LspTool,
                image-aware file_read, and the SkillLoader wiring.
        ^
query / commands / ui / server
```

Key rules that make this acyclic:

- Contracts (ports) live in the layer that DECLARES the callback; the
  implementation importing that contract is a downward edge.
- A module that imports a concrete service implementation belongs ABOVE
  services (orchestration), not in tools.
- services may depend DOWN to contracts/types/utils; never up to hooks'
  concrete logic.

The lint rank is therefore a total order; "upward" is unambiguous, and the
only allowed across-rank edges are into a module under an explicitly named
contract package (`*.port`, `*.contract`, `cc.types`, `cc.config.*_types`),
listed in `tools/arch/port_allowlist.txt`.

## 2. Complete cut families (verified counts)

| # | Edge(s) | Count | Cut |
|---|---|---:|---|
| 1 | hooks.{turn_diffs,prompt_suggestion,assistant_history,background_task_navigation} -> state.app_state | 4 | DELETE — dead imports, zero `AppState` symbols (full build verified for 3; background_task_navigation same shape, verify at execution) |
| 2 | hooks.voice_hooks -> services.voice.{voice,voice_stream_stt,keyterms} | 3 | Define `cc.hooks.voice.port` with the abstract audio-capture / STT / keyterm interfaces VoiceRecorder/VoicePlayback call. Move the concrete `AudioCapture`, `VoiceStreamSTTService`, `get_voice_keyterms` call sites behind injected port instances. services.voice IMPLEMENTS the port (a legal services->contract downward edge). voice_hooks keeps zero services imports. |
| 3 | hooks.notifs.remaining_notifs -> services.mcp.{types,connection_manager} | 2 | The bridge (`inject_mcp_connectivity_from_manager(McpConnectionManager&)`) is a composition-root helper, not hook logic. MOVE it to orchestration (the REPL wiring layer), next to where the manager is owned. Define the hook-side shape `McpConnectivityInfo` (already hook-local) and a narrow consumer that accepts plain snapshots / a `std::function` snapshot provider — no mcp types in hooks. hooks loses both imports; the moved helper importing services is legal inside orchestration. |
| 4 | services.streaming_executor -> tools.tool | 1 | Sink the two DTOs it names (`ToolInput`,`ToolResult`) into `cc.types`; **split first**: `ToolInput::has_field` calls `cc.utils::json::parse` (tool.cppm:62), so the data structs move down but that json helper stays in tools (cc.types cannot import utils). tools.tool re-exports the DTOs; streaming_executor imports types (downward). |
| 5 | utils.ide_integration -> services.mcp.{client,types} | 2 | RELOCATE module to `cc.services.ide_integration`. |
| 6 | skills.bundled.debug -> tools.tool | 1 | Invert: the verify/fix skill receives an injected callback/registry interface declared in skills; the composition root binds ToolRegistry. If test-only, move to a test seam. |
| 7 | state.teammate_view_helpers -> task_types | 1 | Sink referenced DTO to types (or merge helper into the lower module). |
| 8 | tools.mcp_tool -> hooks.remaining_notifs | 1 | setter/callback in mcp_tool (`set_connectivity_snapshot_provider`) registered by orchestration; tools never imports hooks. |
| 9 | tools.mcp_tool -> config.config (McpServerConfig/OAuth) | 1 | Consolidate the **6** McpServerConfig into ONE low `cc.config.mcp_types` leaf (pure data + parse) with no utils dependency; re-exported where needed. Persisted-settings compatibility is an explicit acceptance item (read old JSON). tools imports the leaf. |
| 10 | file_read_tool -> services.image (3 call sites), runtime_registry -> services.image | 2 | MOVE the image-aware path of file_read and the image (de)code wiring in runtime_registry up to orchestration; OR inject an `ImageCodec` port declared in tools and implemented in orchestration. Decision at implementation: port preferred (keeps the core file_read primitive in tools); runtime_registry's registry of tools legitimately lives in orchestration if it references concrete services — see §3. |
| 11 | file_{read,edit,write}_tool + runtime_registry -> skills.skill (`notify_file_access`, `SkillLoader`) | 4 | `notify_file_access` is a fire-and-forget hook: declare a sink in a low contract (`cc.skills.file_access.port` or a tools-declared `std::function` registry) that file tools call; skills registers the implementation (downward). `SkillLoader` construction in runtime_registry moves to orchestration wiring. |
| 12 | cc.tools.lsp -> services.lsp.LSPServerManager (1) + cc.tools.mcp -> services.mcp.{config,connection_manager,auth,types} (4) | 5 | MOVE McpTool/LspTool (and other service-backed tools) to `cc.orchestration.tools` — they are adapters over a concrete manager. |
| 13 | agent.run/.resume/.fork/.utils + `cc.tools.agent` facade -> services.api/skills (25) | 25 | MOVE the 5-module subtree to `cc.orchestration.agent` (REV 1 decision, unchanged). |
| – | config->utils.json, state->utils | downward | keep |

| 14 | reverse tools->orchestration exposed by the lift: team/spawn modules + runtime_registry -> moved facade/mcp/lsp (disposition in §0b: sink AgentConfig/permission types to agent_types; move spawn_multi_agent/runtime_team_shared + concrete registry wiring to orchestration; keep team_create/team_delete in tools) | 7 | type sink + selective move; breaks the tools↔orchestration SCC |
| – | config->utils.json, state->utils, services->utils, skills->utils/config, hooks->utils, task_types->utils | downward (config->utils is one unique pair: cc.config.config -> cc.utils.json) | keep |

### Test-seam note (implementable detail)

`cc.tools.team_create` / `team_delete` have zero src importers and are called
only from tests/test_tools.cpp via `register_runtime_tools`. When
runtime_registry splits, that entry point must stay reachable from tools
through injected factories, or the two wrappers move to a test seam. The
Phase B graph gate ("predicts 9 singleton SCCs before bodies move") catches
a regression either way.

Total upward edges among the 8 areas (live, re-derived): **52** = hooks→services 5 + hooks→state 4 + services→tools 1 + utils→services 2 + skills→tools 1 + state→task_types 1 + tools→hooks 1 + tools→config 1 + tools→services 27 + tools→skills 9. After families 1-14 (incl. the 7 family-14 edges and orchestration as a 9th node) the target graph is **9 singleton SCCs**.

## 3. Targets after Phase B

- `cc_tools` keeps only pure domain tools (bash, primitives, the tool
  CONTRACT types in cc.types, registry mechanics with no concrete-service
  refs). runtime_registry is split: contract/registry mechanics stay in
  tools; concrete tool/service construction moves to orchestration.
- new `cc_orchestration` static library: agent subtree, Mcp/Lsp tools,
  image-aware file reads, SkillLoader/MCP snapshot wiring.
- `cc_hooks`: contracts + pure logic, zero services/state imports.
- `cc_services`: implements voice/skills contracts (downward), no import of
  concrete hook logic.
- Graph invariant after cuts: Tarjan over the 9 layered nodes (incl. orchestration) = **9 singleton SCCs**.

## 4. Gate sequencing (fixes the A/E and B/E inversion)

Add a bootstrap milestone **E0** that lands `tools/arch/graph_check.py`
(plus the port allowlist) FIRST, before A and B graduate on it:

1. E0 — graph_check + allowlist + dead-import detection in CI.
2. Family 1 (delete 4 dead imports) — immediate, free.
3. Families 4,5,7,9 (sinks/relocations/type consolidation).
4. Families 2,3,6,8,10,11 (contracts + injection; behavior-preserving).
5. Families 12,13 (lift service-backed tools + agent subtree).
6. Graph gate: before bodies move, graph_check predicts 9 singleton SCCs.

Each step dual-preset -Werror + serial ctest; ctest total reconciled
(prompt_category deletion and any moved test seams are counted exactly).

## 5. Persisted-data hazard (was under-covered)

MCP server config is user-persisted (settings cascade per CLAUDE.md) and has
6 struct copies. Family 9's acceptance: one canonical
`cc.config.mcp_types` model, all call sites compile against it, and
existing settings JSON (file, env, project precedence) round-trips without
loss; add/extend a ctest that loads an old-shaped config.
