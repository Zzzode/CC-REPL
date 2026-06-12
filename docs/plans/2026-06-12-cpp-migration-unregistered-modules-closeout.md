# C++ Migration Unregistered Modules Closeout Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Close the TS to C++ migration gap caused by unregistered C++ module files, stale skeleton implementations, and UI modules that were migrated but never integrated into the active build/runtime path.

**Architecture:** Treat the current CMake build graph as the source of truth. Delete or archive historical skeleton modules that have been superseded by active modules, then integrate valuable unregistered UI modules one domain at a time through CMake, imports, router wiring, and tests. Keep `scripts/cpp-migration-inventory.mjs --strict` as the final structural gate.

**Tech Stack:** C++23 modules, CMake/Ninja, FTXUI, GoogleTest/CTest, Node/Bun migration scripts.

---

## Current Evidence

Fresh checks on 2026-06-12 showed:

- `node scripts/cpp-migration-inventory.mjs --strict` fails.
- C++ source files: 1210.
- CMake registered entries: 1174.
- Unregistered compilable files: 36.
- Migration markers: 205 total, 110 blocking.
- `CC_REPL_CMAKE_BUILD_DIR=cpp_migration/build/clang-debug node scripts/cpp-migration-ctest-gates.mjs` passes the early core gates, then fails because the UI runtime gate cannot discover required tests.

Core gates that passed before UI discovery failure:

- P0 bridge/headless remote lifecycle.
- P1 direct-connect permission protocol.
- P1 sub-agent permission context.
- P1 SendMessage/team/swarm protocol.
- P1 MCP auth and remote behavior.
- P1 session/compaction/context semantics.

The failure mode is not a failing UI assertion. The gate expects test names such as `AppRuntime.StreamingToolUseRendersRunningPreview`, while current tests use names like `AppRuntime.StreamingToolUseShowsSpinnerAndLoadingState`.

## Classification Of Unregistered Files

### Historical Skeletons Or Superseded Prototypes

These should be removed, archived, or explicitly excluded after confirming no useful code remains:

- `cpp_migration/src/hooks/use_permission.cppm`
- `cpp_migration/src/services/query_engine.cppm`
- `cpp_migration/src/state/store_impl.cppm`
- `cpp_migration/src/state/persistence_json.cppm`
- `cpp_migration/src/tools/bash/tool_bash.cppm`
- `cpp_migration/src/tools/core/tool_base.cppm`
- `cpp_migration/src/tools/files/tool_io.cppm`

Known replacements:

- `services/query_engine.cppm` is superseded by `query/query_engine.cppm`.
- `state/store_impl.cppm` and `state/persistence_json.cppm` are superseded by `state/store.cppm`, `state/app_state.cppm`, and `state/persistence.cppm`.
- `tools/bash/tool_bash.cppm` is superseded by `tools/bash_tool.cppm` and `tools/bash/impl_bash.cppm`.
- `tools/core/tool_base.cppm` is superseded by `tools/tool.cppm`.
- `tools/files/tool_io.cppm` is superseded by `tools/files/impl_files.cppm` and the concrete file tools.

### Orphan Helper Modules

These need a short owner decision. If the logic is still useful, merge it into the active module. If not, delete it:

- `cpp_migration/src/tools/bash_comment_label.cppm`
- `cpp_migration/src/tools/file_edit_constants.cppm`
- `cpp_migration/src/tools/tools_utils.cppm`
- `cpp_migration/src/tools/web_fetch_preapproved.cppm`

### Intentionally Excluded But Unfinished

These are already called out in `ui/screens/repl_screen.cppm` as excluded because their signatures do not match the implemented wizard framework:

- `cpp_migration/src/ui/dialogs/install_github_app_wizard.cppm`
- `cpp_migration/src/ui/dialogs/install_slack_app_wizard.cppm`

### Prompt UI Branch Not On Main Runtime Path

These are only referenced by the unregistered composer module. The active CMake graph already contains `ui/prompt/prompt_input_full.cppm`, `ui/prompt/footer.cppm`, and related prompt modules:

- `cpp_migration/src/ui/components/prompt_input_composer.cppm`
- `cpp_migration/src/ui/prompt/prompt_footer.cppm`
- `cpp_migration/src/ui/prompt/prompt_widgets.cppm`
- `cpp_migration/src/ui/prompt/suggestion_dropdown.cppm`

### Migrated UI Modules Not Yet Integrated

These look like valuable migration output, but they are not in CMake and not wired into active runtime routes:

- `cpp_migration/src/ui/dialogs/diff_dialog.cppm`
- `cpp_migration/src/ui/mcp/mcp_add_server_wizard.cppm`
- `cpp_migration/src/ui/mcp/mcp_security_dialog.cppm`
- `cpp_migration/src/ui/mcp/mcp_server_details.cppm`
- `cpp_migration/src/ui/mcp/mcp_server_list.cppm`
- `cpp_migration/src/ui/permissions/permission_batch_panel.cppm`
- `cpp_migration/src/ui/permissions/permission_single_prompt.cppm`
- `cpp_migration/src/ui/permissions/sandbox_config_dialog.cppm`
- `cpp_migration/src/ui/plugins/plugin_install_flow.cppm`
- `cpp_migration/src/ui/plugins/plugin_manage_panel.cppm`
- `cpp_migration/src/ui/plugins/plugin_marketplace_browse.cppm`
- `cpp_migration/src/ui/plugins/plugin_settings_dialog.cppm`
- `cpp_migration/src/ui/tasks/task_components.cppm`
- `cpp_migration/src/ui/tasks/task_details_dialog.cppm`
- `cpp_migration/src/ui/tasks/task_list_view.cppm`
- `cpp_migration/src/ui/tasks/task_wizard.cppm`
- `cpp_migration/src/ui/teams/swarm_collaboration_view.cppm`
- `cpp_migration/src/ui/teams/team_details_dialog.cppm`
- `cpp_migration/src/ui/teams/teams_overview.cppm`

## Task 1: Add A Decision Register For Unregistered Modules

**Files:**

- Create: `cpp_migration/docs/unregistered-modules-decision-register.md`

**Step 1: Write the register**

Create a table with columns:

- File.
- Module name.
- Classification.
- Replacement or integration target.
- Decision: delete, merge, integrate, allowlist temporarily.
- Owner area.
- Notes.

**Step 2: Fill it from this plan**

Use the classifications above as the initial rows.

**Step 3: Verify no file is missing**

Run:

```bash
node scripts/cpp-migration-inventory.mjs --json \
  | node -e "let s='';process.stdin.on('data',d=>s+=d);process.stdin.on('end',()=>{const r=JSON.parse(s); for (const f of r.cmake.unregisteredCompilableFiles) console.log(f)})"
```

Expected: every printed file appears in the decision register.

**Step 4: Commit**

```bash
git add cpp_migration/docs/unregistered-modules-decision-register.md
git commit -m "docs(cpp-migration): classify unregistered modules"
```

## Task 2: Remove Historical Skeletons

**Files:**

- Delete: `cpp_migration/src/hooks/use_permission.cppm`
- Delete: `cpp_migration/src/services/query_engine.cppm`
- Delete: `cpp_migration/src/state/store_impl.cppm`
- Delete: `cpp_migration/src/state/persistence_json.cppm`
- Delete: `cpp_migration/src/tools/bash/tool_bash.cppm`
- Delete: `cpp_migration/src/tools/core/tool_base.cppm`
- Delete: `cpp_migration/src/tools/files/tool_io.cppm`
- Modify if needed: `cpp_migration/tests/state_smoke.cpp`

**Step 1: Confirm no active imports**

Run:

```bash
rg -n "import cc\\.hooks\\.use_permission|import cc\\.services\\.query_engine|import cc\\.state\\.store_impl|import cc\\.state\\.persistence_json|import cc\\.tools\\.bash_skel|import cc\\.tools\\.core|import cc\\.tools\\.files\\b" cpp_migration/src cpp_migration/tests
```

Expected: only `state_smoke.cpp` and dependencies between the deleted skeleton files should appear. If active source files appear, stop and update this task.

**Step 2: Decide what to do with `state_smoke.cpp`**

`state_smoke.cpp` imports `cc.state.store_impl` and `cc.state.persistence_json`, which are not in CMake. Either:

- Delete `state_smoke.cpp` if it is obsolete and not registered as a CTest target.
- Or rewrite it to use the active `cc.state.store` and `cc.state.persistence` modules.

Prefer deletion unless a current migration script depends on it.

**Step 3: Delete obsolete files**

Use `apply_patch` or normal tracked deletion. Do not touch active replacements.

**Step 4: Verify**

Run:

```bash
node scripts/cpp-migration-inventory.mjs
cmake --build cpp_migration/build/clang-debug --target test_state test_tools test_services
ctest --test-dir cpp_migration/build/clang-debug -R "State|Tools|QueryEngine|SessionHistory" --output-on-failure
```

Expected:

- Inventory unregistered count drops by the number of deleted files.
- Build succeeds.
- Matching tests pass.

**Step 5: Commit**

```bash
git add -A cpp_migration/src cpp_migration/tests
git commit -m "refactor(cpp-migration): remove superseded skeleton modules"
```

## Task 3: Resolve Orphan Helper Modules

**Files:**

- Inspect: `cpp_migration/src/tools/bash_comment_label.cppm`
- Inspect: `cpp_migration/src/tools/file_edit_constants.cppm`
- Inspect: `cpp_migration/src/tools/tools_utils.cppm`
- Inspect: `cpp_migration/src/tools/web_fetch_preapproved.cppm`
- Likely modify: `cpp_migration/src/tools/bash_result_formatting.cppm`
- Likely modify: `cpp_migration/src/tools/file_edit_tool.cppm`
- Likely modify: `cpp_migration/src/tools/web_fetch_tool.cppm`
- Likely modify: `cpp_migration/src/tools/tool_display_names.cppm`

**Step 1: Search for equivalent active logic**

Run:

```bash
rg -n "comment label|bash comment|file edit constant|preapproved|pre-approved|web fetch|ToolUtils|tools utils" cpp_migration/src/tools cpp_migration/src/utils
```

**Step 2: For each helper, choose exactly one action**

- Merge useful constants/functions into the active module.
- Add the helper to CMake and import it from an active module.
- Delete it if equivalent logic already exists.

Avoid adding a module to CMake if no active runtime code imports it.

**Step 3: Add focused tests if behavior changes**

Use existing tests in `cpp_migration/tests/test_tools.cpp`. Add or update tests for:

- Bash label rendering if `bash_comment_label.cppm` is merged.
- File edit constants if user-visible limits or prompts change.
- WebFetch preapproval behavior if `web_fetch_preapproved.cppm` is connected.

**Step 4: Verify**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_tools
ctest --test-dir cpp_migration/build/clang-debug -R "Tools\\.|WebFetch|Bash|FileEdit" --output-on-failure
node scripts/cpp-migration-inventory.mjs
```

Expected: no regressions; unregistered count decreases.

**Step 5: Commit**

```bash
git add -A cpp_migration/src/tools cpp_migration/tests/test_tools.cpp
git commit -m "refactor(tools): resolve orphan migration helpers"
```

## Task 4: Decide Prompt UI Branch Fate

**Files:**

- Inspect: `cpp_migration/src/ui/components/prompt_input_composer.cppm`
- Inspect: `cpp_migration/src/ui/prompt/prompt_footer.cppm`
- Inspect: `cpp_migration/src/ui/prompt/prompt_widgets.cppm`
- Inspect: `cpp_migration/src/ui/prompt/suggestion_dropdown.cppm`
- Compare: `cpp_migration/src/ui/prompt/prompt_input_full.cppm`
- Compare: `cpp_migration/src/ui/prompt/footer.cppm`
- Compare: `cpp_migration/src/ui/prompt/autocomplete.cppm`
- Compare: `cpp_migration/src/ui/prompt/vim_input.cppm`

**Step 1: Compare exported capabilities**

Run:

```bash
rg -n "export module|export namespace|struct |class |\\[\\[nodiscard\\]\\]|Component" cpp_migration/src/ui/components/prompt_input_composer.cppm cpp_migration/src/ui/prompt/prompt_footer.cppm cpp_migration/src/ui/prompt/prompt_widgets.cppm cpp_migration/src/ui/prompt/suggestion_dropdown.cppm cpp_migration/src/ui/prompt/prompt_input_full.cppm cpp_migration/src/ui/prompt/footer.cppm cpp_migration/src/ui/prompt/autocomplete.cppm
```

**Step 2: Prefer deletion if the active prompt path covers the behavior**

The unregistered composer exports `ui.components.prompt_input_composer`, which does not follow the `cc.*` module naming convention. That is a strong signal it is not intended as current production surface.

**Step 3: If any behavior is missing, port only that behavior**

Move the specific behavior into active prompt modules. Do not preserve the whole parallel branch.

**Step 4: Verify UI tests**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui
ctest --test-dir cpp_migration/build/clang-debug -R "Components\\.TextInput|AppRuntime|Prompt" --output-on-failure
node scripts/cpp-migration-inventory.mjs
```

Expected: prompt-related tests pass and four prompt branch files are no longer unregistered.

**Step 5: Commit**

```bash
git add -A cpp_migration/src/ui cpp_migration/tests/test_ui.cpp
git commit -m "refactor(ui): remove unused prompt migration branch"
```

## Task 5: Fix Or Explicitly Defer Install Wizard Modules

**Files:**

- Inspect: `cpp_migration/src/ui/dialogs/install_github_app_wizard.cppm`
- Inspect: `cpp_migration/src/ui/dialogs/install_slack_app_wizard.cppm`
- Modify: `cpp_migration/src/ui/screens/repl_screen.cppm`
- Possibly modify: `cpp_migration/src/CMakeLists.txt`
- Possibly modify: `scripts/cpp-migration-inventory.mjs`

**Step 1: Try compiling the modules in isolation**

Temporarily add both files to the `cc_ui` CMake module set.

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target cc_ui
```

Expected: either success or signature/API mismatch errors that identify the exact incompatibility with `wizard_dialog`.

**Step 2: Choose one path**

Path A, preferred if errors are small:

- Fix signatures.
- Import the modules in `repl_screen.cppm`.
- Route `InstallGitHubApp` and `InstallSlackApp` modes to real wizard components.

Path B, acceptable short-term:

- Keep them excluded.
- Add them to an explicit inventory allowlist with comments pointing to this plan.
- Keep `repl_screen.cppm` comments accurate.

**Step 3: Add tests**

If implementing Path A, add UI tests in `cpp_migration/tests/test_ui.cpp` that verify:

- GitHub install mode renders wizard content.
- Slack install mode renders wizard content.
- Escape/cancel returns to normal mode.

If choosing Path B, add an inventory test or script assertion that allowlisted files are documented.

**Step 4: Verify**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui
ctest --test-dir cpp_migration/build/clang-debug -R "AppRuntime|Install|Wizard" --output-on-failure
node scripts/cpp-migration-inventory.mjs --strict
```

Expected:

- Path A: strict inventory improves because the files are registered.
- Path B: strict inventory only passes if the inventory script supports documented allowlist entries.

**Step 5: Commit**

```bash
git add -A cpp_migration/src cpp_migration/tests scripts
git commit -m "feat(ui): resolve install wizard migration status"
```

## Task 6: Integrate Or Retire Diff Dialog

**Files:**

- Inspect: `cpp_migration/src/ui/dialogs/diff_dialog.cppm`
- Modify: `cpp_migration/src/CMakeLists.txt`
- Modify: `cpp_migration/src/ui/screens/repl_screen.cppm`
- Test: `cpp_migration/tests/test_ui.cpp`

**Step 1: Add the module to CMake**

Add `ui/dialogs/diff_dialog.cppm` to the `cc_ui` module list.

**Step 2: Build**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target cc_ui
```

Expected: build succeeds or exposes integration errors.

**Step 3: Route it from `repl_screen.cppm`**

Use the file's own dialog router notes as guidance, but keep the implementation minimal:

- Add the needed import.
- Add or reuse a diff mode.
- Render the diff dialog when that mode is active.
- Close on Escape.

**Step 4: Test**

Add tests that render the diff route and close it.

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui
ctest --test-dir cpp_migration/build/clang-debug -R "Diff|AppRuntime" --output-on-failure
```

**Step 5: Commit**

```bash
git add cpp_migration/src/CMakeLists.txt cpp_migration/src/ui cpp_migration/tests/test_ui.cpp
git commit -m "feat(ui): wire migrated diff dialog"
```

## Task 7: Integrate Plugin Panels

**Files:**

- Modify: `cpp_migration/src/CMakeLists.txt`
- Modify: `cpp_migration/src/ui/dialogs/plugin_dialog.cppm`
- Add/register as appropriate:
  - `cpp_migration/src/ui/plugins/plugin_install_flow.cppm`
  - `cpp_migration/src/ui/plugins/plugin_manage_panel.cppm`
  - `cpp_migration/src/ui/plugins/plugin_marketplace_browse.cppm`
  - `cpp_migration/src/ui/plugins/plugin_settings_dialog.cppm`
- Test: `cpp_migration/tests/test_ui.cpp`

**Step 1: Add one plugin panel at a time**

Start with `plugin_manage_panel.cppm`.

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target cc_ui
```

Expected: either success or concrete import/API errors.

**Step 2: Replace fallback text in `plugin_dialog.cppm`**

`plugin_dialog.cppm` currently documents delegation but does not import the delegated panels. Replace fallback content such as delegated placeholder text with real panel rendering.

**Step 3: Repeat for marketplace, install flow, and settings**

Do not add all four modules in one patch unless the first one builds cleanly and proves the pattern.

**Step 4: Test route coverage**

Add tests for:

- Manage plugins route.
- Marketplace browse route.
- Install flow route or explicit disabled/deferred state.
- Settings route.

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui
ctest --test-dir cpp_migration/build/clang-debug -R "Plugin|AppRuntime" --output-on-failure
```

**Step 5: Commit**

```bash
git add cpp_migration/src/CMakeLists.txt cpp_migration/src/ui cpp_migration/tests/test_ui.cpp
git commit -m "feat(ui): wire plugin management panels"
```

## Task 8: Integrate MCP Panels

**Files:**

- Modify: `cpp_migration/src/CMakeLists.txt`
- Modify one or both:
  - `cpp_migration/src/ui/dialogs/mcp_dialog.cppm`
  - `cpp_migration/src/ui/dialogs/mcp_dialogs.cppm`
- Add/register as appropriate:
  - `cpp_migration/src/ui/mcp/mcp_add_server_wizard.cppm`
  - `cpp_migration/src/ui/mcp/mcp_security_dialog.cppm`
  - `cpp_migration/src/ui/mcp/mcp_server_details.cppm`
  - `cpp_migration/src/ui/mcp/mcp_server_list.cppm`
- Test: `cpp_migration/tests/test_ui.cpp`

**Step 1: Decide single MCP UI owner**

The active graph already has `mcp_dialog.cppm`, `mcp_dialogs.cppm`, `mcp_settings_panel.cppm`, `mcp_tool_browser.cppm`, and related modules. Decide whether the unregistered modules replace or enrich this active path.

**Step 2: Add server list first**

Add `mcp_server_list.cppm` and build:

```bash
cmake --build cpp_migration/build/clang-debug --target cc_ui
```

**Step 3: Resolve shared helper issue**

`mcp_server_details.cppm` comments mention `status_badge` from `mcp_server_list`. Avoid relying on module-local linker symbols. Export the helper from a shared active module or duplicate a small local helper if simpler.

**Step 4: Add wizard/security/details after list builds**

Integrate one module per patch.

**Step 5: Test**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui test_services
ctest --test-dir cpp_migration/build/clang-debug -R "Mcp|MCP|AppRuntime" --output-on-failure
```

**Step 6: Commit**

```bash
git add cpp_migration/src/CMakeLists.txt cpp_migration/src/ui cpp_migration/tests/test_ui.cpp
git commit -m "feat(ui): wire MCP management panels"
```

## Task 9: Integrate Permission Advanced Panels

**Files:**

- Modify: `cpp_migration/src/CMakeLists.txt`
- Modify active permission UI modules:
  - `cpp_migration/src/ui/permissions/permission_request.cppm`
  - `cpp_migration/src/ui/permissions/permission_views.cppm`
  - `cpp_migration/src/ui/dialogs/sandbox_dialog.cppm`
- Add/register:
  - `cpp_migration/src/ui/permissions/permission_batch_panel.cppm`
  - `cpp_migration/src/ui/permissions/permission_single_prompt.cppm`
  - `cpp_migration/src/ui/permissions/sandbox_config_dialog.cppm`
- Test: `cpp_migration/tests/test_ui.cpp`

**Step 1: Add `permission_single_prompt.cppm` first**

Build `cc_ui`.

**Step 2: Add batch panel after single prompt**

The batch panel imports `cc.ui.permissions.single_prompt`, so it must come second.

**Step 3: Decide sandbox dialog ownership**

There is already `ui/dialogs/sandbox_dialog.cppm`. Either wire `sandbox_config_dialog.cppm` into it or delete the unregistered one after porting useful logic.

**Step 4: Test permission render paths**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui
ctest --test-dir cpp_migration/build/clang-debug -R "Permission|Sandbox|AppRuntime" --output-on-failure
```

**Step 5: Commit**

```bash
git add cpp_migration/src/CMakeLists.txt cpp_migration/src/ui cpp_migration/tests/test_ui.cpp
git commit -m "feat(ui): wire advanced permission panels"
```

## Task 10: Integrate Or Collapse Task UI Branch

**Files:**

- Modify: `cpp_migration/src/CMakeLists.txt`
- Modify active task UI:
  - `cpp_migration/src/ui/tasks/task_list_ui.cppm`
  - `cpp_migration/src/ui/tasks/task_detail_dialog.cppm`
  - `cpp_migration/src/ui/screens/repl_screen.cppm`
- Add/register or delete after merging:
  - `cpp_migration/src/ui/tasks/task_components.cppm`
  - `cpp_migration/src/ui/tasks/task_details_dialog.cppm`
  - `cpp_migration/src/ui/tasks/task_list_view.cppm`
  - `cpp_migration/src/ui/tasks/task_wizard.cppm`
- Test: `cpp_migration/tests/test_ui.cpp`
- Test: `cpp_migration/tests/test_tasks.cpp`

**Step 1: Compare current and unregistered task modules**

Run:

```bash
rg -n "export module|struct |enum class|Component|Task" cpp_migration/src/ui/tasks/task_list_ui.cppm cpp_migration/src/ui/tasks/task_detail_dialog.cppm cpp_migration/src/ui/tasks/task_components.cppm cpp_migration/src/ui/tasks/task_details_dialog.cppm cpp_migration/src/ui/tasks/task_list_view.cppm cpp_migration/src/ui/tasks/task_wizard.cppm
```

**Step 2: Choose one active task UI API**

Prefer the existing registered module names unless the unregistered versions are clearly more complete.

**Step 3: Merge reusable widgets**

If `task_components.cppm` has reusable rendering that is not present in active modules, register it and import it from active task UI. Otherwise port minimal helpers and delete the branch.

**Step 4: Test**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui test_tasks
ctest --test-dir cpp_migration/build/clang-debug -R "Task|Tasks|AppRuntime" --output-on-failure
```

**Step 5: Commit**

```bash
git add -A cpp_migration/src/ui cpp_migration/tests
git commit -m "feat(ui): consolidate task views"
```

## Task 11: Integrate Team And Swarm UI

**Files:**

- Modify: `cpp_migration/src/CMakeLists.txt`
- Modify:
  - `cpp_migration/src/ui/teams/team_status.cppm`
  - `cpp_migration/src/ui/screens/repl_screen.cppm`
- Add/register:
  - `cpp_migration/src/ui/teams/teams_overview.cppm`
  - `cpp_migration/src/ui/teams/team_details_dialog.cppm`
  - `cpp_migration/src/ui/teams/swarm_collaboration_view.cppm`
- Test: `cpp_migration/tests/test_ui.cpp`

**Step 1: Add `teams_overview.cppm` first**

It is imported by both other unregistered team modules.

**Step 2: Build**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target cc_ui
```

**Step 3: Add details and swarm views**

Add each module separately. Resolve imports against `cc.ui.team_status` and existing agent view components.

**Step 4: Route team modes from `repl_screen.cppm`**

Add visible routes only after modules compile.

**Step 5: Test**

Run:

```bash
cmake --build cpp_migration/build/clang-debug --target test_ui
ctest --test-dir cpp_migration/build/clang-debug -R "Team|Swarm|AppRuntime" --output-on-failure
```

**Step 6: Commit**

```bash
git add cpp_migration/src/CMakeLists.txt cpp_migration/src/ui cpp_migration/tests/test_ui.cpp
git commit -m "feat(ui): wire team and swarm views"
```

## Task 12: Align UI Runtime Gate With Current Behavior

**Files:**

- Modify: `scripts/cpp-migration-ctest-gates.mjs`
- Modify if behavior is missing: `cpp_migration/tests/test_ui.cpp`

**Step 1: List current UI runtime tests**

Run:

```bash
ctest --test-dir cpp_migration/build/clang-debug -N -R "Terminal\\.StatusBar|Components\\.RenderPermissionPrompt|AppRuntime\\."
```

Current expected list includes:

- `Terminal.StatusBarRendersTokensAndCost`
- `Components.RenderPermissionPromptReturnsElement`
- `AppRuntime.CtrlCWhileStreamingQueryCancelsWithoutExiting`
- `AppRuntime.StreamingToolUseShowsSpinnerAndLoadingState`
- `AppRuntime.StreamingThinkingShowsSpinnerAndFinalContent`
- `AppRuntime.PermissionCallbackRendersAndResolvesUserChoices`
- `AppRuntime.RenderMessageShowsThinkingContent`
- `AppRuntime.RenderMessageShowsToolUseContent`
- `AppRuntime.RenderMessageShowsAssistantText`

**Step 2: Decide whether to rename tests or update gate expectations**

Prefer updating the gate if current test names better describe behavior. Prefer adding a combined test if the gate requires a behavior that current tests split across multiple tests.

**Step 3: Run the UI gate only**

Run:

```bash
ctest --test-dir cpp_migration/build/clang-debug -R "Terminal\\.StatusBar|Components\\.RenderPermissionPrompt|AppRuntime\\." --output-on-failure
```

Expected: all discovered UI runtime tests pass.

**Step 4: Run full migration CTest gates**

Run:

```bash
CC_REPL_CMAKE_BUILD_DIR=cpp_migration/build/clang-debug node scripts/cpp-migration-ctest-gates.mjs
```

Expected: no missing-test discovery failures.

**Step 5: Commit**

```bash
git add scripts/cpp-migration-ctest-gates.mjs cpp_migration/tests/test_ui.cpp
git commit -m "test(cpp-migration): align UI runtime gate"
```

## Task 13: Make Inventory Strict Actionable

**Files:**

- Modify: `scripts/cpp-migration-inventory.mjs`
- Modify: `cpp_migration/docs/unregistered-modules-decision-register.md`

**Step 1: Add an explicit temporary allowlist only for documented deferrals**

Allowed temporary entries should be rare and linked to a decision-register row. Do not allowlist stale skeletons.

Candidate temporary allowlist entries only if they remain intentionally deferred:

- `ui/dialogs/install_github_app_wizard.cppm`
- `ui/dialogs/install_slack_app_wizard.cppm`

**Step 2: Fail on undocumented unregistered files**

The script should continue to fail strict mode for:

- Any unregistered file not in the allowlist.
- Any allowlisted file missing from the decision register.
- Any allowlist entry whose file no longer exists.

**Step 3: Verify**

Run:

```bash
node scripts/cpp-migration-inventory.mjs --strict
```

Expected: passes only after all non-deferred files are either registered or removed.

**Step 4: Commit**

```bash
git add scripts/cpp-migration-inventory.mjs cpp_migration/docs/unregistered-modules-decision-register.md
git commit -m "test(cpp-migration): require documented unregistered modules"
```

## Task 14: Final Full Verification

**Files:**

- No planned source edits.

**Step 1: Run inventory strict**

```bash
node scripts/cpp-migration-inventory.mjs --strict
```

Expected: exit 0.

**Step 2: Build migration test targets**

```bash
cmake --build cpp_migration/build/clang-debug --target test_bridge test_services test_state test_tools test_ui
```

Expected: exit 0.

**Step 3: Run migration CTest gates**

```bash
CC_REPL_CMAKE_BUILD_DIR=cpp_migration/build/clang-debug node scripts/cpp-migration-ctest-gates.mjs
```

Expected: exit 0.

**Step 4: Run C++ migration E2E**

```bash
node scripts/cpp-migration-e2e.mjs
```

Expected: exit 0. If this requires a `dist/cc-repl` binary, run the repo build command first.

**Step 5: Run full CTest suite if feasible**

```bash
ctest --test-dir cpp_migration/build/clang-debug --output-on-failure
```

Expected: all tests pass.

**Step 6: Commit verification docs if updated**

```bash
git status --short
```

Only commit source, test, script, and docs changes related to this plan.

## Final Acceptance Criteria

- `node scripts/cpp-migration-inventory.mjs --strict` exits 0.
- There are zero undocumented unregistered C++ module files.
- Historical skeleton modules are removed or archived outside the source build graph.
- Intentional deferrals are documented in `cpp_migration/docs/unregistered-modules-decision-register.md`.
- UI runtime gate no longer fails on missing expected tests.
- `CC_REPL_CMAKE_BUILD_DIR=cpp_migration/build/clang-debug node scripts/cpp-migration-ctest-gates.mjs` exits 0.
- Relevant UI routes no longer show "delegated to ..." fallback text when a migrated panel exists.
- The CMake graph and runtime imports agree on which modules are active.

## Execution Notes

- Work in small commits by domain. Do not mix skeleton deletion with UI integration.
- Avoid adding unregistered UI modules to CMake in bulk. Add one domain at a time and let compile errors guide the integration.
- Treat `repl_screen.cppm` as the active UI router unless a later architecture decision replaces it.
- Treat `query/query_engine.cppm`, `tools/tool.cppm`, `tools/bash_tool.cppm`, and `state/store.cppm` as active implementations unless current imports prove otherwise.
- Do not claim migration completion until final verification commands have been run fresh in the current branch.
