# C++ Migration Unregistered Modules Decision Register

This register tracks every C++ source file currently reported by
`scripts/cpp-migration-inventory.mjs` as a compilable file that is not registered
in `cpp_migration/src/CMakeLists.txt`.

Last reviewed: 2026-06-12.

## Decisions

| File | Module name | Classification | Replacement or integration target | Decision | Owner area | Notes |
|---|---|---|---|---|---|---|
| `hooks/use_permission.cppm` | `cc.hooks.use_permission` | Historical skeleton | `cc.hooks.tool_permissions` and active permission hooks | Delete | Hooks | Phase 3-S3 skeleton; no active imports. |
| `services/query_engine.cppm` | `cc.services.query_engine` | Superseded prototype | `query/query_engine.cppm` (`cc.query.query_engine`) | Delete | Query | Phase 3-A skeleton surface; no active imports. |
| `state/persistence_json.cppm` | `cc.state.persistence_json` | Superseded prototype | `state/persistence.cppm` | Delete | State | Only used by obsolete standalone `state_smoke.cpp`. |
| `state/store_impl.cppm` | `cc.state.store_impl` | Superseded prototype | `state/store.cppm`, `state/app_state.cppm` | Delete | State | Only used by obsolete standalone `state_smoke.cpp`. |
| `tools/bash/tool_bash.cppm` | `cc.tools.bash_skel` | Historical skeleton | `tools/bash_tool.cppm`, `tools/bash/impl_bash.cppm` | Delete | Tools | Phase 3-S skeleton; no active imports. |
| `tools/bash_comment_label.cppm` | `cc.tools.bash_comment_label` | Orphan helper | `tools/bash_helpers.cppm` | Delete | Tools | Duplicate of active `extract_bash_comment_label` in `bash_helpers.cppm`. |
| `tools/core/tool_base.cppm` | `cc.tools.core` | Historical skeleton | `tools/tool.cppm` | Delete | Tools | Phase 3-S skeleton; only imported by deleted skeletons. |
| `tools/file_edit_constants.cppm` | `cc.tools.file_edit_constants` | Orphan helper | `tools/file_edit_tool.cppm`, `tools/file_edit_types.cppm` | Delete | Tools | Constants already exist in active file edit types/tool modules. |
| `tools/files/tool_io.cppm` | `cc.tools.files` | Historical skeleton | `tools/files/impl_files.cppm` and concrete file tools | Delete | Tools | Phase 3-S skeleton; no active imports. |
| `tools/tools_utils.cppm` | `cc.tools.utils` | Orphan helper | `utils/tool_helpers.cppm` or active tool modules | Delete | Tools | No active imports; helper APIs are unused. |
| `tools/web_fetch_preapproved.cppm` | `cc.tools.web_fetch_preapproved` | Orphan helper | `tools/web_fetch_tool.cppm` | Delete | Tools | No active imports; WebFetch does not currently consume this allowlist. |
| `ui/components/prompt_input_composer.cppm` | `ui.components.prompt_input_composer` | Unused prompt branch | `ui/prompt/prompt_input_full.cppm` and active prompt modules | Delete | UI Prompt | Active prompt modules cover this path; non-`cc.*` composer was never wired. |
| `ui/dialogs/diff_dialog.cppm` | `cc.ui.dialogs.diff_dialog` | Registered UI module | `ui/screens/repl_screen.cppm` dialog routing | Registered; route later | UI Dialogs | Added to `cc_ui` and fixed FTXUI API drift; still needs runtime route wiring. |
| `ui/dialogs/install_github_app_wizard.cppm` | `cc.ui.dialogs.install_github_app_wizard` | Intentional deferral | `ui/screens/repl_screen.cppm`, `ui/wizard_dialog` | Defer | UI Dialogs | Compile probe confirms old WizardProviderProps/WizardComponent and old FTXUI APIs; needs wizard API migration. |
| `ui/dialogs/install_slack_app_wizard.cppm` | `cc.ui.dialogs.install_slack_app_wizard` | Intentional deferral | `ui/screens/repl_screen.cppm`, `ui/wizard_dialog` | Defer | UI Dialogs | Compile probe confirms old WizardProviderProps/WizardComponent API; needs wizard API migration. |
| `ui/mcp/mcp_add_server_wizard.cppm` | `cc.ui.mcp.mcp_add_server_wizard` | Registered UI module | Active MCP dialogs and settings panels | Registered; route later | UI MCP | Added to `cc_ui`; fixed expected include, old decorators, mask chars, and TabReverse API. |
| `ui/mcp/mcp_security_dialog.cppm` | `cc.ui.mcp.mcp_security_dialog` | Registered UI module | Active MCP dialogs and permission UI | Registered; route later | UI MCP | Added to `cc_ui`; fixed countdown lambda capture. |
| `ui/mcp/mcp_server_details.cppm` | `cc.ui.mcp.mcp_server_details` | Registered UI module | Active MCP dialogs and `mcp_server_list` | Registered; route later | UI MCP | Added to `cc_ui`; imports server list helper and builds. |
| `ui/mcp/mcp_server_list.cppm` | `cc.ui.mcp.mcp_server_list` | Registered UI module | Active MCP dialogs | Registered; route later | UI MCP | Added to `cc_ui`; fixed decorator and lambda capture drift. |
| `ui/permissions/permission_batch_panel.cppm` | `cc.ui.permissions.batch_panel` | Registered UI module | Active permission request/views | Registered; route later | UI Permissions | Added to `cc_ui`; depends on registered single prompt. |
| `ui/permissions/permission_single_prompt.cppm` | `cc.ui.permissions.single_prompt` | Registered UI module | Active permission request/views | Registered; route later | UI Permissions | Added to `cc_ui`; fixed TabReverse API drift. |
| `ui/permissions/sandbox_config_dialog.cppm` | `cc.ui.permissions.sandbox_config` | Registered UI module | `ui/dialogs/sandbox_dialog.cppm` | Registered; route later | UI Permissions | Added to `cc_ui`; fixed TabReverse and Backspace/Delete handling. |
| `ui/plugins/plugin_install_flow.cppm` | `cc.ui.plugins.plugin_install_flow` | Intentional deferral | `ui/dialogs/plugin_dialog.cppm`, `ui/wizard_dialog` | Defer | UI Plugins | Compile probe shows old WizardProviderProps/WizardComponent API; needs wizard API migration. |
| `ui/plugins/plugin_manage_panel.cppm` | `cc.ui.plugins.plugin_manage_panel` | Registered UI module | `ui/dialogs/plugin_dialog.cppm` | Registered and routed | UI Plugins | Added to `cc_ui` and mounted from `plugin_dialog`. |
| `ui/plugins/plugin_marketplace_browse.cppm` | `cc.ui.plugins.plugin_marketplace_browse` | Registered UI module | `ui/dialogs/plugin_dialog.cppm` | Registered and routed | UI Plugins | Added to `cc_ui`, fixed Color constexpr/padding drift, and mounted from `plugin_dialog`. |
| `ui/plugins/plugin_settings_dialog.cppm` | `cc.ui.plugins.plugin_settings_dialog` | Registered UI module | `ui/dialogs/plugin_dialog.cppm` | Registered and routed | UI Plugins | Added to `cc_ui` and mounted from `plugin_dialog`. |
| `ui/prompt/prompt_footer.cppm` | `cc.ui.prompt.prompt_footer` | Unused prompt branch | `ui/prompt/footer.cppm`, `ui/prompt/prompt_input_full.cppm` | Delete | UI Prompt | Only imported by deleted composer; active footer modules remain. |
| `ui/prompt/prompt_widgets.cppm` | `cc.ui.prompt.prompt_widgets` | Unused prompt branch | `ui/prompt/notifications.cppm`, `ui/prompt/prompt_help_menu.cppm`, `ui/prompt/prompt_queued_commands.cppm`, `ui/prompt/voice_indicator.cppm` | Delete | UI Prompt | Only imported by deleted composer; active prompt widget modules remain. |
| `ui/prompt/suggestion_dropdown.cppm` | `cc.ui.prompt.suggestion_dropdown` | Unused prompt branch | `ui/prompt/autocomplete.cppm` | Delete | UI Prompt | Only imported by deleted composer; active autocomplete module remains. |
| `ui/tasks/task_components.cppm` | `cc.ui.tasks.task_components` | Registered UI module | Active task UI modules | Registered; route later | UI Tasks | Added to `cc_ui`; fixed Color and Unicode string drift. |
| `ui/tasks/task_details_dialog.cppm` | `cc.ui.tasks.task_details_dialog` | Registered UI module | `ui/tasks/task_detail_dialog.cppm` | Registered; route later | UI Tasks | Added to `cc_ui`; fixed old FTXUI includes and decorator composition. |
| `ui/tasks/task_list_view.cppm` | `cc.ui.tasks.task_list_view` | Registered UI module | `ui/tasks/task_list_ui.cppm` | Registered; route later | UI Tasks | Added to `cc_ui`; fixed old FTXUI includes and decorator composition. |
| `ui/tasks/task_wizard.cppm` | `cc.ui.tasks.task_wizard` | Intentional deferral | Active task UI route and `ui/wizard_dialog` | Defer | UI Tasks | Compile probe shows old WizardProviderProps/WizardComponent API; needs wizard API migration. |
| `ui/teams/swarm_collaboration_view.cppm` | `cc.ui.teams.swarm_collaboration_view` | Registered UI module | `ui/screens/repl_screen.cppm`, `ui/teams/team_status.cppm` | Registered; route later | UI Teams | Added to `cc_ui`; fixed invalid pseudo-import and helper references. |
| `ui/teams/team_details_dialog.cppm` | `cc.ui.teams.team_details_dialog` | Registered UI module | `ui/screens/repl_screen.cppm`, `ui/teams/team_status.cppm` | Registered; route later | UI Teams | Added to `cc_ui`; fixed helper references and role enum drift. |
| `ui/teams/teams_overview.cppm` | `cc.ui.teams.teams_overview` | Registered UI module | `ui/screens/repl_screen.cppm`, `ui/teams/team_status.cppm` | Registered; route later | UI Teams | Added to `cc_ui`; removed invalid permission StatusDot alias. |

## Review Commands

List current unregistered files:

```bash
node scripts/cpp-migration-inventory.mjs --json \
  | node -e "let s='';process.stdin.on('data',d=>s+=d);process.stdin.on('end',()=>{const r=JSON.parse(s); for (const f of r.cmake.unregisteredCompilableFiles) console.log(f)})"
```

Check imports for historical skeletons:

```bash
rg -n "import cc\\.hooks\\.use_permission|import cc\\.services\\.query_engine|import cc\\.state\\.store_impl|import cc\\.state\\.persistence_json|import cc\\.tools\\.bash_skel|import cc\\.tools\\.core|import cc\\.tools\\.files\\b" cpp_migration/src cpp_migration/tests
```
