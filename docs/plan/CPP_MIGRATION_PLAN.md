# CC-REPL C++23 Migration Plan

> Generated: 2026-05-29
> Updated: 2026-06-03
> Status: **NATIVE RUNTIME SMOKE VALIDATED** — C++ build, CTest, strict inventory, native entrypoint E2E, and selected command dispatch pass. Functional parity is still in progress.

---

## Overview

This document tracks the complete migration plan from TypeScript (Bun + React/Ink) to C++23 (Modules + FTXUI).

### Current Inventory Snapshot

```
TS source files:              1,946
C++ source files:             1,093
C++ modules:                  1,091 .cppm
C++ implementation files:     2 .cpp
Command root overlap:         Name overlap only; not a feature-parity score
Tool root overlap:            Name overlap only; not a feature-parity score
Runtime command surface:      Typed C++ command registry with selected hardened local actions
Runtime tool surface:         Registered C++ tool registry with mixed real and adapter-backed tools
CMake registration gaps:      0
Blocking migration markers:   0
Compiler:                     LLVM Clang 22.1.6
Generator:                    Ninja
Build status:                 Native build, CTest, inventory, and E2E pass for smoke coverage
```

### Current Validation Gates

```bash
bun run build
cmake --build --preset clang-release
ctest --test-dir cpp_migration/build/clang-release --output-on-failure
node scripts/cpp-migration-inventory.mjs --strict
bun run migration:e2e
```

The default product entrypoint is `dist/cc-repl`. `dist/cli.js` is retained only as a compatibility launcher that delegates to the native binary.

`bun run typecheck` is not part of the native product gate and currently fails in the retained TypeScript diagnostic path because of missing TS modules, ES library target drift, React compiler-runtime typings, and union narrowing issues. Keep this visible if `start:ts` must remain a supported development path.

### Runtime Command Hardening

The command migration is no longer only an inventory pass. The previous readiness-level helper responses have been replaced by local behavior where the command can be completed without external services:

- `/ant-trace`, `/bughunter`, and `/perf-issue` collect real local diagnostic snapshots.
- `/break-cache` removes local cache directories or an explicit cache path.
- `/backfill-sessions` scans local session files and backfills assistant session history.
- `/debug-tool-call` validates and summarizes JSON tool-call payloads.
- `/mock-limits` and `/reset-limits` update the native rate-limit hook state.
- `/bridge` inspects IDE bridge lockfiles instead of returning a static status.
- `/extra-usage`, `/onboarding`, `/init-verifiers`, and `/create-moved-to-plugin-command` execute concrete local actions.
- `/autofix-pr` and `/commit-push-pr` perform Git/GitHub preflight checks; full PR automation still depends on authenticated external transports.

### Architecture Mapping

| TS Stack | C++23 Stack |
|----------|-------------|
| Bun runtime | Native binary |
| React + Ink | FTXUI 5.0.0 |
| Commander.js | Custom CLI parser |
| Zod | Struct validation |
| Anthropic SDK | libcurl + yyjson |
| MCP SDK | Custom MCP client |
| libuv (via Bun) | libuv (direct) |

---

## Phase 0: Infrastructure Hardening

**Goal**: Ensure current codebase compiles and links correctly.

### Tasks

- [x] **0.1** CMake configure validation
  - Run `cmake -B build -G Ninja` and ensure all 386 .cppm files are recognized
  - Fix any FILE_SET issues

- [x] **0.2** Fix all `import cc.core.*` residual references
  - ~32 files still reference old `cc.core.*` module paths
  - Update to new paths: `cc.constants.*`, `cc.types.*`, `cc.context.*`, etc.

- [x] **0.3** Establish CI baseline
  - GitHub Actions workflow with clang-18 + cmake 3.28+
  - Or local build script for rapid iteration

### Exit Criteria
- `cmake --build build` completes without module resolution errors
- All targets link (even if implementations are stubs)

---

## Phase 1: Utils Core Layer

**Goal**: Expand utils from 40→~160 files. This is the largest bottleneck — most other modules depend on utils.

### Tier 1: Zero-Dependency Foundations (~30 files)

- [x] **1.1** String/Array/Set utilities (8 files)
  - `string_utils.cppm` — advanced string operations (truncate, pad, repeat)
  - `array_utils.cppm` — array helpers (chunk, unique, flatten)
  - `set_utils.cppm` — set operations
  - `truncate.cppm` — smart text truncation with ANSI awareness
  - `slice_ansi.cppm` — ANSI-aware string slicing
  - `intl.cppm` — number/date formatting
  - `xml_utils.cppm` — XML generation/parsing helpers
  - `yaml.cppm` — YAML parser (minimal subset)

- [x] **1.2** Path/File/Environment (12 files)
  - `path_utils.cppm` — advanced path operations
  - `cwd.cppm` — working directory management
  - `xdg.cppm` — XDG base directory spec
  - `system_directories.cppm` — platform config/cache/data dirs
  - `tempfile.cppm` — temp file/dir creation with RAII cleanup
  - `file_read.cppm` — buffered file reading with encoding detection
  - `file_read_cache.cppm` — LRU cache for file reads
  - `file_state_cache.cppm` — file stat caching
  - `file_history.cppm` — file modification history tracking
  - `file_persistence.cppm` — durable write with atomic rename
  - `outputs_scanner.cppm` — scan directory for output files
  - `fs_operations.cppm` — high-level filesystem operations

- [x] **1.3** JSON/Serialization (4 files)
  - `json_read.cppm` — streaming JSON reader
  - `zod_to_json_schema.cppm` — schema generation from structs
  - `frontmatter_parser.cppm` — YAML frontmatter extraction
  - `lazy_schema.cppm` — deferred schema validation

- [x] **1.4** Logging/Error/Debug (6 files)
  - `errors_utils.cppm` — error classification and formatting
  - `debug.cppm` — debug output with namespace filtering
  - `debug_filter.cppm` — debug namespace pattern matching
  - `diag_logs.cppm` — diagnostic log collection
  - `error_log_sink.cppm` — error persistence to disk
  - `warning_handler.cppm` — warning deduplication and suppression

### Tier 2: System Interaction (~29 files)

- [x] **1.5** Process/Shell/Terminal (10 files)
  - `shell.cppm` — shell spawning and management
  - `shell_config.cppm` — shell detection and configuration
  - `exec_file.cppm` — exec with timeout and output capture
  - `exec_sync.cppm` — synchronous command execution
  - `find_executable.cppm` — PATH-based executable discovery
  - `which.cppm` — which(1) equivalent
  - `generic_process_utils.cppm` — process tree utilities
  - `graceful_shutdown.cppm` — SIGTERM/SIGINT handler registry
  - `signal_utils.cppm` — signal handling helpers
  - `sleep_utils.cppm` — precise sleep / timer utilities

- [x] **1.6** Git Operations (8 files)
  - `git_diff.cppm` — diff parsing and generation
  - `git_settings.cppm` — git config reading
  - `git_config_parser.cppm` — git config file parser
  - `git_filesystem.cppm` — .git directory operations
  - `gitignore.cppm` — .gitignore pattern matching
  - `detect_repository.cppm` — repo root detection
  - `get_worktree_paths.cppm` — worktree path resolution
  - `worktree_utils.cppm` — worktree management

- [x] **1.7** HTTP/Network (5 files)
  - `proxy_utils.cppm` — proxy configuration resolution
  - `ca_certs_config.cppm` — CA certificate bundle management
  - `mtls.cppm` — mutual TLS setup
  - `peer_address.cppm` — peer address parsing
  - `user_agent.cppm` — user-agent string construction

- [x] **1.8** Crypto/Auth (6 files)
  - `auth_utils.cppm` — authentication flow helpers
  - `auth_file_descriptor.cppm` — secure credential storage
  - `auth_portable.cppm` — cross-platform auth
  - `uuid_utils.cppm` — UUID v4/v7 generation
  - `fingerprint.cppm` — machine fingerprinting
  - `user_utils.cppm` — user identity resolution

### Tier 3: Business Utilities (~50 files)

- [x] **1.9** Model System (16 files)
  - `model/agent.cppm` — agent model selection
  - `model/aliases.cppm` — model alias resolution
  - `model/ant_models.cppm` — Anthropic model registry
  - `model/bedrock.cppm` — AWS Bedrock integration
  - `model/check_1m_access.cppm` — 1M context access check
  - `model/configs.cppm` — model configuration presets
  - `model/context_window_upgrade.cppm` — context window upgrade detection
  - `model/deprecation.cppm` — model deprecation warnings
  - `model/model.cppm` — core model abstraction
  - `model/model_allowlist.cppm` — allowed models list
  - `model/model_capabilities.cppm` — capability flags per model
  - `model/model_options.cppm` — model option parsing
  - `model/model_strings.cppm` — human-readable model names
  - `model/model_support_overrides.cppm` — per-org overrides
  - `model/providers.cppm` — provider abstraction (Anthropic/Bedrock/Vertex)
  - `model/validate_model.cppm` — model validation logic

- [x] **1.10** Markdown/Highlighting (4 files)
  - `markdown_utils.cppm` — Markdown rendering helpers
  - `markdown_config_loader.cppm` — Markdown config from CLAUDE.md
  - `cli_highlight.cppm` — syntax highlighting for terminal
  - `text_highlighting.cppm` — search result highlighting

- [x] **1.11** Configuration/Settings (6 files)
  - `config_utils.cppm` — config file loading/merging
  - `config_constants.cppm` — config key constants
  - `env_dynamic.cppm` — dynamic environment resolution
  - `env_utils.cppm` — environment variable helpers
  - `env_validation.cppm` — environment validation
  - `managed_env.cppm` — managed environment configuration

- [x] **1.12** Scheduling/Locking/Versioning (6 files)
  - `cron_utils.cppm` — cron expression parsing
  - `cron_scheduler.cppm` — cron job scheduler
  - `cron_tasks.cppm` — cron task definitions
  - `lockfile.cppm` — file-based locking
  - `semver.cppm` — semantic version parsing/comparison
  - `sequential.cppm` — sequential execution queue

- [x] **1.13** Session Management (10 files)
  - `session_state.cppm` — session state machine
  - `session_start.cppm` — session initialization
  - `session_restore.cppm` — session restoration from disk
  - `session_activity.cppm` — activity tracking
  - `session_env_vars.cppm` — session environment variables
  - `session_environment.cppm` — session environment setup
  - `session_url.cppm` — session URL generation
  - `session_storage_portable.cppm` — portable storage layer
  - `list_sessions.cppm` — session listing implementation
  - `concurrent_sessions.cppm` — multi-session management

- [x] **1.14** Remaining Business Utilities (~20 files)
  - `claudemd.cppm` — CLAUDE.md file parsing and loading
  - `thinking.cppm` — thinking block management
  - `effort.cppm` — effort level configuration
  - `plans.cppm` — plan mode state management
  - `diff_utils.cppm` — unified diff generation
  - `memoize.cppm` — function memoization
  - `stats_utils.cppm` — statistics computation
  - `heatmap.cppm` — activity heatmap generation
  - `treeify.cppm` — directory tree rendering
  - `render_options.cppm` — render configuration
  - `hyperlink.cppm` — terminal hyperlink generation
  - `theme.cppm` — theme system
  - `display_tags.cppm` — tag display formatting
  - `billing.cppm` — billing/usage tracking
  - `fast_mode.cppm` — fast mode configuration
  - `idle_timeout.cppm` — idle timeout management
  - `immediate_command.cppm` — immediate command execution
  - `image_resizer.cppm` — image resizing for API
  - `image_validation.cppm` — image format validation
  - `ripgrep_utils.cppm` — ripgrep invocation wrapper

### Phase 1 Summary
- **New files**: ~120
- **Running total**: ~506 .cppm files

---

## Phase 2: Tools Completion

**Goal**: Expand each tool module with prompt/security/validation logic.

### Tasks

- [x] **2.1** BashTool security layer (3 files)
  - `bash_permissions.cppm` — permission checking for commands
  - `bash_security.cppm` — dangerous command detection
  - `command_semantics.cppm` — command classification (read/write/destructive)

- [x] **2.2** FileEditTool completion (2 files)
  - `file_edit_types.cppm` — edit operation types
  - `file_edit_prompt.cppm` — prompt generation for file edits

- [x] **2.3** AgentTool sub-agent system (4 files)
  - `agent_types.cppm` — agent type definitions
  - `agent_prompt.cppm` — agent prompt construction
  - `built_in_agents.cppm` — built-in agent registry (explore, verify, plan)
  - `agent_utils.cppm` — agent lifecycle utilities

- [x] **2.4** ScriptTool completion (3 files)
  - `script_sandbox.cppm` — script sandboxing
  - `script_diagnostics.cppm` — diagnostic formatting
  - `script_types.cppm` — script execution types

- [x] **2.5** PowerShellTool completion (2 files)
  - `powershell_security.cppm` — PS-specific security checks
  - `powershell_permissions.cppm` — PS permission model

- [x] **2.6** MCP/LSP tool completion (3 files)
  - `mcp_classify.cppm` — MCP tool output classification
  - `lsp_formatters.cppm` — LSP response formatting
  - `lsp_symbol_context.cppm` — symbol context extraction

- [x] **2.7** Task tool group (3 files)
  - `task_create.cppm` — task creation logic (split from task_tool)
  - `task_get.cppm` — task retrieval
  - `task_list.cppm` — task listing with filters

- [x] **2.8** Tool shared layer (3 files)
  - `tool_prompts.cppm` — aggregated prompt definitions
  - `git_operation_tracking.cppm` — git op tracking for tools
  - `spawn_multi_agent.cppm` — multi-agent spawning

- [x] **2.9** Missing tool implementations (5 files)
  - `read_mcp_resource_tool.cppm` — MCP resource reading
  - `list_mcp_resources_tool.cppm` — MCP resource listing
  - `remote_trigger_tool.cppm` — remote agent trigger
  - `mcp_auth_tool.cppm` — MCP authentication
  - `synthetic_output_tool.cppm` — synthetic output generation

### Phase 2 Summary
- **New files**: ~28
- **Running total**: ~534 .cppm files

---

## Phase 3: Services Completion

**Goal**: Fill gaps in the service layer.

### Tasks

- [x] **3.1** API service completion (8 files)
  - `services/api/usage.cppm`
  - `services/api/files_api.cppm`
  - `services/api/grove.cppm`
  - `services/api/referral.cppm`
  - `services/api/admin_requests.cppm`
  - `services/api/session_ingress.cppm`
  - `services/api/metrics_opt_out.cppm`
  - `services/api/error_utils.cppm`

- [x] **3.2** MCP service completion (10 files)
  - `services/mcp/channel_allowlist.cppm`
  - `services/mcp/channel_notification.cppm`
  - `services/mcp/channel_permissions.cppm`
  - `services/mcp/elicitation_handler.cppm`
  - `services/mcp/env_expansion.cppm`
  - `services/mcp/normalization.cppm`
  - `services/mcp/official_registry.cppm`
  - `services/mcp/sdk_control_transport.cppm`
  - `services/mcp/mcp_utils.cppm`
  - `services/mcp/xaa.cppm`

- [x] **3.3** Analytics completion (4 files)
  - `services/analytics/metadata.cppm`
  - `services/analytics/sink.cppm`
  - `services/analytics/sink_killswitch.cppm`
  - `services/analytics/config.cppm`

- [x] **3.4** Compact service completion (6 files)
  - `services/compact/grouping.cppm`
  - `services/compact/micro_compact.cppm`
  - `services/compact/prompt.cppm`
  - `services/compact/post_compact_cleanup.cppm`
  - `services/compact/session_memory_compact.cppm`
  - `services/compact/warning_hook.cppm`

- [x] **3.5** LSP service completion (3 files)
  - `services/lsp/diagnostic_registry.cppm`
  - `services/lsp/manager.cppm`
  - `services/lsp/passive_feedback.cppm`

- [x] **3.6** OAuth completion (3 files)
  - `services/oauth/auth_code_listener.cppm`
  - `services/oauth/crypto.cppm`
  - `services/oauth/oauth_profile.cppm`

- [x] **3.7** AutoDream completion (3 files)
  - `services/auto_dream/config.cppm`
  - `services/auto_dream/consolidation_lock.cppm`
  - `services/auto_dream/consolidation_prompt.cppm`

- [x] **3.8** Tips service completion (3 files)
  - `services/tips/tip_history.cppm`
  - `services/tips/tip_registry.cppm`
  - `services/tips/tip_scheduler.cppm`

- [x] **3.9** Memory service completion (2 files)
  - `services/memory/prompts.cppm`
  - `services/memory/session_memory_utils.cppm`

- [x] **3.10** Plugin service (3 files)
  - `services/plugins/installation_manager.cppm`
  - `services/plugins/cli_commands.cppm`
  - `services/plugins/operations.cppm`

- [x] **3.11** Policy/RateLimit completion (3 files)
  - `services/policy/types.cppm`
  - `services/rate_limit/messages.cppm`
  - `services/rate_limit/mock.cppm`

- [x] **3.12** RemoteManagedSettings (4 files)
  - `services/remote_settings/sync_cache.cppm`
  - `services/remote_settings/sync_cache_state.cppm`
  - `services/remote_settings/security_check.cppm`
  - `services/remote_settings/types.cppm`

### Phase 3 Summary
- **New files**: ~52
- **Running total**: ~586 .cppm files

---

## Phase 4: Hooks Completion

**Goal**: Expand hooks from 32→~62 modules.

### Tasks

- [x] **4.1** Notification hooks (8 files)
  - `hooks/notifs/auto_mode_unavailable.cppm`
  - `hooks/notifs/deprecation_warning.cppm`
  - `hooks/notifs/fast_mode.cppm`
  - `hooks/notifs/ide_status_indicator.cppm`
  - `hooks/notifs/install_messages.cppm`
  - `hooks/notifs/lsp_initialization.cppm`
  - `hooks/notifs/rate_limit_warning.cppm`
  - `hooks/notifs/startup.cppm`

- [x] **4.2** Permission system hooks (3 files)
  - `hooks/tool_permission/coordinator_handler.cppm`
  - `hooks/tool_permission/interactive_handler.cppm`
  - `hooks/tool_permission/swarm_worker_handler.cppm`

- [x] **4.3** Session/Remote hooks (4 files)
  - `hooks/session_backgrounding.cppm`
  - `hooks/remote_session.cppm`
  - `hooks/ssh_session.cppm`
  - `hooks/teleport_resume.cppm`

- [x] **4.4** IDE integration hooks (3 files)
  - `hooks/ide_at_mentioned.cppm`
  - `hooks/ide_connection_status.cppm`
  - `hooks/ide_logging.cppm`

- [x] **4.5** Plugin/Skill hooks (3 files)
  - `hooks/skills_change.cppm`
  - `hooks/plugin_recommendation.cppm`
  - `hooks/marketplace_notification.cppm`

- [x] **4.6** UI interaction hooks (5 files)
  - `hooks/clipboard_image_hint.cppm`
  - `hooks/copy_on_select.cppm`
  - `hooks/double_press.cppm`
  - `hooks/blink.cppm`
  - `hooks/elapsed_time.cppm`

- [x] **4.7** Data/Analytics hooks (4 files)
  - `hooks/log_messages.cppm`
  - `hooks/dynamic_config.cppm`
  - `hooks/api_key_verification.cppm`
  - `hooks/cost_hook.cppm`

### Phase 4 Summary
- **New files**: ~30
- **Running total**: ~616 .cppm files

---

## Phase 5: Commands Completion

**Goal**: Expand commands from 68→~90 modules.

### Tasks

- [x] **5.1** install-github-app flow (3 files)
  - `commands/install_github_app/flow.cppm`
  - `commands/install_github_app/steps.cppm`
  - `commands/install_github_app/github_actions.cppm`

- [x] **5.2** Plugin command completion (4 files)
  - `commands/plugin/browse_marketplace.cppm`
  - `commands/plugin/manage_plugins.cppm`
  - `commands/plugin/plugin_install.cppm`
  - `commands/plugin/plugin_trust.cppm`

- [x] **5.3** Review command completion (2 files)
  - `commands/review/ultrareview.cppm`
  - `commands/review/review_remote.cppm`

- [x] **5.4** MCP command completion (2 files)
  - `commands/mcp/add_command.cppm`
  - `commands/mcp/xaa_idp.cppm`

- [x] **5.5** Remote commands (3 files)
  - `commands/remote_env.cppm`
  - `commands/remote_setup.cppm`
  - `commands/remote_setup_api.cppm`

- [x] **5.6** Remaining commands (8 files)
  - `commands/exit.cppm`
  - `commands/keybindings_cmd.cppm`
  - `commands/privacy_settings.cppm`
  - `commands/rate_limit_options.cppm`
  - `commands/release_notes.cppm`
  - `commands/sandbox_toggle.cppm`
  - `commands/stickers.cppm`
  - `commands/thinkback.cppm`

### Phase 5 Summary
- **New files**: ~22
- **Running total**: ~638 .cppm files

---

## Phase 6: CLI Layer

**Goal**: Expand CLI from 1→~19 modules.

### Tasks

- [x] **6.1** Transport layer (5 files)
  - `cli/sse_transport.cppm`
  - `cli/websocket_transport.cppm`
  - `cli/hybrid_transport.cppm`
  - `cli/worker_state_uploader.cppm`
  - `cli/ccr_client.cppm`

- [x] **6.2** CLI handlers (6 files)
  - `cli/handlers/agents.cppm`
  - `cli/handlers/auth.cppm`
  - `cli/handlers/auto_mode.cppm`
  - `cli/handlers/mcp_handler.cppm`
  - `cli/handlers/plugins_handler.cppm`
  - `cli/handlers/template_jobs.cppm`

- [x] **6.3** CLI output/IO (5 files)
  - `cli/print.cppm`
  - `cli/structured_io.cppm`
  - `cli/remote_io.cppm`
  - `cli/ndjson_stringify.cppm`
  - `cli/exit.cppm`

- [x] **6.4** Background processes (2 files)
  - `cli/bg.cppm`
  - `cli/update.cppm`

### Phase 6 Summary
- **New files**: ~18
- **Running total**: ~656 .cppm files

---

## Phase 7: Bridge Completion

**Goal**: Expand bridge from 12→~25 modules.

### Tasks

- [x] **7.1** Bridge auth/security (4 files)
  - `bridge/jwt_utils.cppm`
  - `bridge/work_secret.cppm`
  - `bridge/trusted_device.cppm`
  - `bridge/session_id_compat.cppm`

- [x] **7.2** Bridge messaging (4 files)
  - `bridge/inbound_messages.cppm`
  - `bridge/inbound_attachments.cppm`
  - `bridge/flush_gate.cppm`
  - `bridge/capacity_wake.cppm`

- [x] **7.3** Bridge configuration (3 files)
  - `bridge/poll_config.cppm`
  - `bridge/poll_config_defaults.cppm`
  - `bridge/envless_config.cppm`

- [x] **7.4** Bridge debug/status (2 files)
  - `bridge/debug_utils.cppm`
  - `bridge/status_util.cppm`

### Phase 7 Summary
- **New files**: ~13
- **Running total**: ~669 .cppm files

---

## Phase 8: Skills System

**Goal**: Expand skills from 2→~12 modules.

### Tasks

- [x] **8.1** Skill loading/discovery (2 files)
  - `skills/load_skills_dir.cppm`
  - `skills/mcp_skill_builders.cppm`

- [x] **8.2** Bundled skills (8 files)
  - `skills/bundled/batch.cppm`
  - `skills/bundled/claude_api.cppm`
  - `skills/bundled/debug.cppm`
  - `skills/bundled/loop.cppm`
  - `skills/bundled/remember.cppm`
  - `skills/bundled/simplify.cppm`
  - `skills/bundled/stuck.cppm`
  - `skills/bundled/verify.cppm`

### Phase 8 Summary
- **New files**: ~10
- **Running total**: ~679 .cppm files

---

## Phase 9: UI Layer (FTXUI Redesign)

**Goal**: Not a 1:1 migration — redesign UI with FTXUI idioms.

### Tasks

- [x] **9.1** Design system base components (8 files)
  - `ui/design/dialog.cppm`
  - `ui/design/divider.cppm`
  - `ui/design/progress_bar.cppm`
  - `ui/design/tabs.cppm`
  - `ui/design/themed_box.cppm`
  - `ui/design/themed_text.cppm`
  - `ui/design/list_item.cppm`
  - `ui/design/status_icon.cppm`

- [x] **9.2** PromptInput full implementation (5 files)
  - `ui/prompt/footer.cppm`
  - `ui/prompt/notifications.cppm`
  - `ui/prompt/shimmer.cppm`
  - `ui/prompt/voice_indicator.cppm`
  - `ui/prompt/mode_indicator.cppm`

- [x] **9.3** Message rendering system (5 files)
  - `ui/messages/message_row.cppm`
  - `ui/messages/message_response.cppm`
  - `ui/messages/message_timestamp.cppm`
  - `ui/messages/tool_use_loader.cppm`
  - `ui/messages/structured_diff.cppm`

- [x] **9.4** Dialog system (6 files)
  - `ui/dialogs/permission_dialog.cppm`
  - `ui/dialogs/trust_dialog.cppm`
  - `ui/dialogs/model_picker.cppm`
  - `ui/dialogs/quick_open.cppm`
  - `ui/dialogs/help_v2.cppm`
  - `ui/dialogs/onboarding.cppm`

- [x] **9.5** termio layer completion (3 files)
  - `ui/termio/csi.cpp`
  - `ui/termio/dec.cpp`
  - `ui/termio/ansi.cpp`

- [x] **9.6** Layout engine completion (3 files)
  - `ui/layout/yoga.cppm` — flexbox-like layout algorithm
  - `ui/layout/measure.cppm` — element measurement
  - `ui/layout/wrap_text.cppm` — text wrapping with ANSI

- [x] **9.7** Event system implementation (3 files)
  - `ui/events/click_event.cpp`
  - `ui/events/focus_event.cpp`
  - `ui/events/keyboard_event.cpp`

### Phase 9 Summary
- **New files**: ~33
- **Running total**: ~712 .cppm files

---

## Phase 10: Tail Directory Migration

**Goal**: Cover remaining unmigrated directories.

### Tasks

- [x] **10.1** vim/ mode (3 files)
  - `vim/vim_mode.cppm` — vim mode state machine
  - `vim/vim_commands.cppm` — ex commands
  - `vim/vim_motions.cppm` — motion/text objects

- [x] **10.2** daemon/ (2 files)
  - `daemon/daemon_server.cppm` — background daemon process
  - `daemon/daemon_client.cppm` — daemon IPC client

- [x] **10.3** remote/ (3 files)
  - `remote/remote_session.cppm` — remote session management
  - `remote/remote_connection.cppm` — connection lifecycle
  - `remote/remote_auth.cppm` — remote authentication

- [x] **10.4** migrations/ (3 files)
  - `migrations/migration_runner.cppm` — migration execution engine
  - `migrations/migration_registry.cppm` — migration discovery
  - `migrations/schema_versions.cppm` — schema version tracking

- [x] **10.5** server/ (2 files)
  - `server/server_main.cppm` — HTTP server (headless mode)
  - `server/server_routes.cppm` — route definitions

- [x] **10.6** screens/ completion (2 files)
  - `screens/doctor_screen.cppm` — doctor diagnostic screen
  - `screens/resume_screen.cppm` — session resume screen

- [x] **10.7** schemas/ (1 file)
  - `schemas/validation_schemas.cppm` — shared validation schemas

### Phase 10 Summary
- **New files**: ~16
- **Running total**: ~728 .cppm files

---

## Phase 11: Integration & Verification

**Goal**: End-to-end validation and optimization.

### Tasks

- [x] **11.1** Full compilation verification
  - All ~728 .cppm files compile without errors
  - Zero undefined symbol errors at link time

- [x] **11.2** Basic smoke test
  - Launch → display prompt → accept input → send to API → render response → exit
  - Slash commands: `/help`, `/clear`, `/compact`, `/model`

- [x] **11.3** Performance baseline
  - Startup time target: <50ms (vs ~200ms TS)
  - Memory usage target: <30MB idle (vs ~150MB TS)
  - First token latency: no regression vs TS

- [x] **11.4** Module dependency graph cleanup
  - No circular dependencies
  - Dependency depth ≤ 5 levels
  - Generate dependency visualization

- [ ] **11.5** API integration test
  - Real Anthropic API call with streaming
  - Tool use round-trip (bash, file read, grep)
  - Multi-turn conversation

- [ ] **11.6** Feature parity checklist
  - [x] Interactive REPL mode
  - [x] Slash commands (core subset)
  - [x] Tool execution (Bash, File*, Glob, Grep)
  - [ ] Streaming response rendering parity
  - [ ] Session persistence parity
  - [ ] MCP server connection lifecycle
  - [ ] Permission system parity
  - [ ] Keyboard shortcuts parity
  - [ ] Thinking mode display parity
  - [ ] Cost tracking parity
  - [ ] Auto-compact parity

---

## Projection

| Historical phase estimate | New Files | Cumulative | Estimated coverage |
|-------|-----------|------------|----------|
| Current | — | 386 | ~13% |
| Phase 0 | 0 (fixes) | 386 | ~13% |
| Phase 1 | ~120 | 506 | ~17% |
| Phase 2 | ~28 | 534 | ~18% |
| Phase 3 | ~52 | 586 | ~20% |
| Phase 4 | ~30 | 616 | ~21% |
| Phase 5 | ~22 | 638 | ~22% |
| Phase 6 | ~18 | 656 | ~22% |
| Phase 7 | ~13 | 669 | ~23% |
| Phase 8 | ~10 | 679 | ~23% |
| Phase 9 | ~33 | 712 | ~24% |
| Phase 10 | ~16 | 728 | ~25% |
| **Original final estimate** | **~342** | **~728** | **~25% files / ~85% functional** |

> Current validation no longer uses the historical phase estimate as the completion gate.
> The active gate is strict inventory plus native build/test/E2E smoke coverage:
> no CMake registration gaps, no blocking migration markers, key runtime surfaces present,
> and targeted behavior tests for migrated functionality.

---

## Priority Matrix

```
         HIGH IMPACT
              │
   Phase 1    │    Phase 9
   (utils)    │    (UI)
              │
LOW EFFORT ───┼─── HIGH EFFORT
              │
   Phase 0    │    Phase 3
   (infra)    │    (services)
              │
         LOW IMPACT
```

**Critical path**: Phase 0 → Phase 1 → Phase 2 → Phase 11 (minimal viable product)

**Parallelizable**: Phase 3-8 can proceed concurrently after Phase 1 Tier 2 completes.

---

## Conventions

### Module Naming
```
cc.<directory>.<file_name>

Examples:
  cc.utils.string_utils
  cc.tools.bash_tool
  cc.services.api.client
  cc.hooks.text_input
```

### File Naming
- snake_case for all .cppm files
- Match TS filename where possible (camelCase → snake_case)
- Suffix `_utils` only when disambiguating from a type/class of the same name

### Dependencies
- Each library target links only what it directly uses
- No target may depend on `cc_core` except leaf targets (cc_app, cc_hooks)
- Prefer fine-grained imports over blanket `cc_core` dependency
