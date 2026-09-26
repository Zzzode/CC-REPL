# RFC 0001 Phase B — remaining-families execution plan (2026-09-26)

Status: **design artifact**, produced by a 17-agent workflow (8 parallel family mappers over the live tree at 7aa4daa (module counts in per-batch specs are measured live; the replay parses the working tree, not the 7aa4daa snapshot), one adversarial verifier per plan, one synthesis with an executable cumulative Tarjan replay). Every family plan was initially returned `request-changes` by its skeptic; the corrections below are merged. This is the execution contract for the remaining Phase B work; each batch still gets its own implementation agent + independent adversarial review + dual-preset/serial-ctest/macos-14 gates, and RFC §12 records measured results per landed batch.

Live inputs: 841 modules/958 units; one directory SCC `{cc.hooks, cc.services, cc.tools}`; `graph_check --target-core8` FAIL; 13 frozen upward edges (backlog), 139 frozen dead imports; serial ctest baseline **1699**.

## Batch order

| # | Batch | Families | Depends on | ctest |
|---|---|---|---|---|
| 1 | B1 F9 dead-module purge | F9/B1 | — | 1699/1699 unchanged (zero test references; no new cases). |
| 2 | B2 F9 canonical mcp_types leaf | F9/B2 | [1] | 1699/1699 unchanged. |
| 3 | B3 F9 services alias onto canonical model | F9/B3 | [2] | 1699/1699 (renames only; aggregate inits use designated initializers). |
| 4 | B4 F9 loader sink cuts mcp -> cc.config.config | F9/B4 | [3] | 1699 + 6 new TESTs (2 loader-sink + 4 config round-trip; gtest_discover_tests auto-counts) = 1705 expected; zero failures. |
| 5 | B5 F4 tool_types DTO sink (atomic; target gate flips PASS) | F4 | [4] | 1705/1705 unchanged count (spelling-only edits). --target-core8 PASSES for the first time at this batch (orchestration node exists as isolated singleton — an unconnected node is still a singleton SCC). |
| 6 | B6 F3/8 additive snapshots sink in mcp_tool | F3/F8 step1 | [5] | 1705/1705 unchanged. |
| 7 | B7 F3/8 interim bootstrap bridge + composition wiring | F3/F8 step2-3 | [6] | 1705/1705 unchanged (interim idempotent double publish). |
| 8 | B8 F3/8 atomic cut of the 3 SCC legs | F3/F8 atomic cut | [7] | 1705 + 2 new = 1707; the 4 rewrites are count-neutral 1:1. |
| 9 | B9 optional CMake link hygiene | F3/8 hygiene | [8] | 1707/1707; separate commit so any link surprise is isolated from the graph cut. |
| 10 | B10 F10-A cc.skills.file_access.port extraction | F10/A | [9] | 1707/1707; zero behavior change (same inline storage, same registrar, same call sites). |
| 11 | B11 F10-B image codec port + cc_orchestration target born | F10/B | [10] | 1707 + 2 = 1709; the 5 adapted cases keep identical assertions. |
| 12 | B12 F11-C SkillLoader executor into the unified seam | F11/C | [11] | 1709/1709; registry-contains sentinels at :1221/:1271 unchanged. |
| 13 | B13 F14-Alpha1 agent permission types sink | F14/Alpha1 | [12] | 1709/1709; one entity, one owner; pure mechanical type move. |
| 14 | B14 F14-Alpha2 worktree leaf + runtime_team_shared rewire | F14/Alpha2-3 | [13] | 1709/1709; the two worktree tests pin identical removed/dirty outcomes and messages. |
| 15 | B15 B11 ATOMIC: lifts + unified runtime_backends composition (9 singletons) | F12, F13, F14-Beta, B11, F3/8 bridge rehome | [14] | 1709/1709 — B11 adds/deletes/renames ZERO test cases (import/link/entry-point edits + one pre-main installer TU); final reconciled serial total 1709 (1699 baseline + F9's 6 + F3/8's 2 + F10's 2; the F10 unset-codec case was dropped per the corrected design). |

## Cumulative graph proof

Method: `0001-phase-b-replay.py` (committed beside this document) imports tools/arch/graph_check.py itself, calls load_units()+module_deps() on master 7aa4daa (841 modules/958 units — matches live gate output), then mutates one in-memory module->imports set per batch with the EXACT edge edits listed in each batch's expectedGraphDelta (new modules, edge drops/adds, module renames via rename() which rewrites every surviving importer, and module kills). After each batch it runs three predicates using graph_check's own functions: (1) tarjan_scc over the FULL module graph (DAG check); (2) the illegal-upward-edge computation with is_contract() (leaf == 'port' or endswith '_types' or cc.types.*) diffed against the live 13-edge frozen baseline (additions fail, removals allowed); (3) the exact --target-core8 predicate from graph_check.py:674-686 — tg seeded with ALL NINE TARGET_AREAS keys (so cc.orchestration is an isolated singleton node even before any module exists), edges added only when BOTH endpoint areas are in the set, SCC = tarjan_scc(tg) components of size >1. Replay output, verbatim: [live] modcycles=0 newUp=0 illegalUp=13 targetSCCs=[['cc.hooks','cc.services','cc.tools']] pass9=False; [B1] SCC same 3-node, up13; [B2] same; [B3] same (services->config is 7->1 down); [B4] same; [B5 F4] illegalUp=12 targetSCCs=none pass9=True — the gate flips HERE (the streaming_executor edge is the unique services->tools leg; once gone no return path from services to the trio exists, even though hooks legs survive until B8); [B6] pass9=True; [B7] pass9=True (bridge is area cc.bootstrap rank13, NOT one of the 9 nodes); [B8 F3/8 cut] illegalUp=10 (baseline lines 12-13 pruned) pass9=True; [B9] unchanged; [B10] pass9=True (file port edges 8->5 contract; agent_resume dead skill import deleted); [B11] cc.orchestration node gains real modules with down-edges only -> pass9=True; [B12] pass9=True; [B13] pass9=True; [B14] pass9=True; [B15 B11 atomic] modcycles=0 newUp=0 illegalUp=10 targetSCCs=none pass9=True. FINAL 9-area adjacency printed: cc.config->{cc.utils}; cc.hooks->{cc.utils}; cc.services->{cc.config,cc.utils}; cc.skills->{cc.config,cc.utils}; cc.state->{cc.task_types,cc.utils}; cc.task_types->{cc.utils}; cc.tools->{cc.skills,cc.utils} (the 3 surviving tools->skills edges are all *.port contracts); cc.utils->{}; cc.orchestration->{cc.config,cc.hooks,cc.services,cc.skills,cc.tools,cc.utils} — every outgoing orchestration edge goes to strictly lower rank, and all 9 nodes are singleton SCCs. The residual 10 illegal-up edges are the pre-RFC cc.config/cc.migrations -> cc.utils backlog (never in scope; removals-only discipline keeps them). Link-level proof: the script separately parses every target_link_libraries() in src/cmake/targets/*.cmake, Tarjans the cc_* target graph live (no cycle) and post-B15 as modeled (cc_orchestration new with PUBLIC links to cc_tools/cc_services/cc_skills_core/cc_hooks/cc_config/cc_utils/cc_types; cc_tools trimmed; cc_bootstrap loses cc_tools; cc_commands/cc_ui/cc_server/cc_core add cc_orchestration; cc_skills drops cc_tools) -> NO cycle; orchestration linkers post-B15 = cc_commands, cc_core, cc_server, cc_ui (all strictly upper layers; no lower library links back). Script exit code 0. Re-run command: python3 docs/rfcs/attachments/0001-phase-b-replay.py

The replay script is committed beside this file as `0001-phase-b-replay.py` (it imports `tools/arch/graph_check.py`; run from repo root). Re-derive it after graph_check edits.


## B15 — irreducible atomic diff (lifts + unified seam)

MINIMUM IRREDUCIBLE ATOMIC DIFF (single commit/PR; no intermediate is CI-green), with all F12/F13/F14/B11 verdict corrections merged and one unified seam:

(1) MOVES (git mv; module NAME changes are the graph cut; namespaces cc::tools retained byte-for-byte): src/tools/agent_tool.cppm, agent_run.cppm, agent_resume.cppm, agent_fork.cppm, agent_sub_utils.cppm + impls agent_sub_utils_{budget,config,hooks,json,messages,teammates,tools_mcp}.cpp -> src/orchestration/agent/ as cc.orchestration.agent{,.run,.resume,.fork,.utils} (the 7 impl units become `module cc.orchestration.agent.utils;`); src/tools/mcp_tool.cppm -> src/orchestration/tools/mcp_tool.cppm (cc.orchestration.tools.mcp); src/tools/lsp_tool.cppm -> src/orchestration/tools/lsp_tool.cppm (cc.orchestration.tools.lsp); src/tools/spawn_multi_agent.cppm -> src/orchestration/agent/spawn_multi_agent.cppm (cc.orchestration.agent.spawn_multi_agent); src/tools/runtime_registry_computer_use.cpp -> src/orchestration/runtime_backends_computer_use.cpp (impl of cc.orchestration.runtime_backends); src/bootstrap/mcp_connectivity.cppm -> src/orchestration/mcp_connectivity.cppm (cc.orchestration.mcp_connectivity).

(2) cc_tools STRIP (same commit; any surviving tools->renamed edge is a new non-contract 8->9 edge AND a 2-node SCC): runtime_registry.cppm delete imports agent/lsp/mcp, delete LSP decls (153-157), computer-use decls (280-305, 326-329), collector decls (417-420); register.cpp delete agent block + collector bodies 509-559; dispatch.cpp replace lsp/mcp/list_mcp_resources/read_mcp_resource/mcp_auth/computer_use branches with seam lookups (null-sink string stays 'Runtime tool \'{}\' has no runtime handler'); executors.cpp delete lsp trio 52-114 (moves verbatim next to lsp_action_name). The FOUR computer-use test setters (runtime_registry.cppm:314-322) keep their strong symbols IN cc_tools: define their trivial bodies inline in the cppm (assign/reset the exported inline detail::computer_use_*_override vars; retain `import cc.tools.computer_use;` line 22). Add `using cc::tools::AgentLivePermissionCheck; using cc::tools::AgentLivePermissionCheckFn;` re-exports (qualified ids, NOT unqualified using-declarators). Agent registration becomes one registrar call placed BEFORE `auto permission_check = std::move(options.permission_check);` at register.cpp:113, invoked on a COPIED checker (the simple() lambda at :228 reuses it). RuntimeToolOptions.agent_tool_factory appended as LAST member.

(3) NEW cc_orchestration MODULES: cc.orchestration.runtime_backends (.cppm with `export namespace` blocks for cc::tools collector decls + cc::orchestration::install_runtime_backends; inter-TU non-exported decls lsp_backend/mcp_backend/list_mcp_resources_backend/read_mcp_resource_backend/mcp_auth_backend/computer_use_backend each Result<ToolResult>(const ToolInput&); impl TUs runtime_backends_lsp.cpp (verbatim 11-string parse_lsp_action mirror), runtime_backends_mcp.cpp (four inline branches lifted as standalone fns deriving their own input.json(); two collect_* fns), runtime_backends_computer_use.cpp (MOVED TU; ADDS import cc.tools.runtime_registry for json_string/json_int + override vars and cc.tools.image_codec.port; mcp import renamed; keeps cc.services.image), runtime_backends_install.cpp (std::call_once; builds six RuntimeToolExecutors incl computer_use covering BOTH 'computer_use'/'computer', the skill executor bound B12, the agent registrar binding make_agent_tool 5-arg, snapshot sinks live in function-local statics of cc.tools.runtime_backends.port — NO raw pointer ever escapes). cc.orchestration.mcp_connectivity (to_hook_status/project_connectivity from interim bridge; wire body folded into install_runtime_backends; imports hooks.remaining_notifs, services.mcp.{types,connection_manager}, orch.tools.mcp).

(4) CMAKE (irreducible): new rows in cc_orchestration.cmake for all moved FILE_SET/PRIVATE files (PUBLIC links cc_tools cc_services cc_skills_core cc_hooks cc_config cc_utils cc_types); delete the moved rows from cc_tools.cmake (8 FILE_SET + 7 PRIVATE + computer_use PRIVATE + spawn row; trim cc_tools links to cc_utils cc_types cc_skills_core yyjson uv_a); drop cc_tools from cc_skills.cmake:28; cc_bootstrap drops cc_tools and loses the bridge FILE_SET row; add cc_orchestration PUBLIC to cc_commands, cc_ui, cc_server and to cc_core INTERFACE (required: test_fix_lsp_tool links cc_core ONLY); cc_server link must be PUBLIC (server_main.cppm and test_services.cpp import server_routes).

(5) IMPORTER REWRITES (compile fails until all done): main.cpp (install call once at top of main ~1711/1797 BEFORE all 3 register sites and every all_statuses path; collectors at 2002/2007 + server_routes:818/822 + test_tools:11433/11436 import cc.orchestration.runtime_backends; missing-tool lambdas replaced by factory preserving all_statuses order/last_error/std::string{input.json()}/exact ToolNotFound text); server installs via std::call_once-safe installer (call before HttpServer accept loop ~server_main.cppm:700, never per-request inside execute_native_query); commands/mcp_cmd.cppm, commands/color.cppm, ui/prompt/at_attachments_impl.cpp:16, ui/prompt/autocomplete_sources_impl.cpp:16, commands/mcp/core_settings_loader.cppm import renames; 7 agent TUs' `import cc.tools.mcp` rewritten (agent_fork's deleted — dead baseline :60), intra-subtree imports renamed.

(6) BASELINES + CI (default gate fails otherwise): rename the 44 (45 minus agent_runtime) dead_imports rows cc.tools.agent* -> cc.orchestration.agent*; rename ONLY dead_imports_baseline.txt:140 (at_attachments -> cc.orchestration.tools.mcp); LEAVE :141 (-> cc.skills.bundled) untouched; rename inline_def_baseline.txt:49 and :57 preserving 'c1 c2-done'; flip .github/workflows/arch-check.yml after line 28 to also run `python3 tools/arch/graph_check.py --target-core8`.

(7) TESTS: new tests/test_runtime_backends_install.cpp (pre-main global calling install_runtime_backends — slot assignment only) compiled into test_tools + cc_orchestration link; test_tools import lines 30/31/53 renames PLUS `import cc.orchestration.runtime_backends;` for the collectors; bind the real agent factory at the 7 registry sites test_tools.cpp:4645,4757,4926,8716,8847,8879,8969 (runtime_registry_team_dispatch 'Agent' dispatches at :135-136 and runtime_message_delivery:489-490 drive the team sites — 'zero body edits' was false); test_tasks.cpp:10 + link; test_fix_lsp_tool.cpp:32; test_fix_notifs.cpp bridge import -> cc.orchestration.mcp_connectivity. Total 1699 count unchanged by B11 (all renames/entry-point edits). Proof gate: 9 singleton SCCs (simulated), 0 tools->orchestration edges, dual-preset -Werror, serial ctest -j1, nm check single guard/setter definitions, --list-runtime-tools output identical.

HONEST BOUND: list_mcp_resources_tool.cppm deletion is EXCLUDED (zero importers but unrelated to the cycle — independent follow-up); AppConstructor elicitation/at-mention responder wiring stays ui->services (untouched); the 4 textual-dead bootstrap imports in the agent subtree are CARRIED verbatim (prune as separate dead-code commit post-B11 with real compile verification).


## Batch specifications


### B1 — B1 F9 dead-module purge

Families: F9/B1; dependencies: none.

**Changes:**

- git rm src/commands/mcp/add_command.cppm (cc.commands.mcp.add_command, legacy pipe-format ~/.loom/mcp_servers.txt, 0 importers)
- git rm src/cli/handlers/mcp_handler.cppm (cc.cli.handlers.mcp_handler, 0 importers; also removes the cc.cli.handlers.mcp_handler -> cc.utils.bash_execution edge)
- git rm src/entrypoints/mcp_entrypoint.cppm (cc.entrypoints.mcp_entrypoint Loom-as-MCP-server stub, 0 importers; its McpServerConfig{cwd,debug,verbose,cache} was an unrelated name collision)
- git rm src/ui/features/mcp/mcp_settings_panel.cppm (cc.ui.features.mcp.mcp_settings_panel, 0 importers)
- Delete the four CMake FILE_SET rows: src/cmake/targets/cc_commands.cmake:54, cc_cli.cmake:7, cc_entrypoints.cmake:9, cc_ui.cmake:185
- Delete dead get_logging_safe_mcp_base_url() at src/services/mcp/types.cppm:508-517 (comment at 508; zero callers tree-wide; also removes one pre-B3 .url adaptation site)
- Verified no string/registry registration references the symbols (grep start_mcp_server/handle_mcp_command/add_mcp_server/render_mcp_settings/create_mcp_server across src/tests/benchmarks returns only the deleted files)

**Predicted graph delta:** Modules deleted: 4. Module edge deleted: cc.cli.handlers.mcp_handler -> cc.utils.bash_execution (rank-DOWN 14->2, not baselined). No edge among the 9 target areas changes; area SCC remains {cc.hooks,cc.services,cc.tools}; illegal-up count unchanged 13; no new dead imports (the modules vanish with their rows).

**ctest:** 1699/1699 unchanged (zero test references; no new cases).


### B2 — B2 F9 canonical mcp_types leaf

Families: F9/B2; dependencies: [1].

**Changes:**

- Create src/config/mcp_types.cppm: global module fragment includes only (<optional>,<string>,<unordered_map>,<vector>); `export module cc.config.mcp_types;` `import std;`; export namespace cc::core with McpOAuthConfig (5 fields verbatim from config.cppm:130-136 incl. issuer) and McpServerConfig (11 fields verbatim from :144-156). SOLE cc import: none — a rank-1 leaf importing cc.utils rank-2 would be a NEW illegal upward edge.
- src/config/config.cppm: delete the two struct definitions (:130-136, :144-156) and add `export import cc.config.mcp_types;` after line 12; parse_and_merge/serialize_settings/Settings/XaaIdpSettings untouched
- src/cmake/targets/cc_config.cmake: add config/mcp_types.cppm to PUBLIC FILE_SET before config/config.cppm; no target_link change
- All 12 (live count, not 9) cc.config.config importers resolve cc::core::McpServerConfig unchanged through the re-export — zero call-site edits; the two existing ConfigManager MCP tests (test_services.cpp:5514,:5542) are the transparency guard

**Predicted graph delta:** New module cc.config.mcp_types (zero cc.* outgoing edges). Edge created: cc.config.config -> cc.config.mcp_types (intra-area re-export). Area graph unchanged; SCC {hooks,services,tools} unchanged; illegal-up 13; no new dead imports (re-export is skipped by the detector, graph_check.py:571).

**ctest:** 1699/1699 unchanged.


### B3 — B3 F9 services alias onto canonical model

Families: F9/B3; dependencies: [2].

**Changes:**

- src/services/mcp/types.cppm: add `export import cc.config.mcp_types;` (plain `import` is PROVEN to trip the dead-import detector because qualified `cc::core::X` RHS of aliases has only 1 '::' segment — graph_check.py:601-602; the re-export both silences the gate and grants complete-type reachability to auth/connection_manager/config_impl which do not import the leaf)
- Replace struct McpOAuthConfig (:448-453) and McpServerConfig (:455-460) with exported aliases inside export namespace cc::services::mcp: `using McpOAuthConfig = cc::core::McpOAuthConfig;` `using McpServerConfig = cc::core::McpServerConfig;`
- src/cmake/targets/cc_services.cmake:93-104 add cc_config PUBLIC (mandatory: no services TU imports cc.config.* today; BMI propagation is link-only in this P1689 build; no link cycle — cc_config links only utils/types/constants)
- Field adaptations (one atomic commit, partial state does not compile): src/services/mcp/auth.cppm:500 `.type`->`.transport`; six url reads :501,:621,:793,:804,:848,:914 -> `server_config.url.value_or(std::string{})`; connection_manager.cppm:457/:461/:465 `.type`->`.transport`; src/tools/mcp_tool.cppm to_oauth_server_config :297 `.type`->`.transport`, delete convert_core_oauth :246-256 and assign `native.oauth = server.oauth;` at :273
- Tests: test_state.cpp:1111 `.type`->`.transport`; test_services.cpp 7 sites :4587,:4678,:4765,:4850,:4917,:5035,:5113 ->`.transport` and :4881,:5002,:5094 ->`*auth_config.url` (always assigned just above)
- Shape invariant: get_server_key hashes JSON {"type":...,"url":...,"headers":...} (auth.cppm:496-518) — transport_to_auth_type outputs stay 'sse'/'http'/'stdio', url non-empty on remote paths, headers map unchanged; the 7 existing auth tests guard token-file keys

**Predicted graph delta:** Edge created: cc.services.mcp.types -> cc.config.mcp_types (rank 7->1 DOWNWARD, leaf suffix _types is is_contract()). Area edge cc.services -> cc.config NEW (down, legal); SCC unchanged; illegal-up stays 13; zero new dead imports (re-export exempt).

**ctest:** 1699/1699 (renames only; aggregate inits use designated initializers).


### B4 — B4 F9 loader sink cuts mcp -> cc.config.config

Families: F9/B4; dependencies: [3].

**Changes:**

- src/tools/mcp_tool.cppm: swap line 10 `import cc.config.config;` -> `import cc.config.mcp_types;`; add CoreSettingsMcpServersLoader = std::function<std::expected<std::vector<NativeMcpConfiguredServer>,std::string>()>, a function-local static slot (same anchor pattern as NativeMcpRuntime::instance :849 and global_mcp_router :1335) and set_core_settings_mcp_loader(); rewrite ensure_loaded_from_config core block :909-916 to call the loader and propagate its error; KEEP the services ConfigLoader merge :917-923 and plugin discovery :924 untouched; unset slot = core layer skipped (hermetic for test binaries)
- Create src/commands/mcp/core_settings_loader.cppm (module cc.commands.mcp.core_settings_loader; imports cc.config.config + cc.tools.mcp) exporting cc::commands::install_core_settings_mcp_loader(); lambda body is the VERBATIM deleted ConfigManager block (default ctor, load(), unexpected(loaded.error().message), map to_native_mcp_server); add FILE_SET row to cc_commands.cmake where add_command.cppm sat
- src/main.cpp: add import near :27 and call install_core_settings_mcp_loader() once right after apply_teammate_environment(opts) (:1796), before every MCP use (--run-runtime-tool ~:672, dynamic providers ~:2002); the single loom main also covers in-process server routes; mcp_cmd's explicit sync path (sync_native_runtime) correctly bypasses the slot
- Tests: test_tools.cpp NativeMcpRuntimeLoadsRemoteConfigWithOAuthFromConfigFiles (:9697) imports the installer, calls it before the :9725 reload, AND resets the slot to nullptr in cleanup (RAII); add CoreSettingsMcpLoaderFeedsLazyLoad and CoreSettingsMcpLoaderErrorPropagates (both reset); test_services.cpp add the 4 persisted-data cases (old snake_case read + camelCase rewrite round-trip, project-replaces-global merge semantics, env-layer non-interference) per plan testsAffected

**Predicted graph delta:** Edges deleted: cc.tools.mcp -> cc.config.config. Edges created: cc.tools.mcp -> cc.config.mcp_types (8->1 downward contract); cc.commands.mcp.core_settings_loader -> cc.config.config (11->1 down) and -> cc.tools.mcp (11->8 down). main.cpp is a non-module TU and creates NO graph edge. Area tools->config edge count stays 1 (now lands on the contract leaf); SCC unchanged; illegal-up 13.

**ctest:** 1699 + 6 new TESTs (2 loader-sink + 4 config round-trip; gtest_discover_tests auto-counts) = 1705 expected; zero failures.


### B5 — B5 F4 tool_types DTO sink (atomic; target gate flips PASS)

Families: F4; dependencies: [4].

**Changes:**

- ONE atomic commit — stepwise intermediates were PROVEN to fail the dead-import gate or ODR. Create src/types/tool_types.cppm: `export module cc.types.tool_types; import std;`, export namespace cc::core with ToolInput (raw_json + json() + static from_json; NO has_field), ToolOutputContent (4 fields + text/json/image/document_output factories verbatim, preserving "text"/"json"/"image"/"document" discriminators), ToolResult (content/is_error + success/error/success_multi verbatim, field ORDER text,format,media_type,data); add row to cc_types.cmake (no link change)
- src/tools/tool.cppm: delete struct region :45-139 (banner :41-43 stays), add `export import cc.types.tool_types;` next to :13, keep `import cc.utils.json;` (:14) and define FREE `[[nodiscard]] inline bool has_field(const ToolInput&, std::string_view) noexcept` with the identical body (empty-key guard, cc::utils::json::parse, parsed->root().has(key)); ToolPermission/SchemaProperty/InputSchema/ToolDefinition/concepts/ITool/ToolWrapper/ToolRegistry untouched
- RE-POINT (replace `import cc.tools.tool;` with `import cc.types.tool_types;`), verified DTO-only: src/query/query_engine_tools.cpp:13, src/tools/agent_sub_utils_teammates.cpp:11, src/tools/mcp_tool.cppm:16, src/tools/runtime_registry_computer_use.cpp:13, src/tools/runtime_registry_executors.cpp:14, src/tools/spawn_multi_agent.cppm:12
- KEEP-IMPORT (ADD `import cc.types.tool_types;` AND annotate the retained tool import with `// arch-check: keep-import`): src/tools/runtime_message_delivery.cppm, src/tools/runtime_registry_dispatch.cpp, src/tools/runtime_registry_team_dispatch.cpp — they genuinely use ToolRegistry, but graph_check's raw-string-naive parser cannot see it (R"(...)" literals in InputSchema::to_json :168-182 unbalance its brace stack); simulation proved the marker is required-and-sufficient
- src/services/tools/streaming_executor.cppm:14 swap to `import cc.types.tool_types;` (the SCC keystone); delete line 14 `cc.services.streaming_executor -> cc.tools.tool` from tools/arch/upward_edge_baseline.txt
- tests/test_tools.cpp: re-point 16 expects at :2221-2227 (7), :2231-2233 (3), :10097-10102 (6) to cc::core::has_field(input,key) across the 3 named TESTs (:2214,:2230,:10095); names unchanged

**Predicted graph delta:** Module edges DELETED 3: streaming_executor/mcp/spawn_multi_agent -> cc.tools.tool. CREATED 8: cc.types.tool_types <- {tool (re-export), streaming_executor, query_engine, agent.utils, mcp, runtime_message_delivery, runtime_registry, spawn_multi_agent}. AREA: cc.services -> cc.tools disappears (the only services->tools edge); area SCC {hooks,services,tools} -> EMPTY. Illegal-up 13 -> 12 (baseline line removed); 0 new dead imports only WITH the 3 keep-import markers.

**ctest:** 1705/1705 unchanged count (spelling-only edits). --target-core8 PASSES for the first time at this batch (orchestration node exists as isolated singleton — an unconnected node is still a singleton SCC).


### B6 — B6 F3/8 additive snapshots sink in mcp_tool

Families: F3/F8 step1; dependencies: [5].

**Changes:**

- Adopt the review's SINGLE-SNAPSHOT-SINK design (the original two-sink provider+tick design was rejected: needless second fetch and 9 extra exports). In src/tools/mcp_tool.cppm add `using McpSnapshotsSink = std::function<void(std::vector<svc_mcp::McpServerSnapshot>)>;`, a detail:: function-local holder (precedent global_mcp_router :1335-1338), and set_mcp_snapshots_sink(std::function)
- Restructure NativeMcpRuntime::all_statuses() (:995-1009): take `auto snapshots = manager_->snapshot_all_servers();` as a NAMED local INSIDE the lock_guard scope, build statuses from it, and AFTER the scope: `if (auto& s = detail::mcp_snapshots_sink(); s) s(std::move(snapshots));`. ensure_loaded_from_config() failure still returns {} before any sink fire (identical failure semantics). Do NOT add current_snapshots/native_mcp_server_snapshots.
- KEEP the existing call at :1004 for this batch (interim double-publish; both projections identical; this commit alone is green). One fetch, no re-entrant ensure_loaded, no added failure mode.
- src/hooks/notifs/remaining_notifs.cppm untouched this batch (keeps DTO, set_raw_mcp_connectivity, the two services imports)

**Predicted graph delta:** No module-edge change (sink is a std::function over svc_mcp types already imported; `import std;` present). SCC empty; gates unchanged.

**ctest:** 1705/1705 unchanged.


### B7 — B7 F3/8 interim bootstrap bridge + composition wiring

Families: F3/F8 step2-3; dependencies: [6].

**Changes:**

- Create src/bootstrap/mcp_connectivity.cppm: `export module cc.bootstrap.mcp_connectivity;`, imports std + cc.hooks.remaining_notifs + cc.services.mcp.types + cc.services.mcp.connection_manager + cc.tools.mcp; export namespace cc::bootstrap::mcp_connectivity with to_hook_status (verbatim 5-arm switch incl NeedsAuth->Error), project_connectivity (verbatim: both ids = snap.name, last_error copy, now-ms), wire_mcp_connectivity_bridge(). wire() body: cc::tools::set_mcp_snapshots_sink([](std::vector<cc::services::mcp::McpServerSnapshot> snaps){ cc::hooks::notifs::set_raw_mcp_connectivity(project_connectivity(std::move(snaps))); })
- QUALIFY cc.tools SYMBOLS WITHOUT A LEADING '::' (`cc::tools::set_mcp_snapshots_sink`, not `::cc::tools::...`): leading-colon makes graph_check flag `import cc.tools.mcp` as a NEW dead import (1-segment path rule, graph_check.py:477-486,601-610); add an in-file warning comment. Leading-:: is safe for the deep cc::services::mcp names only.
- src/cmake/targets/cc_bootstrap.cmake: add bootstrap/mcp_connectivity.cppm to FILE_SET and cc_tools to target_link_libraries (forward target ref legal: cc_bootstrap included at src/CMakeLists.txt:109 before cc_tools :112)
- src/main.cpp: add `import cc.bootstrap.mcp_connectivity;` near :25 and call wire_mcp_connectivity_bridge() after apply_teammate_environment (~:1797), before every all_statuses path (:674,:1951, server_routes:796, registry enumeration sites :522/:551/:255/:180). Idempotent; old :1004 call still present (provably behavior-identical interim).

**Predicted graph delta:** New module cc.bootstrap.mcp_connectivity with 4 edges -> hooks(4)/services(7)x2/tools(8), ALL rank-DOWN from bootstrap rank 13; bootstrap is NOT one of the 9 target areas so target graph is unaffected. Illegal-up unchanged; no new dead imports provided the no-leading-colon rule is honored.

**ctest:** 1705/1705 unchanged (interim idempotent double publish).


### B8 — B8 F3/8 atomic cut of the 3 SCC legs

Families: F3/F8 atomic cut; dependencies: [7].

**Changes:**

- ONE atomic commit. src/hooks/notifs/remaining_notifs.cppm: delete imports :58-59, to_mcp_server_status (:475-486), inject_mcp_connectivity_from_manager (:488-509), stale banner text :14-18/:457-473. HOOKS SIDE IS DELETIONS ONLY — no provider tag/type/get/set, no refresh_mcp_connectivity (the review's adopted design); enum McpServerStatus (:424-430), McpConnectivityInfo (:432-438), the slot (:440-446) and set_raw_mcp_connectivity stay
- src/tools/mcp_tool.cppm: delete `import cc.hooks.remaining_notifs;` (:18) and the :1004 call — the bridge-installed snapshot sink now drives the hook slot exclusively
- Prune tools/arch/upward_edge_baseline.txt lines 12-13 (the two hooks->services identities) so the frozen backlog stays truthful
- tests/test_fix_notifs.cpp: rewrite the 4 mapping cases (:65-87) against cc::bootstrap::mcp_connectivity::to_hook_status with svc_mcp::ConnectionStatus (imports cc.bootstrap.mcp_connectivity); NEW ProjectConnectivityMapsSnapshots (synthetic snapshot: name->both ids, NeedsAuth->Error, last_error, last_seen_ms>0) and WireSinkRefreshesSlot (empty temp cwd => snapshot_all_servers iterates mcp_config_.servers and returns {}; pre-seed sentinel slot, assert sink-driven replacement to {}); fixture dtor MUST reset set_mcp_snapshots_sink({}) and clear the hook slot (process-globals). Add cc_bootstrap to test_fix_notifs link libs; also add test_fix_notifs.cpp to the LATER B15 rename checklist (second bridge importer the old plan missed)
- tests/test_hooks.cpp: NotifHooks.McpConnectivityIssueDetected (:510-527) untouched (aggregate field ORDER and detail::now_ms stay exported); no provider/refresh case (that API was dropped from the design)

**Predicted graph delta:** Edges DELETED 3: cc.hooks.remaining_notifs -> cc.services.mcp.types and -> cc.services.mcp.connection_manager; cc.tools.mcp -> cc.hooks.remaining_notifs (the ONLY tools->hooks edge). hooks gains zero imports, tools gains zero. Illegal-up 12 -> 10 (baseline pruned). Area: hooks->services 2->0, tools->hooks 1->0; target stays 9 singletons (already since B5).

**ctest:** 1705 + 2 new = 1707; the 4 rewrites are count-neutral 1:1.


### B9 — B9 optional CMake link hygiene

Families: F3/8 hygiene; dependencies: [8].

**Changes:**

- Remove cc_services from src/cmake/targets/cc_hooks.cmake:46-56 (post-B8 no hooks module imports services — grep-verified)
- Remove cc_hooks from src/cmake/targets/cc_tools.cmake:123-133 (the mcp import was the only tools->hooks edge)
- Audit recorded in the review and re-grepped at execution per target: cc_ui links both explicitly (cc_ui.cmake:305-321 with anticipatory comment), cc_tasks keeps its own cc_hooks link (in_process_teammate_task.cppm:13 uses hook symbols), cc_query links both explicitly (cc_tools.cmake:138), cc_server reaches cc_hooks via cc_query PUBLIC, cc_bootstrap keeps its own direct links; tests use the cc_core aggregator

**Predicted graph delta:** No module-graph change (CMake-only); no CMake target cycle (verified by the link-graph Tarjan in /tmp/phaseB_sim.py).

**ctest:** 1707/1707; separate commit so any link surprise is isolated from the graph cut.


### B10 — B10 F10-A cc.skills.file_access.port extraction

Families: F10/A; dependencies: [9].

**Changes:**

- Create src/skills/file_access_port.cppm: `export module cc.skills.file_access.port; import std;` exporting in namespace cc::skills the FOUR symbols moved VERBATIM from src/skills/skill.cppm:368-420 (FileAccessHook starts :384): the typedef, detail::file_access_hook() function-local static, set_file_access_hook, notify_file_access incl. the try/catch swallow. Keep them INLINE in the SAME cc_skills_core target so the existing COMDAT/guard identity (relied on by the load_skills_dir registrar and cc_tools callers) is preserved; add FILE_SET row to cc_skills_core.cmake
- src/skills/skill.cppm: delete the :368-420 block and add `export import cc.skills.file_access.port;` after :14 (re-export keeps load_skills_dir.cppm registrar and all importers working)
- src/tools/file_read_tool.cppm:17, src/tools/file_edit_tool.cppm:38, src/tools/file_write_tool.cppm:13: swap `import cc.skills.skill;` -> `import cc.skills.file_access.port;`; call text cc::skills::notify_file_access(path,cwd) unchanged at file_read:420/file_edit:598/file_write:291
- MANDATORY reveal cleanup (simulation-proven): removing the hook block deletes a spurious exported 'void' token that was masking a genuinely dead import — delete `import cc.skills.skill;` at src/tools/agent_resume.cppm:28 (zero cc::skills references; same textual deadness exists in agent_fork/run/tool but their masks survive — leave those for B15)

**Predicted graph delta:** Edges deleted: file_read/file_edit/file_write -> cc.skills.skill (3), agent.resume -> cc.skills.skill (1). Created: same 3 file tools -> cc.skills.file_access.port (8->5 DOWN, leaf 'port' is is_contract structural) and cc.skills.skill -> cc.skills.file_access.port (re-export, intra-area). Area tools->skills 9->8 (module count); SCC stays empty; no new dead imports only if the agent_resume import is deleted.

**ctest:** 1707/1707; zero behavior change (same inline storage, same registrar, same call sites).


### B11 — B11 F10-B image codec port + cc_orchestration target born

Families: F10/B; dependencies: [10].

**Changes:**

- Create src/tools/image_codec_port.cppm with EXACTLY ONE module declaration (the sketched doubled `export module` was a compile error): global fragment optional, `export module cc.tools.image_codec.port; import std;`; export namespace cc::tools::image_codec with ImageInfo{width,height,size_bytes,mime,summary}, GetInfoFn/ToBase64Fn/FromBase64Fn, Codec struct with explicit operator bool, set_codec/codec/has_codec backed by ONE function-local static; ALSO add clear_codec() so tests can reset (review correction). Add FILE_SET row to cc_tools.cmake
- src/tools/file_read_tool.cppm: delete `import cc.services.image;` (:13) and using-decls :30-31, add the port import; read_image (:508-531) acquires codec(), returns error string when unset, maps via info.mime/info.summary (the 'Image file read:' text, application/octet-stream rejection :519 stay byte-identical); both to_base64 calls (:523,:540 incl. PDF) route via codec
- CREATE THE cc_orchestration TARGET HERE (first lift-wave home, per F10/F11 sequencing): src/cmake/targets/cc_orchestration.cmake — add_library(cc_orchestration), one PUBLIC FILE_SET src/orchestration/runtime_backends.cppm module cc.orchestration.runtime_backends written with `export namespace cc::orchestration { ... }` (non-exported namespaces made all three roots fail to compile in simulation) and `export namespace cc::tools` only where collector names will later live; THIS COMMIT's imports are JUST std + cc.services.image + cc.tools.image_codec.port (+ cc.utils.json accepted) — the four skill/registry/agent imports must wait for B12 or they are NEW dead imports. Implement make_image_codec() (forwards get_info: mime=format_to_mime, summary=ImageInfo::summary(), error=.error().message; to/from_base64 verbatim) and install_runtime_backends() (std::call_once; codec only this batch). target_link_libraries cc_orchestration PUBLIC cc_tools cc_services cc_skills_core cc_utils (+cc_types); include() in src/CMakeLists.txt after line 118
- Composition wiring: src/main.cpp import + call cc::orchestration::install_runtime_backends() immediately before the opts.list_runtime_tools block (:1826; covers registry sites :1828/:1847/:1912). src/server/server_routes.cppm: import near :15 and call once before the per-session ToolRegistry at :771 (installer is call_once-guarded so this is race-free despite per-request invocation). CMake: cc_server PUBLIC cc_orchestration (server_main.cppm + test_services.cpp re-export server_routes), loom explicit link
- runtime_registry_computer_use.cpp: swap import :19 to the port; from_base64 :137 and to_base64 :281 go through the accessor with unset guards; rgba->png mapping :286-294 stays; THIS BATCH keeps the TU in cc_tools (the image edge is cut via the port); the TU physically moves in B15
- tests: add `import cc.orchestration.runtime_backends;` to test_tools.cpp imports; FileToolServicesGuard RAII (ctor install, DTOR clear_codec + clear executor) next to RuntimeComputerUseProviderGuard :375, instantiated in the 5 cases :1798,:1842,:2691,:2725 and (in B12) :4167; add cc_orchestration to test_tools link libs. Add only TWO new cases (get_info PNG-header mime/summary; base64 round-trip AQIDBA==) — the unset-codec case is DROPPED from this binary (first guard at :1798 installs globally; a clear+reset variant can live in a separate binary later)

**Predicted graph delta:** Edges deleted: cc.tools.file_read -> cc.services.image and cc.tools.runtime_registry -> cc.services.image (the 2 family-10 edges). Created intra-tools: file_read and runtime_registry -> cc.tools.image_codec.port. NEW AREA NODE cc.orchestration appears: cc.orchestration.runtime_backends -> cc.services.image (9->7 down) and -> cc.tools.image_codec.port (9->8 down). Area tools->services 25->23. SCC stays empty; --target-core8 stays PASS (orchestration has down-edges only, nobody imports it in-module; main.cpp/server wiring is invisible to graph_check).

**ctest:** 1707 + 2 = 1709; the 5 adapted cases keep identical assertions.


### B12 — B12 F11-C SkillLoader executor into the unified seam

Families: F11/C; dependencies: [11].

**Changes:**

- Create src/tools/runtime_backends_port.cppm: `export module cc.tools.runtime_backends.port; import std; import cc.types.tool_types;` — THE UNIFIED REGISTRY SEAM (resolution of the F12 vs F14 seam conflict: one module, one installer). This batch adds only `using SkillLoaderExecutor = std::function<std::optional<cc::core::Result<cc::core::ToolResult>>(const cc::core::ToolInput&)>;` plus function-local-slot storage + set/clear/accessor; B15 extends the same header with the six RuntimeToolExecutors, mcp providers, missing-tool handler and agent factory so F12's ServiceBackedToolSinks and F14's RuntimeBackendSinks never diverge. Add FILE_SET row to cc_tools.cmake
- src/tools/runtime_registry.cppm: add SkillLoaderExecutor + inline std::optional<SkillLoaderExecutor> override storage beside the computer-use overrides (:307-309); declare set/clear_skill_loader_executor (strong defs anchored in runtime_registry_skills.cpp). src/tools/runtime_registry_skills.cpp: delete BOTH `import cc.skills.skill;` (:12) AND `import cc.tools.agent_runtime;` (:13 — its use moves; deleting only :12 leaves a proven NEW dead import); keep the name parse + manual SKILL.md walk (:49-64) as terminal fallback; add setter defs
- src/tools/runtime_registry_dispatch.cpp skill branch (:218-226): order stays execute_skill_tool_simple -> skill_loader_executor_override (nullopt falls through) -> manual walk; terminal strings 'skill requires name'/'Skill not found: {}' unchanged
- src/orchestration/runtime_backends.cppm: THIS commit adds the four imports previously deferred (cc.skills.skill, cc.tools.agent_runtime, cc.tools.runtime_registry, cc.tools.tool) and make_skill_loader_executor(). CRITICAL: the moved SkillLoader block (HOME/.codex/skills, HOME/.agents/skills, cwd/skills, SkillLoader ctor defaults, discover_all_with_plugin_skills via agent_runtime::discover_plugin_component_paths, json_string field read) lives INSIDE the returned lambda — HOME/cwd/plugin paths must evaluate PER CALL (the plugin test at test_tools.cpp:4167 sets cwd at :4112 before execute at :4166; install-time evaluation breaks it); install_runtime_backends() also binds set_skill_loader_executor
- Add FileToolServicesGuard to Tools.AgentToolLoadsPluginAgentsAndPluginSkills (:4018, execute 'plugin-fixture:review-skill' at :4167 — qualified name shape is '<plugin>:<skill>' produced by skill.cppm:159-162, NOT 'plugin:<plugin>:<skill>'; body moves verbatim)

**Predicted graph delta:** Edge deleted: cc.tools.runtime_registry -> cc.skills.skill (6th family edge). Created intra-tools: runtime_registry -> cc.tools.runtime_backends.port (and port -> cc.types.tool_types, 8->0 contract). cc.orchestration.runtime_backends adds edges -> cc.skills.skill (9->5), cc.tools.agent_runtime/runtime_registry/tool (9->8), cc.utils.json (9->2): ALL down. Area tools->skills 8->7; tools final skills edges are the 3 file port edges; SCC empty.

**ctest:** 1709/1709; registry-contains sentinels at :1221/:1271 unchanged.


### B13 — B13 F14-Alpha1 agent permission types sink

Families: F14/Alpha1; dependencies: [12].

**Changes:**

- One green commit (F14 Alpha 1). src/tools/agent_types.cppm already exists with zero cc.* imports: add inside `export namespace cc::tools` the three types VERBATIM from agent_sub_utils.cppm:38-58 — AgentConfig (fix typo: prefer_in_process_teammate, defaults max_turns=200/max_depth=3/default_model literal), AgentLivePermissionCheck, `using AgentLivePermissionCheckFn = std::function<AgentLivePermissionCheck(std::string_view,std::string_view,std::string_view)>;` — plus `export namespace cc::tools::agent { using cc::tools::AgentConfig; using ...; using ...; }` so cc::tools::agent::X resolves from the leaf
- src/tools/agent_sub_utils.cppm: delete defs :38-58, add `import cc.tools.agent_types;` and inside export namespace cc::tools::agent::utils add the 3 exported using-aliases (agent_tool.cppm `using utils::AgentConfig` at :74-76 and intra-cluster references keep resolving; the ~25 using-declarations need no edit)
- src/tools/team_create.cppm:10, src/tools/team_delete.cppm:11, src/tools/runtime_registry.cppm:19: swap `import cc.tools.agent;` -> `import cc.tools.agent_types;`; bodies unchanged (AgentLivePermissionCheck(Fn) now resolve from the leaf). runtime_registry.cppm:397-398 become qualified re-exports `using cc::tools::AgentLivePermissionCheck; using cc::tools::AgentLivePermissionCheckFn;` (main.cpp:585-650 and server_routes spell cc::tools:: names while importing only runtime_registry)
- graph_check open-namespace caveat (ns_owner unions): re-run the dead-import gate after the sink and adjust baseline only if attribution flips

**Predicted graph delta:** 3 reverse tools->tools edges repointed to the leaf (team_create, team_delete, runtime_registry -> cc.tools.agent_types instead of cc.tools.agent); agent.utils -> agent_types intra-area. NO area change; the cc.tools.agent facade still has 4 importers (runtime_team_shared, spawn, + these were 2 of them) — note runtime_registry's facade edge is now gone here, shrinking B15's blast radius.

**ctest:** 1709/1709; one entity, one owner; pure mechanical type move.


### B14 — B14 F14-Alpha2 worktree leaf + runtime_team_shared rewire

Families: F14/Alpha2-3; dependencies: [13].

**Changes:**

- Create src/tools/agent_worktree.cppm: `export module cc.tools.agent_worktree; import std;` exporting in `namespace cc::tools::agent` AgentWorktreeCleanupResult (verbatim fields :594-599) and `[[nodiscard]] AgentWorktreeCleanupResult cleanup_agent_worktree(std::string_view agent_id);`. Create src/tools/agent_worktree.cpp (`module cc.tools.agent_worktree;`) importing std, cc.tools.agent_runtime, cc.utils.git, AND cc.tools.runtime_shared_utils; move the body verbatim from agent_sub_utils_hooks.cpp:619-end but replace the TWO shell_quote calls (:683,:689) with cc::tools::runtime_shared_utils::shell_quote (diffed byte-identical single-quote escaper) — a verbatim move would make tools(8) import the LIFTED agent.utils(9) in B15 and recreate the {tools,orchestration} SCC through agent_runtime (15 importers). Add both rows to cc_tools.cmake (FILE_SET + PRIVATE)
- agent_sub_utils.cppm: delete struct+decl :594-601, add `import cc.tools.agent_worktree;` and aliases in utils namespace (AgentWorktreeCleanupResult + cleanup_agent_worktree); delete the body from agent_sub_utils_hooks.cpp keeping its shell_quote/guards/hook-runner machinery
- src/tools/runtime_team_shared.cppm:22 swap `import cc.tools.agent;` -> `import cc.tools.agent_worktree;`; call at :551 and field reads :552-555 unchanged. Update src/tools/agent_tool.cppm:139 using to agent_runtime/worktree spelling so facade calls :397/:602 and the two test sites test_tools.cpp:6805/:6868 resolve
- Exactly one strong cleanup_agent_worktree definition (nm check); the 3 RAII guard dtors stay singly defined in the (later lifted) hooks impl TU

**Predicted graph delta:** Edges deleted: cc.tools.runtime_team_shared -> cc.tools.agent (3rd reverse facade leg; runtime_registry cut B13, team pair B13 — spawn is the only facade importer left for B15). Created intra-tools: agent_worktree -> {agent_runtime, runtime_shared_utils} and -> cc.utils.git (8->2 down); runtime_team_shared/agent.utils -> agent_worktree. Area unchanged; SCC empty.

**ctest:** 1709/1709; the two worktree tests pin identical removed/dirty outcomes and messages.


### B15 — B15 B11 ATOMIC: lifts + unified runtime_backends composition (9 singletons)

Families: F12, F13, F14-Beta, B11, F3/8 bridge rehome; dependencies: [14].

**Changes:**

- THE ATOMIC MILESTONE — full irreducible content in field 'atomicB11' (must be applied as one commit; every ordering simulation shows F12-only or F13-only intermediates produce a 4-area SCC + 7 new non-contract 8->9 upward edges, and F14-incomplete leaves {tools,orchestration}). Summary of converged decisions: modules cc.orchestration.tools.{mcp,lsp}, cc.orchestration.agent{,.run,.resume,.fork,.utils,.spawn_multi_agent}, cc.orchestration.{runtime_backends,mcp_connectivity}; namespaces cc::tools retained; ONE seam cc.tools.runtime_backends.port; ONE installer install_runtime_backends() std::call_once bound at main + server startup (never per-request: the old server_routes:772 site is a per-connection thread spawned at server_main.cppm:705 — concurrent std::function assignment was a real data race); computer_use is one executor covering both dispatch names; 4 test setters stay strong in cc_tools (inline in runtime_registry.cppm); agent registrar copies permission_check and registers before the checker std::move; collectors decls deleted from tools and re-exported from orchestration; cc_core gains cc_orchestration or test_fix_lsp_tool fails to scan; baselines (44 agent dead-import rows + dead :140 ONLY + inline_def :49/:57) renamed; CI flips on --target-core8; bootstrap bridge git-mv'd (main.cpp AND test_fix_notifs imports updated) and cc_bootstrap drops cc_tools; cc_tools final links = cc_utils cc_types cc_skills_core yyjson uv_a; cc_skills drops cc_tools; cc_orchestration links cc_tools cc_services cc_skills_core cc_hooks cc_config cc_utils cc_types; cc_commands/cc_ui/cc_server(PUBLIC)/cc_core add cc_orchestration
- Pre-implementation spike REQUIRED first (see risks): produce the exact final cc.tools.runtime_backends.port header + the unified register_runtime_tools hunk, because four families rewrite the same register_runtime_tools/register.cpp:111-123 region and its four impl units; spike output is a compile-checked patch design, not a merge

**Predicted graph delta:** LIFTS (area rehome, same edge count): agent subtree 18 ->cc.services + 4 ->cc.skills, mcp 4 ->cc.services + 1 ->cc.config.mcp_types, lsp 1 ->cc.services, all re-anchored on cc.orchestration (9->7/5/1, DOWN); file/image and skills edges already cut B10-B12. DELETED area legs: tools->services 23->0 final (2 live residual after B11: file_read and registry image already cut B11; B15 moves the computer_use TU which deletes the temporary port edge from runtime_registry and re-homes services.image as orch 9->7); tools->hooks 0 (B8); tools->config 1->0 (rehomes orch->config); reverse facade legs 0 (B13/B14 + spawn lift); services->tools 0 (B5). CREATED: cc.orchestration -> {cc.config,cc.hooks,cc.services,cc.skills,cc.tools,cc.utils} all rank-down; commands/ui/server edges -> orchestration are 11/12/13->9 down. FINAL 9-area adjacency (simulated): config->{utils}; hooks->{utils}; services->{config,utils}; skills->{config,utils}; state->{task_types,utils}; task_types->{utils}; tools->{skills,utils} (3 contract *.port edges only); orchestration->{config,hooks,services,skills,tools,utils}; 9 singleton SCCs; module DAG; illegal-up 10 (all pre-RFC config/migrations backlog, untouched); zero NEW dead imports with the enumerated markers/baseline renames; zero CMake target link cycles.

**ctest:** 1709/1709 — B11 adds/deletes/renames ZERO test cases (import/link/entry-point edits + one pre-main installer TU); final reconciled serial total 1709 (1699 baseline + F9's 6 + F3/8's 2 + F10's 2; the F10 unset-codec case was dropped per the corrected design).


## Risks

- B15 is a ~40-file atomic diff with four families editing the SAME register_runtime_tools body (register.cpp:111-123 Agent block, :228 checker capture, :251-294 simple shells) and four impl units. Even with verdicts merged, an implementer cannot discover the final unified seam shape without cross-file design work. REQUIRED before execution: a short redesign/spike that produces the final cc.tools.runtime_backends.port header (six executors + skill executor + agent factory + mcp providers/missing-handler + storage/accessor declarations) and the exact register_runtime_tools hunk, compile-checked on a throwaway branch. F12, F13 and F14 standalone plans are UNSAFE to execute as written (simulated 4-area SCCs, 7 new upward edges, ODR-illformed declaration moves) — they must only ever land merged as B15.
- graph_check's dead-import parser is a coarse textual heuristic with two traps already proven by the reviews: raw-string literals hide ToolRegistry exports (the 3 keep-import markers in B5 are mandatory, not optional), and 1-segment namespace/qualified paths are not evidence (B3 must use `export import`, B7 must not use leading-::). Every batch's final step must run graph_check for real; the simulation proves area/SCC/upward predicates but cannot substitute for the textual dead-import gate.
- B3's exported cross-module type alias (`using X = cc::core::X` reached through `export import`) has NO in-tree precedent; named-module transitive re-export through a second hop (auth.cppm/connection_manager.cppm/config_impl.cpp consuming the aliased complete type) must be compile-verified early in the B3 build. Mitigated: B3 is an isolated green batch with a trivial revert and an explicit alternative (plain import + keep-import markers + direct leaf imports at each consumer).
- Named-module BMI/link surprises: cc_services->cc_config (B3) and cc_server->cc_orchestration PUBLIC (B11/B15) are load-bearing for clang-scan-deps; the B9 link-removal hygiene is deliberately a separate commit because a wrong PUBLIC/PRIVATE choice surfaces as scan-time missing-BMI failures, not plain link errors. Re-grep every target's own module imports before trimming links (the reviews recorded the audit but the tree moves underneath).
- Process-global test state: the MCP loader slot (B4), MCP snapshot sink (B8), image codec/skill executor (B11/B12) and agent registrar (B15) are function-local statics surviving across gtest cases; --gtest-shuffle is off today but every new/edited test must RAII-reset its installed sink (fixture dtor) as specified, or later cases silently merge real-HOME MCP servers and spawn detached connect threads.
- main.cpp/server_routes.cppm are non-module TUs invisible to graph_check — wiring mistakes (missing install, wrong install point) produce SILENT wrong-answer behavior (empty tool pools / unset-codec errors / missing MCP servers) rather than gate or compile failures. The install-order inventory (main:674,1797,1826,1847,1912; server startup pre-accept) must be re-verified against code at execution, and the server-side install must be call_once/one-shot, not per-request (the reviewed plans had a real data race there).
- ctest counts are predicted (1699 -> 1705 after F9 -> 1707 -> 1709 -> final 1709) from gtest_discover_tests auto-discovery; actual additions (F9 +6, F3/8 +2, F10 +2) must be reconciled per commit message, and the F10 unset-codec test was deliberately dropped (cannot observe unset codec after the first process-global install).
- String/shape couplings survive only via byte-exact moves: OAuth token-hash keys {'type','url','headers'} (B3), the 11 LSP action strings and parse_lsp_action mirror (B15 collapses both halves into orchestration), 'Agent' registry/UI/deny-rule literals, mcp__ routing, plugin '<plugin>:<skill>' naming, and image format strings. These are the codebase's documented silent-breakage class; reviewers enumerated them but a moved-body diff should be checked with --word-diff rather than retyped.
- streaming_executor is a genuinely orphaned module (0 importers): B5 sinks its DTO edge as REV3 mandates, but deleting the module outright is graph-equivalent and preferred by CLAUDE.md's dead-code rule; left as an explicit open decision because it changes B5's file manifest.

## Unresolved design decisions (taken at execution, no re-design needed)

- streaming_executor orphan: B5 keeps the module and sinks its DTO edge per REV3; deleting it (0 importers, StreamingToolExecutor/ToolDispatchFn unreferenced) is graph-equivalent and satisfies CLAUDE.md dead-code preference but changes B5's manifest — pick sink-now (chosen) or delete-now before B15.
- Namespace policy: lifted modules keep namespace cc::tools (chosen — zero call-site churn, streaming_executor precedent); a mechanical cc::tools -> cc::orchestration namespace rename is deferred to a post-Phase-B sweep.
- cc.tools.list_mcp_resources_tool.cppm (zero importers, own local McpResource struct) is KEPT in cc_tools during B15 to bound the atomic diff; deletion is an independent follow-up (B11 plan recommended delete, F12 review recommended keep — resolved: keep, then prune).
- F10 unset-codec test dropped (count reconciled to 1709 not 1710): clear_codec + RAII reset exist on the port, but the first guard installs process-globally before the unset case could run; a future isolated-binary test can cover the unset-codec error path.
- cc.tools.runtime_backends.port composition is fixed by this sequencing (single seam, single call_once installer, function-local static slot storage, ServiceToolExecutor set covering computer_use once rather than separate probe+forward slots); the B15 pre-implementation spike (see risks) may adjust signatures but must not introduce a second parallel sink struct or a raw-pointer/stack-address lifetime pattern.
- ConfigManager path drift (hardcoded HOME/.config/loom + cwd/.loom, ignores LOOM_CONFIG_DIR/constants-paths cascade) and the disabled/config_scope/oauth.issuer non-persistence are pre-existing F9 findings explicitly preserved, with new tests pinning the status quo; fixing either is a persisted-shape change outside Phase B.
- Four textually-dead cc.services.api.bootstrap imports in the lifted agent modules and the dead bootstrap/mcp.types/skills imports flagged in dead_imports_baseline are carried verbatim through B15; prune in a dedicated dead-code commit afterward with real clang-22 verification (namespace-attribution false negatives make grep-only deletion unsafe).

## Deferred findings (post-Phase-B tracking)

- **Core `ConfigManager::save()` drops MCP fields** (found during B4, 2026-09-26; pre-existing — present at the parent of B2, NOT introduced by Phase B): `McpServerConfig.{disabled, config_scope}` and `McpOAuthConfig.issuer` have struct fields but no parse/serialize cases in `src/config/config.cppm` (services-layer `config_impl.cpp:101` honors `disabled` independently). Any settings rewrite through the core ConfigManager silently loses these keys. Legacy snake_case/camelCase round-trip IS covered (B4 tests); this gap was explicitly outside Phase B's persisted-shape scope but is real user-data loss and needs its own change with a migration test.
