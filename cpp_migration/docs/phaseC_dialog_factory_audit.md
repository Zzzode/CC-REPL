# Phase C-a: Dialog Factory Audit

## Scope

Audit **all 75 `ReplMode` enum values** in `cpp_migration/src/ui/screens/repl_screen.cppm` (lines 124–199)
against:
1. Actual `Component`-returning factory exports in each referenced UI module.
2. The wiring in `dialog_stubs::get_builder()` dispatch table (lines 588–1047).

Panel modes that render inline (return `nullptr` from `RouteDialog`) are tracked separately.

---

## Summary Matrix

| # | ReplMode | Dispatch entry L# | Stub kind | Module ref | Factory symbol | Status | Notes |
|---|---|---:|---|---|---|:---:|---|
| 1 | `Normal` | — | inline panel | — | — | ✅ | Base REPL layout |
| 2 | `TasksView` | — | inline panel | `ui/tasks/task_list_view.cppm` | `TaskListView()` | ✅ | Rendered inline; has export |
| 3 | `TeamsView` | — | inline panel | `ui/teams/teams_overview.cppm` | `MakeTeamsOverview()` | ✅ | Rendered inline; export present |
| 4 | `SettingsView` | 736 | real | `ui/dialogs/settings_dialog.cppm` | `MakeSettingsDialog()` | ✅ | L929, 10-arg |
| 5 | `HelpView` | 1038–1040 | **thin adapter** | `ui/dialogs/help_v2.cppm` | `HelpDialog()` | 🔴 MISSING | Help module only exports `render_help_dialog()` (string-based), **no FTXUI Component export**. Dispatcher calls a nonexistent `help_v2::HelpDialog()`. |
| 6 | `QuickOpen` | 1041–1045 | **thin adapter** | `ui/dialogs/quick_open.cppm` | `QuickOpenDialog()` | 🔴 MISSING | Module exports only `render_quick_open()` (string-based) + fuzzy filter helpers. **No FTXUI Component**. |
| 7 | `AgentsView` | 886 | real | `ui/agents/agent_list.cppm` | `AgentListComponent()` | ✅ | L413 |
| 8 | `MessageSelector` | 593 | **STUB** | — | `StubDialog("Message Selector")` | 🟡 | Comment says "TODO UI25 — wire to `cc.ui.messages.messages_interactions`". No module yet. |
| 9 | `SandboxPermission` | 603 | real | `ui/dialogs/sandbox_dialog.cppm` | `SandboxDialog()` | ✅ | L331/L413 |
| 10 | `ToolPermission` | 617 | real | `ui/permissions/permission_single_prompt.cppm` | `MakeSinglePromptDialog()` | ✅ | L427 |
| 11 | `PromptHook` | 632 | **STUB** | — | `StubDialog("Hook Prompt")` | 🟡 | "TODO UI2 — wire to `cc.ui.prompt.prompt_input_full HookPrompt`" |
| 12 | `WorkerSandboxPermission` | 608 | real | `ui/dialogs/sandbox_dialog.cppm` | `SandboxDialog()` | ✅ | Same factory as #9, separate props init — fine. |
| 13 | `Elicitation` | 640 | real | `ui/mcp/mcp_security_dialog.cppm` | `McpSecurityDialog()` | ✅ | L331 |
| 14 | `CostThreshold` | 655 | **wrapper** | `ui/dialogs/cost_threshold_dialog.cppm` | `render_cost_threshold()` (Element) | ✅* | Module exports Element-only; dispatcher wraps in `Renderer()`. Builds but has no interactive y/n wiring. |
| 15 | `IdleReturn` | 667 | real (reuse) | `ui/screens/resume_screen.cppm` | `ResumeScreen()` | ✅ | L1288 |
| 16 | `UltraplanChoice` | 679 | **STUB** | — | `StubDialog()` | 🟡 | "TODO UI11 — wire to `cc.ui.dialogs.onboarding`" |
| 17 | `UltraplanLaunch` | 683 | **STUB** | — | `StubDialog()` | 🟡 | "TODO UI11" |
| 18 | `IdeOnboarding` | 687 | real | `ui/dialogs/ide_dialogs.cppm` | `IdeOnboardingDialog()` | ✅ | L244 |
| 19 | `InitOnboarding` | 691 | **STUB** | — | `StubDialog("Welcome")` | 🟡 | "TODO UI11 — wire to onboarding" |
| 20 | `ModelSwitch` | 695 | **thin adapter** | `ui/dialogs/model_picker.cppm` | `model_picker::ModelPicker()` | 🔴 MISSING | Module exports only string-based `render_model_picker()`. **No FTXUI Component**. Namespace is `cc::ui::dialogs` flat (not `::model_picker` sub-ns) — dispatcher's namespace-qualified call **will not compile**. |
| 21 | `UndercoverCallout` | 701 | **STUB** | — | `StubDialog()` | 🟡 | "TODO UI11" |
| 22 | `EffortCallout` | 705 | **STUB** | — | `StubDialog()` | 🟡 | "TODO UI11" |
| 23 | `RemoteCallout` | 709 | **STUB** | — | `StubDialog()` | 🟡 | "TODO UI11" |
| 24 | `DesktopUpsell` | 713 | real | `ui/dialogs/desktop_upsell.cppm` | `DesktopUpsellDialog()` | ✅ | L140/L194 |
| 25 | `LspRecommendation` | 720 | real | `ui/dialogs/plugin_dialog.cppm` | `MakePluginDialog()` | ✅ | L516 (Kind::LspRecommendation) |
| 26 | `PluginHint` | 726 | real | `ui/dialogs/plugin_dialog.cppm` | `MakePluginDialog()` | ✅ | L516 (Kind::Recommendation) |
| 27 | `SettingsUsage` | 744 | real | `ui/dialogs/usage_dialog.cppm` | `UsageDialog()` | ✅ | L243 |
| 28 | `SettingsStatus` | 749 | real | `ui/dialogs/settings_status_page.cppm` | `MakeStatusPage()` | ✅ | L167 |
| 29 | `InstallGitHubApp` | 758 | **integration stub** | `ui/dialogs/install_github_app_wizard.cppm` | `MakeInstallGitHubAppWizard()` | ✅* | L54-69: Renderer + Escape/Return catch-event. Not 12-step but exports the correct API surface. Builds & works for dry-run. |
| 30 | `InstallSlackApp` | 764 | **integration stub** | `ui/dialogs/install_slack_app_wizard.cppm` | `MakeInstallSlackAppWizard()` | ✅* | Mirror of above; 3-step placeholder. Correct API. |
| 31 | `TrustPrompt` | 776 | real | `ui/dialogs/trust_dialog.cppm` | `MakePluginTrustDialog()` / `MakeWorkspaceTrustDialog()` | ✅ | L762/L833, dispatcher dispatches on `plugin_hint_name` presence. |
| 32 | `Permission` | 795 | real | `ui/permissions/permission_single_prompt.cppm` | `MakeSinglePromptDialog()` | ✅ | Duplicates #10 — intentional: both ToolPermission and "other" Permission paths converge. |
| 33 | `PermissionBatch` | 806 | real | `ui/permissions/permission_batch_panel.cppm` | `MakeBatchPanel()` | ✅ | L568 |
| 34 | `PermissionScopeEditor` | 813 | real | `ui/permissions/permission_scope_editor.cppm` | `MakeScopeEditor()` | ✅ | L849 |
| 35 | `PermissionSandbox` | 818 | real | `ui/permissions/sandbox_config_dialog.cppm` | `MakeSandboxConfigDialog()` | ✅ | L852/L873 |
| 36 | `PermissionRuleList` | 824 | real | `ui/permissions/permission_rule_list.cppm` | `MakePermissionRuleList()` | ✅ | L1471 |
| 37 | `PermissionAskUser` | 831 | real | `ui/permissions/permission_advanced_prompts.cppm` | `MakeAskUserQuestionDialog()` | ✅ | L601 |
| 38 | `PermissionSkill` | 839 | real | `ui/permissions/permission_advanced_prompts.cppm` | `MakeSkillPermissionDialog()` | ✅ | L867 |
| 39 | `PermissionFallback` | 846 | real | `ui/permissions/permission_advanced_prompts.cppm` | `MakeFallbackPermissionDialog()` | ✅ | L1254 |
| 40 | `McpServerList` | 859 | real | `ui/mcp/mcp_server_list.cppm` | `MakeMcpDialog()` | ✅ | L635 (named alias) |
| 41 | `McpAddServerWizard` | 863 | real | `ui/mcp/mcp_add_server_wizard.cppm` | `MakeMcpAddServerWizard()` | ✅ | L837 |
| 42 | `McpServerDetails` | 867 | real | `ui/mcp/mcp_server_details.cppm` | `MakeMcpServerDetails()` | ✅ | L969 |
| 43 | `McpSecurityCheck` | 871 | real | `ui/mcp/mcp_security_dialog.cppm` | `MakeMcpFirstAddDialog()` | ✅ | L431 |
| 44 | `AgentsView` | — | inline alias (see #7) | — | — | — | Dedup: `AgentsView` as inline panel (case #7). ReplMode enum value reused? No — line 886 dispatches `AgentsView` again via get_builder. ⚠️ Ambiguity: `RouteDialog()` treats it as inline panel (return nullptr), but `get_builder()` has an entry — entry is dead code. Minor. |
| 45 | `AgentCreationWizard` | 892 | real | `ui/agents/agent_wizard.cppm` | `AgentWizard()` | ✅ | L814 |
| 46 | `AgentDetailsModal` | 897 | real | `ui/agents/agent_details_dialog.cppm` | `AgentDetailsDialog()` | ✅ | L768 |
| 47 | `TeamDetails` | 912 | real | `ui/teams/team_details_dialog.cppm` | `MakeTeamDetailsDialog()` | ✅ | L390 |
| 48 | `TeamSwarm` | 917 | real | `ui/teams/swarm_collaboration_view.cppm` | `MakeSwarmCollaborationView()` | ✅ | L340 |
| 49 | `TaskDetails` | 925 | real | `ui/tasks/task_details_dialog.cppm` | `TaskDetailsDialog()` | ✅ | L317 |
| 50 | `NewTaskWizard` | 931 | real | `ui/tasks/task_wizard.cppm` | `TaskWizard()` | ✅ | L195 |
| 51 | `PluginManager` | 940 | real | `ui/dialogs/plugin_dialog.cppm` | `MakePluginDialog()` | ✅ | L516 (Kind::ManagerLanding) |
| 52 | `PluginManagePanel` | 944 | real | `ui/plugins/plugin_manage_panel.cppm` | `MakeManagePanel()` | ✅ | L367 |
| 53 | `PluginBrowsePanel` | 950 | real | `ui/plugins/plugin_marketplace_browse.cppm` | `MakeBrowsePanel()` | ✅ | L401 |
| 54 | `PluginInstallWizard` | 956 | real | `ui/plugins/plugin_install_flow.cppm` | `MakeInstallWizard()` | ✅ | L635 |
| 55 | `PluginSettingsDialog` | 961 | real | `ui/plugins/plugin_settings_dialog.cppm` | `MakePluginSettingsDialog()` | ✅ | L459 |
| 56 | `DesignThemePicker` | 970 | real | `ui/design_system/design_extras.cppm` | `MakeThemePicker()` | ✅ | L392 |
| 57 | `DesignFuzzyPicker` | 976 | real | `ui/design_system/design_extras.cppm` | `MakeFuzzyPicker()` | ✅ | L154 |
| 58 | `FeatureMemory` | 986 | real | `ui/components/feature_dialogs.cppm` | `MakeMemoryFileSelector()` | ✅ | L742/L766 |
| 59 | `FeatureFeedback` | 991 | real | `ui/components/feature_dialogs.cppm` | `MakeFeedbackDialog()` | ✅ | L1072/L1090 |
| 60 | `FeatureGrove` | 995 | real | `ui/components/feature_dialogs.cppm` | `MakeGroveView()` | ✅ | L1416/L1440 |
| 61 | `Doctor` | 1002 | real | `ui/screens/doctor_screen.cppm` | `DoctorScreen()` | ✅ | L780 |
| 62 | `Resume` | 1007 | real | `ui/screens/resume_screen.cppm` | `ResumeScreen()` | ✅ | L1288 |
| 63 | `LogSelector` | 1012 | real | `ui/screens/log_selector.cppm` | `MakeLogSelector()` | ✅ | L1735 |
| 64 | `DiffView` | 1021 | real | `ui/dialogs/diff_dialog.cppm` | `DiffDialog()` | ✅ | L431 |
| 65 | `GlobalSearch` | 1033 | **thin adapter** | `ui/dialogs/global_search_dialog.cppm` | `global_search_dialog::GlobalSearchDialog()` | 🔴 MISSING | Module exports only Element-level `render_search_results()` + enums. **No FTXUI Component**. No `GlobalSearchOptions` struct — dispatcher references both. |
| 66 | *(end)* | — | — | — | — | — | — |

### Count

| Category | Count | % of 66 |
|---|---:|---:|
| ✅ Fully wired (real factory / inline panel / integration stub with correct API) | 55 | 83.3% |
| ✅* Wired but has "thin" caveat (Element-only / placeholder wizard) | 3 | 4.5% |
| 🟡 Explicit STUB (no corresponding module yet — known gap, flagged in comment) | 7 | 10.6% |
| 🔴 **Compile-time blocker: dispatcher calls a nonexistent Component factory** | 4 | 6.1% |
| ⚠️ Minor dead-code / dedup issues | 1 | 1.5% |

---

## 🔴 Blocker Detail (Will Fail Compile in Phase C-c/d)

These 4 `ReplMode`s must be fixed **before** linking `cc_repl` executable because the dispatcher references symbols that the module files do not export:

### B1. `ReplMode::HelpView` (L1038–1040)

- **Dispatcher says:** `cc::ui::dialogs::help_v2::HelpDialog(std::move(opts))`
- **Module (`help_v2.cppm`) exports:** `cc::ui::dialogs::render_help_dialog()` returning `std::string`
- **Namespace mismatch:** Module exports flat under `cc::ui::dialogs`, no `::help_v2` sub-namespace.
- **Fix options:**
  - [Easy] Rewrap the string renderer in a `Renderer(...)` component + Escape close (matches `CostThreshold` pattern).
  - [Proper] Rewrite `help_v2.cppm` as an FTXUI Component backed by `Menu`/`Container::Vertical`.

### B2. `ReplMode::QuickOpen` (L1041–1045)

- **Dispatcher says:** `cc::ui::dialogs::quick_open::QuickOpenDialog(std::move(opts))`
- **Module exports:** only string-based `render_quick_open()` + `filter_items()` helpers.
- **Namespace mismatch:** also flat `cc::ui::dialogs`.
- **Fix options:**
  - [Easy] Wrapper `Renderer` with existing string output.
  - [Proper] Replace with `design_extras::MakeFuzzyPicker()` (UI27 already does fuzzy + callbacks).

### B3. `ReplMode::ModelSwitch` (L695–L700)

- **Dispatcher says:** `cc::ui::dialogs::model_picker::ModelPicker(std::move(opts))`
- **Module exports:** only string-based `render_model_picker()` under flat `cc::ui::dialogs`.
- **Struct also missing:** dispatcher uses `ModelPickerOptions` (not exported).
- **Fix options:**
  - [Easy] Minimal Component wrapper + stub Options.
  - [Proper] Reuse `custom_select::MakeSingleSelect()` (ui/components/custom_select.cppm L1076) with model list.

### B4. `ReplMode::GlobalSearch` (L1033–L1037)

- **Dispatcher says:** `cc::ui::dialogs::global_search_dialog::GlobalSearchDialog(std::move(opts))`
- **Module exports:** only Element-level `render_search_results()` + `SearchCategory`/`SearchResult` enums.
- **Struct also missing:** dispatcher uses `GlobalSearchOptions` (not defined).
- **Namespace mismatch:** flat `cc::ui::dialogs` vs. `::global_search_dialog` sub-namespace.
- **Fix:** Wrapper with `Input` + render_search_results Element, or reuse `MakeFuzzyPicker(...)` with 3 category items.

### ⚠️ N1. `AgentsView` — dispatcher has dead entry

- `RouteDialog()` (L1066-1070) short-circuits and returns `nullptr` for `AgentsView` (treats it as inline panel).
- But `get_builder()` (L886) has a full `AgentListComponent(...)` factory entry.
- If someone later removes the inline-panel short-circuit, the factory entry would fire; until then it's dead code.
- **Low severity** — cosmetic, no build impact.

---

## 🟡 Known STUBs (Intentional — roadmapped for later phases)

These 7 ReplModes explicitly use `StubDialog()` and have TODO comments pointing to the responsible UI sub-agent.
They **build correctly** (return valid components) and carry explicit placeholder UI.

| ReplMode | Owning sub-agent | Estimated scope |
|---|---|---|
| `MessageSelector` | UI25 — `messages_interactions` | ~300 LOC (rewind/summarize/edit) |
| `PromptHook` | UI2 — prompt system | ~200 LOC (HookPrompt variant) |
| `UltraplanChoice` / `UltraplanLaunch` | UI11 — onboarding + Ultraplan module | ~250 LOC + ULTRAPLAN feature gate |
| `InitOnboarding` | UI11 — onboarding | ~400 LOC (welcome wizard) |
| `UndercoverCallout` / `EffortCallout` / `RemoteCallout` | UI11 — callout dialogs | 3× ~80 LOC each |

**Recommendation:** Leave as-is for Phase C. They are intentional integration-stubs that render correctly in `--dry-run`.

---

## ✅* Thin-Wired (Functional but Not Feature-Complete)

| ReplMode | Current | Gap |
|---|---|---|
| `CostThreshold` | Wraps `render_cost_threshold()` Element | No interactive Y/N + no actual threshold compare callback |
| `InstallGitHubApp` | Renderer + Escape/Enter events | Needs 12-step state machine (Phase 2's `install_github_app_steps` + oauth service) |
| `InstallSlackApp` | Renderer + Escape/Enter events | Needs 3-step state machine + Slack OAuth flow |

**Phase C scope:** All three are acceptable; `--dry-run` will render them. Deeper wiring is Phase 3 (services) work.

---

## Phase C Action Checklist (a → e)

### (a) Factory audit ✅  — this document.

### (b) Fill `get_builder()` table — required before `main.cc`

- [ ] **Fix B1**: `HelpView` — wrap in Renderer or rework to FTXUI Component
- [ ] **Fix B2**: `QuickOpen` — wrap or delegate to `MakeFuzzyPicker()`
- [ ] **Fix B3**: `ModelSwitch` — wrap string output OR use `MakeSingleSelect()`
- [ ] **Fix B4**: `GlobalSearch` — add `Input` + render_search_results
- [ ] Optional: Drop the dead-code `AgentsView` entry in `get_builder()`

### (c) `main.cc` entry point

Depends on `ConfigManager` existence. Propose:

```
Phase C-c deliverables:
  cpp_migration/src/main.cc
    → flags: --dry-run, --config <path>, --help
    → ConfigManager::load_from_default() or from --config
    → cc::ui::design::theme::set_theme(current_theme())
    → ScreenInteractive::FitComponent() with RenderReplScreen(state, &cfg)
```

### (d) CMake `add_executable(cc_repl main.cc)`

Needs to link:
- `cc_ui_screens` (repl_screen / doctor / resume / log_selector)
- `cc_ui_dialogs` + `cc_ui_components` + `cc_ui_design`
- `cc_ui_permissions` + `cc_ui_mcp` + `cc_ui_tasks` + `cc_ui_agents` + `cc_ui_teams` + `cc_ui_plugins`
- `cc_core_config` (ConfigManager)
- `ftxui::component` / `ftxui::dom` / `ftxui::screen`

### (e) `cmake --build` + `./cc_repl --dry-run` smoke

Expected acceptance:
- Prints banner (StatusBar header)
- Shows empty Messages area + prompt
- Accepts Escape → exit (or Ctrl+C)
- Accepts '?' / `/help` key → **but requires B1 fix or fallback to Stub**

---

## Appendix: File → Exported Factories Quick Reference

Used to build the matrix above. Source `cpp_migration/src/ui/**/*.cppm`:

| File (relative to src/) | Module name | Factories / top-level Components |
|---|---|---|
| `ui/dialogs/settings_dialog.cppm` | `cc.ui.dialogs.settings_dialog` | `MakeSettingsDialog()` |
| `ui/dialogs/diff_dialog.cppm` | `cc.ui.dialogs.diff_dialog` | `DiffDialog()` |
| `ui/dialogs/trust_dialog.cppm` | `cc.ui.trust_dialog` | `MakeTrustDialogComponent()`, `MakePluginTrustDialog()`, `MakePathTrustDialog()`, `MakeWorkspaceTrustDialog()` |
| `ui/dialogs/sandbox_dialog.cppm` | `cc.ui.sandbox_dialog` | `SandboxDialog()` (×2 overloads) |
| `ui/permissions/permission_single_prompt.cppm` | `cc.ui.permissions.single_prompt` | `MakeSinglePromptDialog()`, `MakeBashPrompt()`, `MakeFileEditPrompt()`, `MakeNetworkPrompt()` |
| `ui/permissions/permission_batch_panel.cppm` | `cc.ui.permissions.batch_panel` | `MakeBatchPanel()` |
| `ui/permissions/permission_scope_editor.cppm` | `cc.ui.permissions.scope_editor` | `MakeScopeEditor()` |
| `ui/permissions/sandbox_config_dialog.cppm` | `cc.ui.permissions.sandbox_config` | `MakeSandboxConfigDialog()` (×2) |
| `ui/dialogs/install_github_app_wizard.cppm` | `cc.ui.dialogs.install_github_app_wizard` | `MakeInstallGitHubAppWizard()` (stub impl) |
| `ui/dialogs/install_slack_app_wizard.cppm` | `cc.ui.dialogs.install_slack_app_wizard` | `MakeInstallSlackAppWizard()` (stub impl) |
| `ui/dialogs/plugin_dialog.cppm` | `cc.ui.dialogs.plugin_dialog` | `MakePluginDialog()` |
| `ui/plugins/plugin_manage_panel.cppm` | `cc.ui.plugins.plugin_manage_panel` | `MakeManagePanel()` |
| `ui/plugins/plugin_marketplace_browse.cppm` | `cc.ui.plugins.plugin_marketplace_browse` | `MakeBrowsePanel()` |
| `ui/plugins/plugin_install_flow.cppm` | `cc.ui.plugins.plugin_install_flow` | `MakeInstallWizard()` |
| `ui/plugins/plugin_settings_dialog.cppm` | `cc.ui.plugins.plugin_settings_dialog` | `MakePluginSettingsDialog()` |
| `ui/screens/doctor_screen.cppm` | `cc.ui.doctor_screen` | `DoctorScreen()` |
| `ui/screens/resume_screen.cppm` | `cc.ui.resume_screen` | `ResumeScreen()`, `MakeDeleteSessionTrustDialog()` |
| `ui/screens/log_selector.cppm` | `cc.ui.screens.log_selector` | `MakeLogSelector()` |
| `ui/permissions/permission_rule_list.cppm` | `cc.ui.permissions.rule_list` | `MakePermissionRuleList()` |
| `ui/permissions/permission_advanced_prompts.cppm` | `cc.ui.permissions.advanced_prompts` | `MakeAskUserQuestionDialog()`, `MakeSkillPermissionDialog()`, `MakeFallbackPermissionDialog()` |
| `ui/components/feature_dialogs.cppm` | `cc.ui.components.feature_dialogs` | `MakeMemoryFileSelector()`, `MakeFeedbackDialog()`, `MakeGroveView()` |
| `ui/design_system/design_extras.cppm` | `cc.ui.design.extras` | `MakeFuzzyPicker()`, `MakeThemePicker()` |
| `ui/mcp/mcp_server_list.cppm` | `cc.ui.mcp.mcp_server_list` | `McpServerList()`, `MakeMcpDialog()` |
| `ui/mcp/mcp_add_server_wizard.cppm` | `cc.ui.mcp.mcp_add_server_wizard` | `McpAddServerWizard()`, `MakeMcpAddServerWizard()` |
| `ui/mcp/mcp_server_details.cppm` | `cc.ui.mcp.mcp_server_details` | `McpServerDetails()`, `MakeMcpServerDetails()` |
| `ui/mcp/mcp_security_dialog.cppm` | `cc.ui.mcp.mcp_security_dialog` | `McpSecurityDialog()`, `MakeMcpFirstAddDialog()`, `MakeMcpCriticalDialog()`, `MakeMcpSecurityDialog()` |
| `ui/tasks/task_list_view.cppm` | `cc.ui.tasks.task_list_view` | `TaskListView()` |
| `ui/tasks/task_details_dialog.cppm` | `cc.ui.tasks.task_details_dialog` | `TaskDetailsDialog()` |
| `ui/tasks/task_wizard.cppm` | `cc.ui.tasks.task_wizard` | `TaskWizard()` (+ Step1/Step2/Step3) |
| `ui/agents/agent_list.cppm` | `cc.ui.agents.agent_list` | `AgentListComponent()` |
| `ui/agents/agent_wizard.cppm` | `cc.ui.agents.agent_wizard` | `AgentWizard()` (+ 4 Step*) |
| `ui/agents/agent_details_dialog.cppm` | `cc.ui.agents.agent_details_dialog` | `AgentDetailsDialog()` |
| `ui/teams/teams_overview.cppm` | `cc.ui.teams.teams_overview` | `MakeTeamsOverview()` |
| `ui/teams/team_details_dialog.cppm` | `cc.ui.teams.team_details_dialog` | `MakeTeamDetailsDialog()` |
| `ui/teams/swarm_collaboration_view.cppm` | `cc.ui.teams.swarm_collaboration_view` | `MakeSwarmCollaborationView()` |
| `ui/dialogs/ide_dialogs.cppm` | `cc.ui.ide_dialogs` | `IdeAutoConnectDialog()`, `IdeOnboardingDialog()`, `IdeStatusIndicator()` |
| `ui/dialogs/model_picker.cppm` | `cc.ui.dialogs.model_picker` | **(string-only exports)** `render_model_picker()`, `render_model_details()` — **no Component** ❌ |
| `ui/dialogs/desktop_upsell.cppm` | `cc.ui.dialogs.desktop_upsell` | `DesktopUpsellDialog()` (×2) |
| `ui/dialogs/usage_dialog.cppm` | `cc.ui.dialogs.usage_dialog` | `UsageDialog()` |
| `ui/dialogs/settings_status_page.cppm` | `cc.ui.dialogs.settings_status_page` | `MakeStatusPage()` |
| `ui/dialogs/global_search_dialog.cppm` | `cc.ui.dialogs.global_search_dialog` | **(Element-only)** `render_search_results()` — **no Component** ❌ |
| `ui/dialogs/help_v2.cppm` | `cc.ui.dialogs.help_v2` | **(string-only exports)** `render_help_dialog()`, `get_general_help_sections()`, `get_commands_help_sections()` — **no Component** ❌ |
| `ui/dialogs/quick_open.cppm` | `cc.ui.dialogs.quick_open` | **(string-only exports)** `render_quick_open()`, `filter_items()` — **no Component** ❌ |
| `ui/dialogs/cost_threshold_dialog.cppm` | `cc.ui.dialogs.cost_threshold_dialog` | **(Element-only)** `render_cost_threshold()` + `CostThresholdInfo` struct — wrapped by dispatcher via `Renderer()`. |
| `ui/screens/repl_screen.cppm` | *(part of cc_ui_screens)* | `RouteDialog()`, `RenderReplScreen()`, `ReplScreen()` (+ namespace `dialog_stubs`) |
