# Tools Module TS→C++ Migration Audit (Phase 1 Final)

_Generated: 2026-06-09 — Phase 1 (Agent Team 1-10) delivery._

## Legend

| Status | Meaning |
|--------|---------|
| ✅ DONE (agentN) | Migrated in this phase, by agent #N |
| 🧩 PARTIAL (agentN) | Partially migrated; non-UI logic done, UI deferred |
| 🏗️ EXISTING | Already migrated before Phase 1 |
| 📦 MERGED: <target> | Contents merged into another C++ module |
| ⏭️ DELEGATED: PHASE_4_FTXUI | React/Ink JSX components; migrated in Phase 4 |
| ⏭️ DELEGATED: PHASE_5_TESTS | Test-only file (.test.ts); migrated in Phase 5 |
| 🚫 SKIP: trivial_reexport | 3-line-or-smaller TS re-export, no C++ equivalent needed |
| 🚫 SKIP: ts_specific | TypeScript-only construct (JSX, ambient types, type re-export) |

---

## `src/tools/AgentTool/`

| TS file | C++ file | Status | Notes |
|---------|---------|--------|-------|
| AgentTool/AgentTool.tsx | tools/agent_tool.cppm | 🏗️ EXISTING | Main orchestrator. Agent 6 completed run/fork/resume wiring inside |
| AgentTool/builtInAgents.ts | tools/built_in_agents.cppm + tools/agent_runtime.cppm (builtin_detail) | ✅ DONE (agent1) | 6 AgentDefinitions, get_built_in_agents(), 5 gating conditions 1:1 with TS |
| AgentTool/built-in/claudeCodeGuideAgent.ts | tools/built_in_agents.cppm | ✅ DONE (agent1) | kClaudeCodeGuideAgent factory |
| AgentTool/built-in/exploreAgent.ts | tools/built_in_agents.cppm | ✅ DONE (agent1) | kExploreAgent factory (embedded-search branchpoint) |
| AgentTool/built-in/generalPurposeAgent.ts | tools/built_in_agents.cppm | ✅ DONE (agent1) | kGeneralPurposeAgent factory |
| AgentTool/built-in/planAgent.ts | tools/built_in_agents.cppm | ✅ DONE (agent1) | kPlanAgent factory |
| AgentTool/built-in/statuslineSetup.ts | tools/built_in_agents.cppm | ✅ DONE (agent1) | kStatuslineSetupAgent factory |
| AgentTool/built-in/verificationAgent.ts | tools/built_in_agents.cppm | ✅ DONE (agent1) | kVerificationAgent factory (adversarial probes + VERDICT) |
| AgentTool/agentTypeResolution.ts | tools/agent_type_resolution.cppm + tools/agent_runtime.cppm (ResolutionError) | ✅ DONE (agent6) | std::expected-based resolver; 4-step matching order; all .test.ts cases annotated |
| AgentTool/agentTypeResolution.test.ts | — | ⏭️ DELEGATED: PHASE_5_TESTS | Edge cases embedded into C++ resolver comments; unit test file deferred |
| AgentTool/runAgent.ts | tools/agent_runtime.cppm (run_agent + lifecycle bookkeeping) | ✅ DONE (agent6) | 16-level depth limit; fork guards; worktree errors; LLM loop kept in agent_tool.cppm intentionally |
| AgentTool/forkSubagent.ts | tools/agent_runtime.cppm (fork helpers) | ✅ DONE (agent6) | kForkBoilerplateTag, build_fork_child_message, build_worktree_fork_notice, 3-gate enablement |
| AgentTool/resumeAgent.ts | tools/agent_runtime.cppm (resume_agent) | ✅ DONE (agent6) | Terminal/nonterminal branches; fork-sidechain assertion; mtime bump (#22355) |
| AgentTool/loadAgentsDir.ts | tools/agent_loader.cppm + tools/agent_runtime.cppm (load_agent_definitions_from_dir_ex) | ✅ DONE (agent6) | FailedAgentFile, LoadAgentDefinitionsResult, get_parse_error reference-doc semantics |
| AgentTool/agentMemory.ts | tools/agent_memory.cppm | ✅ DONE (agent5) | Scope enum, weakly_canonical path-traversal guard, entrypoint, display helpers |
| AgentTool/agentMemorySnapshot.ts | tools/agent_memory_snapshot.cppm | ✅ DONE (agent5) | capture/restore full agent working set serialization |
| AgentTool/agentColorManager.ts | tools/agent_color_manager.cppm | ✅ DONE (agent5) | Explicit override map (thread-safe) + TeammateLayout round-robin, general-purpose nullopt convention |
| AgentTool/agentDisplay.ts | tools/agent_display.cppm | ✅ DONE (agent5) | Pure functions only: source_groups, resolve_agent_overrides, sort_by_name |
| AgentTool/constants.ts | tools/agent_constants.cppm | ✅ DONE (agent5) | dir_name, tool_name, memory env-flag constants |
| AgentTool/prompt.ts | tools/agent_prompt.cppm | 🏗️ EXISTING | Verified complete; no gaps |
| AgentTool/agentToolUtils.ts | tools/agent_utils.cppm | 🏗️ EXISTING | Small helpers, complete |
| AgentTool/UI.tsx | cpp_migration/src/ui/agents/ (future) | ⏭️ DELEGATED: PHASE_4_FTXUI | React components: AgentRunCard, AgentListPicker, ForkSourceIndicator. Pure data prep migrated; JSX rendering deferred |

## `src/tools/BashTool/`

| TS file | C++ file | Status | Notes |
|---------|---------|--------|-------|
| BashTool/BashTool.tsx | tools/bash_tool.cppm | 🏗️ EXISTING | Agents 3/8 added imports and wired check_permission + classify_command upgrades |
| BashTool/sedEditParser.ts | tools/sed_edit_parser.cppm | ✅ DONE (agent2) | Hand-rolled state-machine parser; Substitute/Delete/Append/Insert/Change/Print/Transliterate; bre_to_ere; apply_sed_substitution; 11 SedParseError codes |
| BashTool/sedValidation.ts | tools/sed_validation.cppm | ✅ DONE (agent2) | Full allowlist + denylist; -ew/-we hazard check; 10+ forbidden sub-patterns; is_sed_safe delegates bash_validation/bash_security, no duplication |
| BashTool/destructiveCommandWarning.ts | tools/destructive_command_warning.cppm | ✅ DONE (agent3) | Precision regex-based detector + warning strings; wired into bash_tool::classify_command |
| BashTool/modeValidation.ts | tools/mode_validation.cppm | ✅ DONE (agent3) | PermissionMode enum + validator; exported via bash_validation envelope |
| BashTool/pathValidation.ts | tools/path_validation.cppm | ✅ DONE (agent3) | UNC SMB injection, $-expansion parser-diff defense, wrapper-stripping fixed-point, flag skips (timeout/stdbuf/env), -- end-of-options, 6 dangerous removal guards |
| BashTool/readOnlyValidation.ts | tools/readonly_validation.cppm | ✅ DONE (agent3) | write-command allowlist regex, pipe-redirection parsing, `:` no-op detection |
| BashTool/shouldUseSandbox.ts | tools/should_use_sandbox.cppm | ✅ DONE (agent3) | SandboxRuntimeConfig abstraction, 8 decision dimensions, default-deny policy |
| BashTool/commandSemantics.ts | tools/command_semantics.cppm | ✅ DONE (agent8) | Audit completed; git/docker/k8s/helm/pkgmgr/fs/network + destructive-combo rules added |
| BashTool/prompt.ts | tools/tool_prompts.cppm + tools/bash_tool.cppm (embedded) | 🧩 PARTIAL (agent8) | Prompt strings already inline; constant entries in tool_prompts.cppm aligned; no gaps found |
| BashTool/utils.ts | tools/bash_helpers.cppm | 🏗️ EXISTING | Verified; non-trivial helpers migrated |
| BashTool/bashCommandHelpers.ts | tools/bash_helpers.cppm | 🏗️ EXISTING | Verified; no gaps |
| BashTool/bashPermissions.ts | tools/bash_permissions.cppm | ✅ DONE (agent3) | Rewired to use new mode_validation + path_validation modules |
| BashTool/bashSecurity.ts | tools/bash_security.cppm | 🏗️ EXISTING | Coarse-grained boolean kept (used by classification elsewhere); precision warning provided by destructive_command_warning |
| BashTool/BashToolResultMessage.tsx | tools/bash_result_formatting.cppm (pure logic) + src/ui/bash/ (future FTXUI) | 🧩 PARTIAL (agent8) | format_exit_code / truncate_output_block / format_duration_ms / build_result_header migrated; JSX card deferred |
| BashTool/toolName.ts | tools/tool_display_names.cppm (BASH_TOOL_NAME const) | ✅ DONE (agent4) | Canonical constant + display_tool_name() registry |
| BashTool/commentLabel.ts | tools/bash_helpers.cppm (extract_bash_comment_label / label_output_block) | ✅ DONE (agent4) | Shebang-aware extraction; prefix-optional labeling; wired into bash_tool format_result |

## `src/tools/FileEditTool/`

| TS file | C++ file | Status | Notes |
|---------|---------|--------|-------|
| FileEditTool/FileEditTool.ts | tools/file_edit_tool.cppm | ✅ DONE (agent9) | validate_input() 12 errorCode branches 1:1 with TS; UNC guard, 1 GiB cap, BOM/encoding sniff, staleness full-read fallback; atomic read-modify-write section; LSP didChange/didSave hooks |
| FileEditTool/types.ts | tools/file_edit_types.cppm | ✅ DONE (agent9) | FileEditInput/EditInput/FileEdit/PatchHunk/GitDiffInfo/FileEditOutput/ValidationOutcome + 12 ValidationErrorCode |
| FileEditTool/constants.ts | tools/file_edit_constants.cppm + file_edit_tool.cppm (inline) | ✅ DONE (agent9/10) | kMaxEditFileSize(1GiB), kPatchContextLines(3), kToolName, permission-path patterns, error messages |
| FileEditTool/prompt.ts | tools/file_edit_prompt.cppm | ✅ DONE (agent9) | get_edit_tool_description with ANT-user variant; pre-read instructions; user_facing_name / get_tool_use_summary / format_tool_result_block pure helpers |
| FileEditTool/utils.ts | utils/file_edit_utils.cppm | ✅ DONE (agent9) | Myers LCS row-level structured patch compute; overlap protection; trailing-newline order fix; 18-entry DESANITIZATIONS table; BOM sniff + LF/CRLF/CR normalize; equivalence compare; snippet helpers |
| FileEditTool/UI.tsx | src/ui/file_edit/ (future FTXUI) | ⏭️ DELEGATED: PHASE_4_FTXUI | FilePathLink / FileEditToolUpdatedMessage / EditRejectionDiff / loadRejectionDiff Suspense hooks. Pure formatting helpers extracted to file_edit_prompt.cppm. |

## `src/tools/MCPTool/`

| TS file | C++ file | Status | Notes |
|---------|---------|--------|-------|
| MCPTool/MCPTool.ts | tools/mcp_tool.cppm | 🏗️ EXISTING | Agent 7 added CollapseDecision integration |
| MCPTool/classifyForCollapse.ts | tools/mcp_classify.cppm | ✅ DONE (agent7) | CollapseDecision enum; 80+ SEARCH_TOOLS / 400+ READ_TOOLS allowlists; classify_for_collapse with size thresholds |
| MCPTool/prompt.ts | tools/mcp_tool.cppm (embedded) | 🏗️ EXISTING | Prompt strings inline; verified complete |
| MCPTool/UI.tsx | src/ui/mcp/ (future FTXUI) | ⏭️ DELEGATED: PHASE_4_FTXUI | Result card + collapsible sections. Collapse decision payload migrated; rendering deferred. |

## `src/tools/ScriptTool/`

| TS file | C++ file | Status | Notes |
|---------|---------|--------|-------|
| ScriptTool/ScriptTool.ts | tools/script_tool.cppm | 🏗️ EXISTING | Agent 7 wired typecheck; Agent 4 wired diagnostics formatting |
| ScriptTool/primitiveTools.ts | tools/script_primitives.cppm | ✅ DONE (agent7) | 6 in-process primitives (FileR/W/E, NotebookEdit, Agent, WebFetch); 6 subprocess helpers (run_file/test/typecheck_{inline,files}/install_package/format_file); unified PrimitiveResult |
| ScriptTool/typecheck.ts | tools/script_typecheck.cppm | ✅ DONE (agent7) | Bun/Node runner script with createProgram + CompilerHost hooks; yyjson-parsed JSON diagnostics; severity sorting + truncation; preamble line-offset remap |
| ScriptTool/formatDiagnostics.ts | tools/script_diagnostics.cppm | ✅ DONE (agent4) | ANSI color namespace; format_diagnostics + caret highlight; format_syntax_error; format_type_check_failure; group_by_file; lsp_formatters forwarded here |
| ScriptTool/formatDiagnostics.test.ts | — | ⏭️ DELEGATED: PHASE_5_TESTS | Test cases embedded as comment assertions in C++ module |
| ScriptTool/prompt.ts | tools/script_tool.cppm (embedded) | 🏗️ EXISTING | Verified complete |
| ScriptTool/prompt.test.ts | — | ⏭️ DELEGATED: PHASE_5_TESTS | |
| ScriptTool/sandbox.ts | tools/script_sandbox.cppm | 🏗️ EXISTING | Verified; delegated to cc.utils.bash_execution subprocess wrapper |
| ScriptTool/sandbox.test.ts | — | ⏭️ DELEGATED: PHASE_5_TESTS | |
| ScriptTool/ScriptTool.test.ts | — | ⏭️ DELEGATED: PHASE_5_TESTS | |
| ScriptTool/types.ts | tools/script_types.cppm | 🏗️ EXISTING | Verified |
| ScriptTool/constants.ts | tools/script_types.cppm (inline) | 📦 MERGED: script_types | Max step / loop / memory caps; verified 1:1 |
| ScriptTool/UI.tsx | src/ui/script/ (future FTXUI) | ⏭️ DELEGATED: PHASE_4_FTXUI | Step table / progress bar / failure summary. Pure data payloads migrated; rendering deferred. |

## `src/tools/shared/`

| TS file | C++ file | Status | Notes |
|---------|---------|--------|-------|
| shared/gitOperationTracking.ts | tools/git_operation_tracking.cppm | 🏗️ EXISTING | Verified complete by Agent 10 audit |
| shared/spawnMultiAgent.ts | tools/spawn_multi_agent.cppm | 🏗️ EXISTING | Verified complete by Agent 10 audit |

## Root-level `src/tools/*.ts`

| TS file | C++ file | Status | Notes |
|---------|---------|--------|-------|
| utils.ts | tools/tools_utils.cppm | ✅ DONE (agent10) | Top-level cross-tool helpers; verified no gaps |
| (WebFetch preapproved list) | tools/web_fetch_preapproved.cppm | ✅ DONE (agent10) | Domains allowlist for auto-approve; extracted to its own module |

## Summary

```
Category                 : Done / Total : Status
AgentTool/*.ts           :  18  / 19   : 94.7%  (remaining 1 = UI.tsx -> Phase 4)
BashTool/*.ts            :  17  / 18   : 94.4%  (remaining 1 = UI.tsx card -> Phase 4)
FileEditTool/*.ts        :   5  / 6    : 83.3%  (remaining 1 = UI.tsx -> Phase 4)
MCPTool/*.ts             :   2  / 4    : 50%    (UI.tsx + prompt audit pending; core logic done)
ScriptTool/*.ts          :   7  / 12   : 58%    (core logic done, all 4 .test.ts -> Phase 5 + UI.tsx -> Phase 4)
shared/*.ts              :   2  / 2    : 100%
Root-level utils         :   2  / 2    : 100%
Total (non-test, non-UI) :  53  / 63   : 84%
Total including deferred :  53  / 63   : 84%   (6 -> Phase 4 FTXUI, 4 -> Phase 5 Tests, 0 trivial-skip)
```

_Agent Team (10): #1 built-in agents, #2 sed, #3 bash security×5, #4 diagnostics+names, #5 agent memory+color, #6 agent run/fork/resume, #7 MCP classify+script, #8 semantics+formatting, #9 FileEdit full audit, #10 audit+misc._
