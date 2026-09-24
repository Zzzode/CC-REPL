# OQ-4 — Phase C baselines and Phase D `cc.utils` mapping

Attached 2026-09-24. First-pass classification from module name,
current subdirectory and first descriptive comment. Items marked
REVIEW need an in-file content read before Phase D moves them.
Module NAMES are not changed (path-decoupled) — destinations are
the target library/namespace; CMake moves only unless noted.

## Phase C — measured baselines (55 interfaces >= 1000 LOC)

- Total inline-definition heuristic across the 55: **8950**
- C1 top-6 (agent_runtime 498, agent.utils 439, query_engine 433,
  repl_screen 321, messages_list 321, runtime_registry 316):
  sum = 2328. C1 exit: each < 30 inline (trivial accessors only).
- C2 remaining 49: C2 exit: none > 100 inline defs.
- C3 lint threshold: set after C2 from the post-conversion
  distribution (proposal: warn at 40, error at 80 inline defs per interface).

| Inline defs (heuristic) | LOC | Module |
|---:|---:|---|
| 498 | 3971 | cc.tools.agent_runtime |
| 439 | 3062 | cc.tools.agent.utils |
| 433 | 3383 | cc.query.query_engine |
| 321 | 4115 | cc.ui.screens.repl_screen |
| 321 | 3731 | cc.ui.messages.messages_list |
| 316 | 2746 | cc.tools.runtime_registry |
| 282 | 2470 | cc.ui.widgets.text_input |
| 248 | 1790 | cc.services.mcp.client |
| 236 | 3016 | cc.ui.permissions.rule_list |
| 228 | 2319 | cc.skills.load_skills_dir |
| 209 | 1555 | cc.daemon.daemon_server |
| 195 | 1637 | cc.tools.mcp |
| 179 | 1930 | cc.services.lsp.client |
| 176 | 1757 | cc.ui.screens.log_selector |
| 172 | 1798 | cc.ui.visual.markdown |
| 169 | 1348 | cc.utils.powershell_parser |
| 154 | 1856 | cc.utils.swarm_backends |
| 154 | 1121 | cc.ui.widgets.text_input_widget |
| 151 | 1367 | cc.utils.plugin_loader |
| 149 | 1189 | cc.commands.mcp_cmd |
| 148 | 1272 | cc.tools.bash |
| 147 | 1779 | cc.ui.screens.resume_screen |
| 143 | 1265 | cc.server.server_routes |
| 141 | 1054 | cc.utils.ide_integration |
| 141 | 1043 | cc.server.server_main |
| 136 | 1505 | cc.bridge.core |
| 136 | 1422 | cc.tools.agent |
| 133 | 1305 | cc.state.store |
| 131 | 1375 | cc.ui.dialogs.mcp_dialogs |
| 130 | 1884 | cc.ui.prompt.prompt_input_footer |
| 127 | 1480 | cc.ui.dialogs.feature_dialogs |
| 127 | 1264 | cc.ui.permissions.advanced_prompts |
| 125 | 1384 | cc.ui.screens.doctor_screen |
| 123 | 1140 | cc.utils.file_edit |
| 122 | 1353 | cc.tools.path_validation |
| 122 | 1188 | cc.utils.hooks_execution |
| 117 | 1264 | cc.ui.dialogs.wizard_dialog |
| 114 | 1257 | cc.hooks.voice_hooks |
| 112 | 1136 | cc.ui.features.tasks.task_list_view |
| 111 | 1389 | cc.services.mcp.channel_notification |
| 110 | 1426 | cc.ui.dialogs.system |
| 110 | 1129 | cc.services.api.client |
| 107 | 1117 | cc.ui.widgets.custom_select |
| 105 | 1008 | cc.utils.team_helpers |
| 101 | 1194 | cc.ui.messages.messages_interactions |
| 100 | 1122 | cc.ui.visual.code_highlight |
| 97 | 1022 | cc.ui.dialogs.sandbox_settings |
| 95 | 1249 | cc.ui.messages.virtual_list |
| 94 | 1844 | cc.ui.foundation.logo_v2 |
| 85 | 1005 | cc.services.prompt_suggestion |
| 84 | 1268 | cc.utils.swarm_helpers |
| 83 | 1189 | cc.ui.app.app |
| 78 | 1542 | cc.ui.dialogs.settings_dialog |
| 70 | 1081 | cc.tools.readonly_validation |
| 15 | 1298 | cc.ui.foundation.design_tokens |

## Phase D — proposed destinations

### cc.agent.id (1)
`agent_id`

### cc.cache (2)
`cache`, `cache_paths`

### cc.config (1)
`config_utils`

### cc.config.settings (6)
`settings_manager`, `settings_merge`, `settings_paths`, `settings_rules`, `settings_sources`, `settings_validation`

### cc.containers (4)
`array_utils`, `circular_buffer`, `object_group_by`, `set_utils`

### cc.crypto (3)
`crypto`, `hash`, `uuid_utils`

### cc.diagnostics (5)
`activity_manager`, `debug`, `debug_filter`, `fps_tracker`, `log`

### cc.error (LEAF - stays) (2)
`error`, `errors_utils`

### cc.fs (13)
`cwd`, `file`, `file_history`, `file_persistence`, `file_read_cache`, `fs_operations`, `glob_utils`, `lockfile`, `memory_file_detection`, `path`, `path_utils`, `read_file_in_range`, `tempfile`

### cc.fs.edit (1)
`file_edit`

### cc.fs.search (1)
`file_index`

### cc.hooks.config (3)
`hooks_config`, `hooks_execution`, `hooks_registry`

### cc.mcp.support (2)
`mcp_helpers`, `mcp_validation`

### cc.mcp.transport (1)
`mcp_transport`

### cc.messages.support (4)
`collapse_notifications`, `collapse_read_search`, `message_mappers`, `message_predicates`

### cc.model (13)
`agent_model`, `effort`, `model.ant_models`, `model.configs`, `model.model`, `model.model_capabilities`, `model.model_support_overrides`, `model.providers`, `model_aliases`, `model_cost`, `thinking`, `token_budget`, `tokens`

### cc.model.prompt (1)
`system_prompt`

### cc.net.http (6)
`github_utils`, `http`, `http_encoding`, `peer_address`, `proxy_utils`, `ssrf_guard`

### cc.parsing.cli (2)
`argument_substitution`, `slash_command_parsing`

### cc.parsing.highlight (1)
`text_highlighting`

### cc.parsing.shell (1)
`powershell_parser`

### cc.parsing.tree_sitter (2)
`tree_sitter.base`, `tree_sitter.bash`

### cc.platform (6)
`binary_check`, `clipboard`, `find_executable`, `platform`, `platform_paths`, `xdg`

### cc.platform.env (4)
`env`, `env_dynamic`, `env_utils`, `env_validation`

### cc.platform.installer (1)
`native_installer`

### cc.platform.terminal (2)
`hyperlink`, `terminal_helpers`

### cc.platform.user (1)
`user_utils`

### cc.plugins (10)
`plugin_dependency_resolver`, `plugin_identifier`, `plugin_lifecycle`, `plugin_loader`, `plugin_manager`, `plugin_marketplace`, `plugin_marketplace_lifecycle`, `plugin_marketplace_rules`, `plugin_validation`, `plugin_versioning`

### cc.process (5)
`abort_controller`, `exec_file`, `exec_sync`, `process`, `timeouts`

### cc.process.async (1)
`async`

### cc.process.bash (3)
`bash_execution`, `bash_security`, `bash_shell_quoting`

### cc.process.editor (1)
`editor_utils`

### cc.process.shell (4)
`shell`, `shell_parser`, `shell_providers`, `shell_rule_matching`

### cc.scm.git (7)
`commit_attribution`, `detect_repository`, `get_worktree_paths`, `git`, `git_diff`, `git_filesystem`, `gitignore`

### cc.search.indexing (1)
`code_indexing`

### cc.security (2)
`privacy_level`, `query_guard`

### cc.security.permissions (4)
`auto_mode_denials`, `permissions`, `permissions_engine`, `tool_deny_rules`

### cc.security.sanitize (1)
`sanitization`

### cc.serdes.frontmatter (1)
`frontmatter_parser`

### cc.serdes.json (1)
`json`

### cc.serdes.yaml (1)
`yaml`

### cc.services.ide (MOVE OUT per OQ-3) (1)
`ide_integration`

### cc.session (4)
`list_sessions`, `session_helpers`, `session_restore`, `session_storage`

### cc.skills.hints (1)
`loom_code_hints`

### cc.skills.support (1)
`skill_usage`

### cc.tasks.output (1)
`task_output`

### cc.tasks.plans (1)
`plans`

### cc.tasks.support (1)
`task_utils`

### cc.teams (3)
`agent_swarms_enabled`, `control_message_compat`, `team_helpers`

### cc.teams.swarm (5)
`swarm`, `swarm_backends`, `swarm_coordination`, `swarm_helpers`, `swarm_pane_observer`

### cc.text (6)
`parse_references`, `semantic_boolean`, `semantic_number`, `string`, `string_utils`, `words`

### cc.text.diff (1)
`diff_utils`

### cc.text.format (1)
`format`

### cc.text.markdown (1)
`markdown_utils`

### cc.text.parse (LEAF) (1)
`parse_int`

### cc.text.stats (1)
`stats_utils`

### cc.tools.support (3)
`script_tool_enabled`, `tool_helpers`, `tool_management`

### cc.types (LEAF) (1)
`tagged_id`

### cc.types.wire (1)
`content_array`

### cc.ui.statusline (MOVE to ui?) (1)
`statusline_runner`

### Manual REVIEW resolutions (2026-09-24)

- `image_store` -> **cc.media.images** (plain data registry; only importer is `ui/messages/message_image`).
- `pdf` -> **cc.media.pdf** leaf (zero importers; retained capability, not a Phase D deletion).
- `prompt_category` -> **deletion candidate** (no source importer anywhere; verify then delete, else `cc.prompt.support`).
- `system_theme` -> **cc.platform.terminal** (plain enums).
- `theme` -> **cc.ui.theme.types** leaf (`struct Theme` data, no FTXUI dependency).

## REVIEW — resolved (content-read 2026-09-24)

| Module | Current path | First description |
|---|---|---|
| `cc.utils.image_store` | image_store.cppm | Read first few bytes for MIME detection |
| `cc.utils.pdf` | pdf.cppm |  |
| `cc.utils.prompt_category` | prompt_category.cppm | registered in CMake but **no source importer at all** -> deletion candidate; confirm in Phase D, otherwise **cc.prompt.support** |
| `cc.utils.system_theme` | system_theme.cppm | ─── System Theme Type ────────────────────────────────────── |
| `cc.utils.theme` | theme.cppm | plain `struct Theme` data carrier, no ftxui import -> **cc.ui.theme.types** leaf (data only; renderer stays in ui.foundation) |
