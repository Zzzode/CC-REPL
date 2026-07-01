# ─── CC-REPL Module Library Targets ───────────────────────────────────────────
# Each target corresponds to a C++23 module partition of the system.
# All .cppm files in each directory are registered as CXX_MODULES sources.

# ─── NOTE: Ninja Job Pools are now configured at the TOP LEVEL CMakeLists.txt
#     (project root), immediately after the project() call.  The GLOBAL
#     set_property(JOB_POOLS) and the CMAKE_NINJA_JOB_POOL_* variables live
#     there so that ALL sibling subdirectories (src/, tests/, benchmarks/)
#     observe the same CC_REPL_SCAN_JOBS cap. This keeps P1689 depfile
#     scanning bounded for every target under this project. Do NOT re-declare JOB_POOLS
#     here; APPENDing to the GLOBAL property twice just duplicates entries.
# ─────────────────────────────────────────────────────────────────────────────

# ─── Optional tree-sitter dependency for bash AST danger classifier ───────────
option(CC_ENABLE_TREE_SITTER "Enable tree-sitter bash parsing for AST danger classifier" OFF)
if(CC_ENABLE_TREE_SITTER)
    # tree-sitter is wired with the deprecated FetchContent_Populate + manual
    # library target pattern. CMake 4.x sets CMP0169 to NEW which forbids
    # FetchContent_Populate; allow it here until the wiring is modernized.
    cmake_policy(SET CMP0169 OLD)
    include(FetchContent)
    FetchContent_Declare(
        tree_sitter
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
        GIT_TAG        v0.22.6
    )
    FetchContent_Declare(
        tree_sitter_bash
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
        GIT_TAG        v0.23.1
    )
    FetchContent_GetProperties(tree_sitter)
    if(NOT tree_sitter_POPULATED)
        FetchContent_Populate(tree_sitter)
    endif()
    FetchContent_GetProperties(tree_sitter_bash)
    if(NOT tree_sitter_bash_POPULATED)
        FetchContent_Populate(tree_sitter_bash)
    endif()

    # Build tree-sitter core as a static library from source.
    add_library(tree-sitter STATIC
        ${tree_sitter_SOURCE_DIR}/lib/src/alloc.c
        ${tree_sitter_SOURCE_DIR}/lib/src/get_changed_ranges.c
        ${tree_sitter_SOURCE_DIR}/lib/src/language.c
        ${tree_sitter_SOURCE_DIR}/lib/src/lexer.c
        ${tree_sitter_SOURCE_DIR}/lib/src/lib.c
        ${tree_sitter_SOURCE_DIR}/lib/src/node.c
        ${tree_sitter_SOURCE_DIR}/lib/src/parser.c
        ${tree_sitter_SOURCE_DIR}/lib/src/query.c
        ${tree_sitter_SOURCE_DIR}/lib/src/stack.c
        ${tree_sitter_SOURCE_DIR}/lib/src/subtree.c
        ${tree_sitter_SOURCE_DIR}/lib/src/tree.c
        ${tree_sitter_SOURCE_DIR}/lib/src/tree_cursor.c
        ${tree_sitter_SOURCE_DIR}/lib/src/wasm_store.c
    )
    target_include_directories(tree-sitter PUBLIC
        ${tree_sitter_SOURCE_DIR}/lib/include
    )
    set_target_properties(tree-sitter PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
    )
    if(MSVC)
        target_compile_options(tree-sitter PRIVATE /wd4101 /wd4244 /wd4267 /wd4457)
    else()
        target_compile_options(tree-sitter PRIVATE -w)
    endif()

    # Build tree-sitter-bash language parser as a static library.
    add_library(tree-sitter-bash STATIC
        ${tree_sitter_bash_SOURCE_DIR}/src/parser.c
        ${tree_sitter_bash_SOURCE_DIR}/src/scanner.c
    )
    target_include_directories(tree-sitter-bash PUBLIC
        ${tree_sitter_bash_SOURCE_DIR}/src
    )
    target_link_libraries(tree-sitter-bash PUBLIC tree-sitter)
    set_target_properties(tree-sitter-bash PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
    )
    if(MSVC)
        target_compile_options(tree-sitter-bash PRIVATE /wd4101 /wd4244 /wd4267)
    else()
        target_compile_options(tree-sitter-bash PRIVATE -w)
    endif()
endif()

# ─── cc_utils: Utilities (no internal deps) ───────────────────────────────────
add_library(cc_utils)
target_sources(cc_utils
    PUBLIC FILE_SET CXX_MODULES FILES
        utils/agent_model.cppm
        utils/activity_manager.cppm
        utils/agent_id.cppm
        utils/agent_swarms_enabled.cppm
        utils/argument_substitution.cppm
        utils/array_utils.cppm
        utils/async.cppm
        utils/auth_file_descriptor.cppm
        utils/auth_portable.cppm
        utils/auth_utils.cppm
        utils/abort_controller.cppm
        utils/auto_mode_denials.cppm
        utils/bash_execution.cppm
        utils/bash_parser.cppm
        utils/bash_security.cppm
        utils/bash_shell_quoting.cppm
        utils/billing.cppm
        utils/binary_check.cppm
        utils/ca_certs_config.cppm
        utils/cache.cppm
        utils/cache_paths.cppm
        utils/circular_buffer.cppm
        utils/claude_code_hints.cppm
        utils/claudemd.cppm
        utils/chrome_messaging.cppm
        utils/cli_highlight.cppm
        utils/collapse_read_search.cppm
        utils/collapse_notifications.cppm
        utils/commit_attribution.cppm
        utils/code_indexing.cppm
        utils/concurrent_sessions.cppm
        utils/content_array.cppm
        utils/control_message_compat.cppm
        utils/config_constants.cppm
        utils/config_utils.cppm
        utils/cron_scheduler.cppm
        utils/cron_tasks.cppm
        utils/cron_utils.cppm
        utils/crypto.cppm
        utils/cwd.cppm
        utils/debug.cppm
        utils/debug_filter.cppm
        utils/deep_link.cppm
        utils/detect_repository.cppm
        utils/dxt_helpers.cppm
        utils/diag_logs.cppm
        utils/diff_utils.cppm
        utils/display_tags.cppm
        utils/editor_utils.cppm
        utils/effort.cppm
        utils/env.cppm
        utils/env_dynamic.cppm
        utils/env_utils.cppm
        utils/env_validation.cppm
        utils/error.cppm
        utils/error_log_sink.cppm
        utils/errors_utils.cppm
        utils/exec_file.cppm
        utils/exec_sync.cppm
        utils/fast_mode.cppm
        utils/file.cppm
        utils/file_edit_utils.cppm
        utils/file_history.cppm
        utils/file_persistence.cppm
        utils/file_read.cppm
        utils/file_read_cache.cppm
        utils/file_state_cache.cppm
        utils/find_executable.cppm
        utils/fingerprint.cppm
        utils/fps_tracker.cppm
        utils/format.cppm
        utils/forked_agent.cppm
        utils/frontmatter_parser.cppm
        utils/fs_operations.cppm
        utils/generic_process_utils.cppm
        utils/get_worktree_paths.cppm
        utils/git.cppm
        utils/git_config_parser.cppm
        utils/git_diff.cppm
        utils/git_filesystem.cppm
        utils/git_settings.cppm
        utils/github_utils.cppm
        utils/gitignore.cppm
        utils/glob_utils.cppm
        utils/graceful_shutdown.cppm
        utils/hash.cppm
        utils/clipboard.cppm
        utils/heatmap.cppm
        utils/http.cppm
        utils/http_encoding.cppm
        utils/hyperlink.cppm
        utils/idle_timeout.cppm
        utils/image_handling.cppm
        utils/image_resizer.cppm
        utils/image_validation.cppm
        utils/immediate_command.cppm
        utils/input_router.cppm
        utils/intl.cppm
        utils/json.cppm
        utils/skill_usage.cppm
        utils/list_sessions.cppm
        utils/lockfile.cppm
        utils/log.cppm
        utils/managed_env.cppm
        utils/markdown_config_loader.cppm
        utils/markdown_utils.cppm
        utils/mcp_helpers.cppm
        utils/mcp_validation.cppm
        utils/memdir.cppm
        utils/memoize.cppm
        utils/memory.cppm
        utils/memory_file_detection.cppm
        utils/message_mappers.cppm
        utils/model/agent.cppm
        utils/model/aliases.cppm
        utils/model/ant_models.cppm
        utils/model/bedrock.cppm
        utils/model/check_1m_access.cppm
        utils/model/configs.cppm
        utils/model/context_window_upgrade.cppm
        utils/model/deprecation.cppm
        utils/model/model.cppm
        utils/model/model_allowlist.cppm
        utils/model/model_capabilities.cppm
        utils/model/model_options.cppm
        utils/model/model_strings.cppm
        utils/model/model_support_overrides.cppm
        utils/model/providers.cppm
        utils/model/validate_model.cppm
        utils/model_aliases.cppm
        utils/model_cost.cppm
        utils/message_predicates.cppm
        utils/mtls.cppm
        utils/native_installer.cppm
        utils/native_utils.cppm
        utils/outputs_scanner.cppm
        utils/object_group_by.cppm
        utils/path.cppm
        utils/path_utils.cppm
        utils/pdf.cppm
        utils/parse_references.cppm
        utils/peer_address.cppm
        utils/permissions.cppm
        utils/permissions_engine.cppm
        utils/plans.cppm
        utils/plugin_dependency_resolver.cppm
        utils/plugin_identifier.cppm
        utils/plugin_marketplace_rules.cppm
        utils/plugin_versioning.cppm
        utils/platform.cppm
        utils/powershell_parser.cppm
        utils/process.cppm
        utils/privacy_level.cppm
        utils/prompt_category.cppm
        utils/proxy_utils.cppm
        utils/query_guard.cppm
        utils/query_helpers.cppm
        utils/read_file_in_range.cppm
        utils/render_options.cppm
        utils/ripgrep_utils.cppm
        utils/sandbox.cppm
        utils/semantic_boolean.cppm
        utils/semantic_number.cppm
        utils/sanitization.cppm
        utils/script_tool_enabled.cppm
        utils/semver.cppm
        utils/sequential.cppm
        utils/session.cppm
        utils/session_activity.cppm
        utils/session_env_vars.cppm
        utils/session_environment.cppm
        utils/session_helpers.cppm
        utils/session_restore.cppm
        utils/session_start.cppm
        utils/session_state.cppm
        utils/session_storage.cppm
        utils/session_storage_portable.cppm
        utils/session_url.cppm
        utils/set_utils.cppm
        utils/settings_merge.cppm
        utils/settings_paths.cppm
        utils/settings_sources.cppm
        utils/settings_validation.cppm
        utils/shell.cppm
        utils/shell_config.cppm
        utils/shell_parser.cppm
        utils/shell_rule_matching.cppm
        utils/signal_utils.cppm
        utils/slash_command_parsing.cppm
        utils/sleep_utils.cppm
        utils/slice_ansi.cppm
        utils/ssrf_guard.cppm
        utils/stats_utils.cppm
        utils/stream_utils.cppm
        utils/string.cppm
        utils/string_utils.cppm
        utils/suggestions.cppm
        utils/swarm.cppm
        utils/system_directories.cppm
        utils/system_prompt.cppm
        utils/system_theme.cppm
        utils/task_output.cppm
        utils/task_utils.cppm
        utils/tagged_id.cppm
        utils/team_helpers.cppm
        utils/teleport_utils.cppm
        utils/tempfile.cppm
        utils/terminal.cppm
        utils/terminal_helpers.cppm
        utils/text_highlighting.cppm
        utils/theme.cppm
        utils/thinking.cppm
        utils/timeouts.cppm
        utils/token_budget.cppm
        utils/tokens.cppm
        utils/tool_helpers.cppm
        utils/treeify.cppm
        utils/truncate.cppm
        utils/user_agent.cppm
        utils/user_utils.cppm
        utils/utils.cppm
        utils/words.cppm
        utils/uuid_utils.cppm
        utils/warning_handler.cppm
        utils/which.cppm
        utils/worktree_utils.cppm
        utils/xdg.cppm
        utils/xml_utils.cppm
        utils/yaml.cppm
        utils/zod_to_json_schema.cppm
        utils/hooks_config.cppm
        utils/hooks_execution.cppm
        utils/hooks_registry.cppm
        utils/statusline_runner.cppm
        utils/plugin_lifecycle.cppm
        utils/plugin_loader.cppm
        utils/plugin_manager.cppm
        utils/plugin_marketplace.cppm
        utils/plugin_marketplace_lifecycle.cppm
        utils/plugin_validation.cppm
        utils/settings_manager.cppm
        utils/settings_rules.cppm
        utils/shell_providers.cppm
        utils/shell_tools.cppm
        utils/swarm_backends.cppm
        utils/swarm_coordination.cppm
        utils/swarm_helpers.cppm
        utils/telemetry.cppm
        utils/telemetry_exporters.cppm
        utils/ultraplan.cppm
        utils/ansi_rendering.cppm
        utils/cleanup_registry.cppm
        utils/prompt_helpers.cppm
        utils/tool_management.cppm
        utils/team_coordination.cppm
        utils/profiling.cppm
        utils/session_management.cppm
        utils/stream_helpers.cppm
        utils/context_helpers.cppm
        utils/mcp_transport.cppm
        utils/terminal_integration.cppm
        utils/image_store.cppm
        utils/classifier_approvals.cppm
        utils/side_query.cppm
        utils/agent_context.cppm
        utils/platform_paths.cppm
        utils/cron_helpers.cppm
        utils/file_index.cppm
        utils/tree_sitter/tree_sitter.cppm
        utils/tree_sitter/bash/ast.cppm
)
target_sources(cc_utils
    PRIVATE
        utils/json_impl.cpp
        utils/skill_usage_impl.cpp
)
set_source_files_properties(utils/swarm_backends.cppm PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang>:-O0>"
)
target_link_libraries(cc_utils
    PUBLIC
        yyjson
        uv_a
        httplib::httplib
)
if(CC_ENABLE_TREE_SITTER)
    target_link_libraries(cc_utils PUBLIC tree-sitter tree-sitter-bash)
    target_compile_definitions(cc_utils PUBLIC CC_HAS_TREE_SITTER=1)
else()
    target_compile_definitions(cc_utils PUBLIC CC_HAS_TREE_SITTER=0)
endif()
if(APPLE)
    target_link_libraries(cc_utils PUBLIC "-framework CoreFoundation")
endif()

# ─── cc_state: State Management ───────────────────────────────────────────────
add_library(cc_state)
target_sources(cc_state
    PUBLIC FILE_SET CXX_MODULES FILES
        state/app_state.cppm
        state/ftxui_integration.cppm
        state/on_change_app_state.cppm
        state/persistence.cppm
        state/selectors.cppm
        state/store.cppm
        state/teammate_view_helpers.cppm
)
target_link_libraries(cc_state
    PUBLIC
        cc_utils
        cc_types
        cc_task_types
        yyjson
)

# ─── cc_task_types: Canonical Task Data Model ─────────────────────────────────
add_library(cc_task_types)
target_sources(cc_task_types
    PUBLIC FILE_SET CXX_MODULES FILES
        task_types.cppm
)
target_link_libraries(cc_task_types PUBLIC cc_utils)

# ─── cc_history: Canonical Conversation History ────────────────────────────────
add_library(cc_history)
target_sources(cc_history
    PUBLIC FILE_SET CXX_MODULES FILES
        history.cppm
)
target_link_libraries(cc_history PUBLIC cc_utils)

# ─── cc_constants: Constants ──────────────────────────────────────────────────
add_library(cc_constants)
target_sources(cc_constants
    PUBLIC FILE_SET CXX_MODULES FILES
        constants/api_limits.cppm
        constants/betas.cppm
        constants/common.cppm
        constants/constants.cppm
        constants/cost_tracker.cppm
        constants/cyber_risk.cppm
        constants/error_ids.cppm
        constants/figures.cppm
        constants/files.cppm
        constants/github_app.cppm
        constants/keys.cppm
        constants/messages.cppm
        constants/oauth.cppm
        constants/output_styles.cppm
        constants/product.cppm
        constants/prompts.cppm
        constants/spinner_verbs.cppm
        constants/system.cppm
        constants/system_prompt_sections.cppm
        constants/tool_limits.cppm
        constants/tools_constants.cppm
        constants/turn_completion_verbs.cppm
        constants/xml.cppm
)
target_link_libraries(cc_constants PUBLIC cc_utils cc_types)

# ─── cc_types: Shared Type Definitions ────────────────────────────────────────
add_library(cc_types)
target_sources(cc_types
    PUBLIC FILE_SET CXX_MODULES FILES
        types/auth.cppm
        types/command.cppm
        types/connector_text.cppm
        types/experiment_event.cppm
        types/hooks.cppm
        types/ids.cppm
        types/internal_event.cppm
        types/logs.cppm
        types/permissions.cppm
        types/plugin.cppm
        types/timestamp.cppm
        types/types.cppm
)
target_link_libraries(cc_types PUBLIC cc_utils)

# ─── cc_config: Configuration ─────────────────────────────────────────────────
add_library(cc_config)
target_sources(cc_config
    PUBLIC FILE_SET CXX_MODULES FILES
        config/config.cppm
        config/feature_flags.cppm
        config/model_config.cppm
        config/settings.cppm
)
target_link_libraries(cc_config PUBLIC cc_utils cc_types)

# ─── cc_context: UI Context ───────────────────────────────────────────────────
add_library(cc_context)
target_sources(cc_context
    PUBLIC FILE_SET CXX_MODULES FILES
        context/context.cppm
        context/fps_metrics.cppm
        context/mailbox.cppm
        context/modal.cppm
        context/notifications.cppm
        context/overlay.cppm
        context/stats.cppm
        context/voice.cppm
)
target_link_libraries(cc_context PUBLIC cc_utils cc_state cc_coordinator)

# ─── cc_keybindings: Keybinding System ────────────────────────────────────────
add_library(cc_keybindings)
target_sources(cc_keybindings
    PUBLIC FILE_SET CXX_MODULES FILES
        keybindings/defaults.cppm
        keybindings/keybinding_system.cppm
        keybindings/keybindings.cppm
        keybindings/load_user_bindings.cppm
        keybindings/match.cppm
        keybindings/parser.cppm
        keybindings/reserved_shortcuts.cppm
        keybindings/resolver.cppm
        keybindings/schema.cppm
        keybindings/shortcut_display.cppm
        keybindings/shortcut_format.cppm
        keybindings/template.cppm
        keybindings/validate.cppm
)
target_link_libraries(cc_keybindings PUBLIC cc_utils cc_config)

# ─── cc_memdir: Memory Directory ──────────────────────────────────────────────
add_library(cc_memdir)
target_sources(cc_memdir
    PUBLIC FILE_SET CXX_MODULES FILES
        memdir/find_relevant_memories.cppm
        memdir/memdir.cppm
        memdir/memory.cppm
        memdir/memory_scan.cppm
        memdir/paths.cppm
        memdir/team_memory.cppm
)
target_link_libraries(cc_memdir PUBLIC cc_utils)

# ─── cc_coordinator: Multi-Agent Orchestration ────────────────────────────────
add_library(cc_coordinator)
target_sources(cc_coordinator
    PUBLIC FILE_SET CXX_MODULES FILES
        coordinator/coordinator.cppm
        coordinator/coordinator_mode.cppm
        coordinator/coordinator_types.cppm
        coordinator/swarm.cppm
)
target_link_libraries(cc_coordinator PUBLIC cc_utils cc_state cc_entrypoints)

# ─── cc_query: Query Engine ───────────────────────────────────────────────────
add_library(cc_query)
target_sources(cc_query
    PUBLIC FILE_SET CXX_MODULES FILES
        query/config.cppm
        query/query_engine.cppm
        query/stop_hooks.cppm
        query/token_budget.cppm
)
target_link_libraries(cc_query PUBLIC cc_utils cc_state cc_config CURL::libcurl)

# ─── cc_session: Session Management ──────────────────────────────────────────
add_library(cc_session)
target_sources(cc_session
    PUBLIC FILE_SET CXX_MODULES FILES
        session/history.cppm
        session/session.cppm
        session/storage.cppm
)
target_link_libraries(cc_session PUBLIC cc_utils cc_state cc_config yyjson)

# ─── cc_entrypoints: CLI Entry Points ─────────────────────────────────────────
add_library(cc_entrypoints)
target_sources(cc_entrypoints
    PUBLIC FILE_SET CXX_MODULES FILES
        entrypoints/cli_entrypoints.cppm
        entrypoints/control_schemas.cppm
        entrypoints/control_types.cppm
        entrypoints/core_schemas.cppm
        entrypoints/core_types.cppm
        entrypoints/init.cppm
        entrypoints/mcp_entrypoint.cppm
        entrypoints/runner.cppm
        entrypoints/runtime_types.cppm
        entrypoints/sandbox_types.cppm
        entrypoints/sdk_types.cppm
        entrypoints/settings_types.cppm
)
target_link_libraries(cc_entrypoints PUBLIC cc_utils cc_state cc_config)

# ─── cc_bootstrap: Startup Bootstrap ─────────────────────────────────────────
add_library(cc_bootstrap)
target_sources(cc_bootstrap
    PUBLIC FILE_SET CXX_MODULES FILES
        bootstrap/setup.cppm
        bootstrap/state.cppm
        bootstrap/interactive_helpers.cppm
)
target_link_libraries(cc_bootstrap PUBLIC cc_utils cc_state cc_config cc_services cc_hooks)

# ─── cc_tasks: Task System ────────────────────────────────────────────────────
add_library(cc_tasks)
target_sources(cc_tasks
    PUBLIC FILE_SET CXX_MODULES FILES
        tasks/dream_task.cppm
        tasks/in_process_teammate_task.cppm
        tasks/local_agent_task.cppm
        tasks/local_main_session_task.cppm
        tasks/local_shell_task.cppm
        tasks/pill_label.cppm
        tasks/remote_agent_task.cppm
        tasks/task.cppm
        tasks/task_graph.cppm
        tasks/types.cppm
)
target_link_libraries(cc_tasks PUBLIC cc_utils cc_types cc_state cc_coordinator cc_hooks)

# ─── cc_skills_core: Skill primitives without tool dependencies ──────────────
add_library(cc_skills_core)
target_sources(cc_skills_core
    PUBLIC FILE_SET CXX_MODULES FILES
        skills/skill.cppm
)
target_link_libraries(cc_skills_core
    PUBLIC
        cc_types
)

# ─── cc_tools: Tools System ───────────────────────────────────────────────────
add_library(cc_tools)
target_sources(cc_tools
    PUBLIC FILE_SET CXX_MODULES FILES
        tools/agent_color_manager.cppm
        tools/agent_constants.cppm
        tools/agent_loader.cppm
        tools/agent_memory.cppm
        tools/agent_memory_snapshot.cppm
        tools/agent_prompt.cppm
        tools/agent_tool.cppm
        tools/agent_sub_utils.cppm
        tools/agent_run.cppm
        tools/agent_resume.cppm
        tools/agent_fork.cppm
        tools/agent_type_resolution.cppm
        tools/agent_types.cppm
        tools/agent_utils.cppm
        tools/ask_user_tool.cppm
        tools/bash_permissions.cppm
        tools/bash_security.cppm
        tools/bash_tool.cppm
        tools/bash_result_formatting.cppm
        tools/brief_tool.cppm
        tools/built_in_agents.cppm
        tools/command_semantics.cppm
        tools/computer_use.cppm
        tools/config_tool.cppm
        tools/cron_tool.cppm
        tools/file_edit_prompt.cppm
        tools/file_edit_tool.cppm
        tools/file_edit_types.cppm
        tools/file_read_tool.cppm
        tools/file_write_tool.cppm
        tools/git_operation_tracking.cppm
        tools/glob_tool.cppm
        tools/grep_tool.cppm
        tools/list_mcp_resources_tool.cppm
        tools/lsp_formatters.cppm
        tools/lsp_symbol_context.cppm
        tools/lsp_tool.cppm
        tools/mcp_auth_tool.cppm
        tools/mcp_classify.cppm
        tools/mcp_tool.cppm
        tools/notebook_tool.cppm
        tools/plan_mode_tool.cppm
        tools/powershell_permissions.cppm
        tools/powershell_security.cppm
        tools/powershell_tool.cppm
        tools/read_mcp_resource_tool.cppm
        tools/repl_tool.cppm
        tools/remote_trigger_tool.cppm
        tools/runtime_computer_use.cppm
        tools/runtime_message_delivery.cppm
        tools/runtime_registry.cppm
        tools/runtime_shared_utils.cppm
        tools/runtime_team_shared.cppm
        tools/script_diagnostics.cppm
        tools/script_sandbox.cppm
        tools/script_tool.cppm
        tools/script_types.cppm
        tools/send_message_tool.cppm
        tools/shared_tool.cppm
        tools/skill_tool.cppm
        tools/sleep_tool.cppm
        tools/spawn_multi_agent.cppm
        tools/synthetic_output_tool.cppm
        tools/task_create.cppm
        tools/task_get.cppm
        tools/task_list.cppm
        tools/task_output.cppm
        tools/task_stop.cppm
        tools/task_tool.cppm
        tools/task_update.cppm
        tools/team_create.cppm
        tools/team_delete.cppm
        tools/team_tool.cppm
        tools/testing_tool.cppm
        tools/todo_write_tool.cppm
        tools/tool.cppm
        tools/tool_display_names.cppm
        tools/tool_name_constants.cppm
        tools/tool_prompts.cppm
        tools/tool_registry.cppm
        tools/tool_search_tool.cppm
        tools/tungsten_tool.cppm
        services/tools/orchestration.cppm
        services/tools/streaming_executor.cppm
        tools/web_browser_tool.cppm
        tools/web_fetch_tool.cppm
        tools/web_search_tool.cppm
        tools/workflow_tool.cppm
        tools/worktree_tool.cppm
        tools/agent_runtime.cppm
        tools/agent_display.cppm
        tools/bash_validation.cppm
        tools/destructive_command_warning.cppm
        tools/mode_validation.cppm
        tools/path_validation.cppm
        tools/readonly_validation.cppm
        tools/should_use_sandbox.cppm
        tools/bash_helpers.cppm
        tools/powershell_validation.cppm
        tools/powershell_helpers.cppm
        tools/sed_edit_parser.cppm
        tools/sed_validation.cppm
        tools/script_typecheck.cppm
        tools/script_primitives.cppm
        tools/brief_attachments.cppm
        tools/file_read_helpers.cppm
        tools/lsp_schemas.cppm
        # Phase 3-B/C real tool primitives: bash exec + file I/O + glob/grep
        tools/bash/impl_bash.cppm
        tools/files/impl_files.cppm
)
target_link_libraries(cc_tools
    PUBLIC
        cc_utils
        cc_types
        cc_config
        cc_services
        cc_skills_core
        cc_hooks
        yyjson
        uv_a
)
if(APPLE)
    target_link_libraries(cc_tools PUBLIC "-framework ApplicationServices")
endif()

target_link_libraries(cc_query PUBLIC cc_tools cc_hooks cc_session cc_memdir)

# ─── cc_commands: Command System ──────────────────────────────────────────────
add_library(cc_commands)
target_sources(cc_commands
    PUBLIC FILE_SET CXX_MODULES FILES
        commands/add_dir.cppm
        commands/ant_trace.cppm
        commands/advisor.cppm
        commands/agents.cppm
        commands/autofix_pr.cppm
        commands/backfill_sessions.cppm
        commands/branch.cppm
        commands/break_cache.cppm
        commands/bridge.cppm
        commands/bridge-kick.cppm
        commands/brief.cppm
        commands/bughunter.cppm
        commands/btw.cppm
        commands/clear.cppm
        commands/chrome.cppm
        commands/color.cppm
        commands/command.cppm
        commands/command_registry.cppm
        commands/commit.cppm
        commands/commit_push_pr.cppm
        commands/compact.cppm
        commands/config.cppm
        commands/context.cppm
        commands/copy_cmd.cppm
        commands/cost.cppm
        commands/create_moved_to_plugin_command.cppm
        commands/ctx-viz.cppm
        commands/desktop.cppm
        commands/debug_tool_call.cppm
        commands/diff.cppm
        commands/doctor.cppm
        commands/effort.cppm
        commands/env.cppm
        commands/exit.cppm
        commands/export_cmd.cppm
        commands/extra_usage.cppm
        commands/fast.cppm
        commands/feedback.cppm
        commands/files.cppm
        commands/good_claude.cppm
        commands/heapdump.cppm
        commands/help.cppm
        commands/hooks.cppm
        commands/ide.cppm
        commands/init.cppm
        commands/init_verifiers.cppm
        commands/insights.cppm
        commands/install.cppm
        commands/install_github_app.cppm
        commands/install_github_app_steps.cppm
        commands/install_slack_app.cppm
        commands/install_github_app/flow.cppm
        commands/install_github_app/github_actions.cppm
        commands/install_github_app/steps.cppm
        commands/issue.cppm
        commands/keybindings_cmd.cppm
        commands/login.cppm
        commands/logout.cppm
        commands/mcp/add_command.cppm
        commands/mcp/xaa_idp.cppm
        commands/mcp_cmd.cppm
        commands/memory.cppm
        commands/mobile.cppm
        commands/model.cppm
        commands/mock_limits.cppm
        commands/oauth_refresh.cppm
        commands/onboarding.cppm
        commands/output_style.cppm
        commands/passes.cppm
        commands/permissions_cmd.cppm
        commands/perf_issue.cppm
        commands/plan.cppm
        commands/plugin/browse_marketplace.cppm
        commands/plugin/manage_plugins.cppm
        commands/plugin/plugin_helpers.cppm
        commands/plugin/plugin_error_formatting.cppm
        commands/plugin/plugin_details_helpers.cppm
        commands/plugin/pagination_util.cppm
        commands/plugin/plugin_install.cppm
        commands/plugin/plugin_manage.cppm
        commands/plugin/plugin_parse_args.cppm
        commands/plugin/plugin_trust.cppm
        commands/plugin/plugin_trust_text.cppm
        commands/plugin/plugin_ui_data.cppm
        commands/plugin_cmd.cppm
        commands/pr_comments.cppm
        commands/privacy_settings.cppm
        commands/rate_limit_options.cppm
        commands/release_notes.cppm
        commands/reload_plugins.cppm
        commands/remote_commands.cppm
        commands/remote_env.cppm
        commands/remote_setup.cppm
        commands/remote_setup_api.cppm
        commands/rename.cppm
        commands/reset_limits.cppm
        commands/resume.cppm
        commands/review.cppm
        commands/review/review_remote.cppm
        commands/review/ultrareview.cppm
        commands/rewind.cppm
        commands/runtime_surface_commands.cppm
        commands/sandbox_toggle.cppm
        commands/security_review.cppm
        commands/session.cppm
        commands/share.cppm
        commands/skills_cmd.cppm
        commands/stats.cppm
        commands/status.cppm
        commands/statusline.cppm
        commands/stickers.cppm
        commands/summary.cppm
        commands/tag.cppm
        commands/tasks_cmd.cppm
        commands/teleport.cppm
        commands/terminal_setup.cppm
        commands/theme.cppm
        commands/thinkback.cppm
        commands/thinkback_play.cppm
        commands/ultraplan.cppm
        commands/upgrade.cppm
        commands/usage.cppm
        commands/version.cppm
        commands/vim.cppm
        commands/voice.cppm
)
target_sources(cc_commands PRIVATE
    commands/command_registry_init.cpp
)
target_link_libraries(cc_commands
    PUBLIC
        cc_utils
        cc_types
        cc_config
        cc_tools
)

# ─── cc_services: Services Layer ──────────────────────────────────────────────
add_library(cc_services)
target_sources(cc_services
    PUBLIC FILE_SET CXX_MODULES FILES
        services/agent_summary/agent_summary.cppm
        services/analytics/analytics.cppm
        services/analytics/config.cppm
        services/analytics/datadog.cppm
        services/analytics/first_party_event_logger.cppm
        services/analytics/growthbook.cppm
        services/analytics/index.cppm
        services/analytics/metadata.cppm
        services/analytics/sink.cppm
        services/analytics/sink_killswitch.cppm
        services/api/admin_requests.cppm
        services/api/bootstrap.cppm
        services/api/client.cppm
        services/api/error_utils.cppm
        services/api/errors.cppm
        services/api/grove.cppm
        services/api/logging.cppm
        services/api/metrics_opt_out.cppm
        services/api/models.cppm
        services/api/referral.cppm
        services/api/session_ingress.cppm
        services/api/streaming.cppm
        services/api/usage.cppm
        services/api/with_retry.cppm
        services/api/sse_client.cppm
        services/api/with_retry_simple.cppm
        services/auto_dream/auto_dream.cppm
        services/auto_dream/config.cppm
        services/auto_dream/consolidation_lock.cppm
        services/auto_dream/consolidation_prompt.cppm
        services/away_summary.cppm
        services/auth/sigv4.cppm
        services/auth/gcp_adc.cppm
        services/auth/azure_credential.cppm
        services/auth/provider_selector.cppm
        services/compact/auto_compact.cppm
        services/compact/compact.cppm
        services/compact/grouping.cppm
        services/compact/micro_compact.cppm
        services/compact/post_compact_cleanup.cppm
        services/compact/prompt.cppm
        services/compact/session_memory_compact.cppm
        services/compact/types.cppm
        services/compact/warning_hook.cppm
        services/diagnostic/diagnostic.cppm
        services/image/image.cppm
        services/lsp/LSPServerInstance.cppm
        services/lsp/LSPServerManager.cppm
        services/lsp/client.cppm
        services/lsp/diagnostic_registry.cppm
        services/lsp/manager.cppm
        services/lsp/passive_feedback.cppm
        services/lsp/types.cppm
        services/magic_docs/magic_docs.cppm
        services/mcp/at_mention_handler.cppm
        services/mcp/auth.cppm
        services/mcp/channel_allowlist.cppm
        services/mcp/channel_notification.cppm
        services/mcp/channel_permissions.cppm
        services/mcp/client.cppm
        services/mcp/config.cppm
        services/mcp/connection_manager.cppm
        services/mcp/elicitation_handler.cppm
        services/mcp/env_expansion.cppm
        services/mcp/mcp_server.cppm
        services/mcp/mcp_server_approval.cppm
        services/mcp/mcp_utils.cppm
        services/mcp/normalization.cppm
        services/mcp/official_registry.cppm
        services/mcp/sdk_control_transport.cppm
        services/mcp/types.cppm
        services/mcp/xaa.cppm
        services/memory/extract_memories.cppm
        services/memory/prompts.cppm
        services/memory/sessionMemory.cppm
        services/memory/session_memory.cppm
        services/memory/session_memory_utils.cppm
        services/notifier/notifier.cppm
        services/oauth/auth_code_listener.cppm
        services/oauth/client.cppm
        services/oauth/crypto.cppm
        services/oauth/oauth_profile.cppm
        services/oauth/types.cppm
        services/plugins/cli_commands.cppm
        services/plugins/installation_manager.cppm
        services/plugins/operations.cppm
        services/policy/policy.cppm
        services/policy/types.cppm
        services/prevent_sleep/prevent_sleep.cppm
        services/prompt_suggestion/prompt_suggestion.cppm
        services/proxy/proxy.cppm
        services/rate_limit/messages.cppm
        services/rate_limit/mock.cppm
        services/rate_limit/rate_limit.cppm
        services/remote/session_manager.cppm
        services/remote_settings/security_check.cppm
        services/remote_settings/sync_cache.cppm
        services/remote_settings/sync_cache_state.cppm
        services/remote_settings/types.cppm
        services/settings_sync/settings_sync.cppm
        services/team_memory/team_memory.cppm
        services/team_memory/team_memory_sync.cppm
        services/team_memory/team_memory_helpers.cppm
        services/api/claude_api.cppm
        services/mcp/mcp_transport_service.cppm
        services/compact/compact_helpers.cppm
        services/telemetry/telemetry.cppm
        services/tips/tip_history.cppm
        services/tips/tip_registry.cppm
        services/tips/tip_scheduler.cppm
        services/tips/tips.cppm
        services/token/estimation.cppm
        services/tool_summary/tool_summary.cppm
        services/vcr/vcr.cppm
        services/voice/voice.cppm
        services/voice/voice_keyterms.cppm
        services/voice/voice_stream_stt.cppm
        services/assistant/session_history.cppm
        services/output_styles/output_styles.cppm
        services/upstream_proxy/upstream_proxy.cppm
        services/api/claude_ai_limits.cppm
        services/api/dump_prompts.cppm
        services/api/empty_usage.cppm
        services/api/first_token_date.cppm
        services/api/overage_credit.cppm
        services/api/prompt_cache_break.cppm
        services/api/ultrareview_quota.cppm
        services/mcp/mcp_string_utils.cppm
        services/mcp/headers_helper.cppm
        utils/ide_integration.cppm
        services/mcp/vscode_sdk_mcp.cppm
        services/mcp/in_process_transport.cppm
        services/mcp/oauth_port.cppm
        services/mcp/xaa_idp_login.cppm
        services/mcp/transport_stdio.cppm
        services/diagnostic/dump_diagnostic.cppm
        services/rate_limit/claude_ai_limits_hook.cppm
        services/compact/api_microcompact.cppm
        services/compact/compact_warning_state.cppm
        services/compact/time_based_config.cppm
        services/team_memory/secret_guard.cppm
        services/team_memory/secret_scanner.cppm
        services/team_memory/watcher.cppm
)
target_sources(cc_services
    PRIVATE
        services/mcp/config_impl.cpp
        services/mcp/headers_helper_impl.cpp
)
target_link_libraries(cc_services
    PUBLIC
        cc_utils
        cc_types
        cc_constants
        CURL::libcurl
        yyjson
        uv_a
        httplib::httplib
        OpenSSL::SSL
        OpenSSL::Crypto
)
# Native macOS Keychain backend (services/oauth) uses the Security framework.
if(APPLE)
    target_link_libraries(cc_services PUBLIC "-framework Security" "-framework CoreFoundation")
endif()
target_link_libraries(cc_commands
    PUBLIC
        cc_services
        cc_constants
        cc_keybindings
        cc_skills_core
        cc_skills
        cc_tasks
)

# ─── cc_hooks: Hooks System ───────────────────────────────────────────────────
add_library(cc_hooks)
target_sources(cc_hooks
    PUBLIC FILE_SET CXX_MODULES FILES
        hooks/after_first_render.cppm
        hooks/api_key_verification.cppm
        hooks/arrow_key_history.cppm
        hooks/assistant_history.cppm
        hooks/auto_save.cppm
        hooks/away_summary.cppm
        hooks/background_task_navigation.cppm
        hooks/blink.cppm
        hooks/cancel_request.cppm
        hooks/claude_code_hint_recommendation.cppm
        hooks/command_keybindings.cppm
        hooks/command_queue.cppm
        hooks/context.cppm
        hooks/copy_on_select.cppm
        hooks/cost_hook.cppm
        hooks/deferred_hook_messages.cppm
        hooks/diff_data.cppm
        hooks/diff_in_ide.cppm
        hooks/direct_connect.cppm
        hooks/double_press.cppm
        hooks/dynamic_config.cppm
        hooks/elapsed_time.cppm
        hooks/exit_handler.cppm
        hooks/file_history_snapshot_init.cppm
        hooks/file_suggestions.cppm
        hooks/file_watcher.cppm
        hooks/global_keybindings.cppm
        hooks/history_search.cppm
        hooks/ide_at_mentioned.cppm
        hooks/ide_connection_status.cppm
        hooks/ide_integration.cppm
        hooks/ide_logging.cppm
        hooks/ide_selection.cppm
        hooks/inbox_poller.cppm
        hooks/input_buffer.cppm
        hooks/input_hooks.cppm
        hooks/issue_flag_banner.cppm
        hooks/log_messages.cppm
        hooks/mailbox_bridge.cppm
        hooks/main_loop_model.cppm
        hooks/manage_plugins.cppm
        hooks/marketplace_notification.cppm
        hooks/memory_usage.cppm
        hooks/merged_providers.cppm
        hooks/min_display_time.cppm
        hooks/notifications.cppm
        hooks/notify_after_timeout.cppm
        hooks/notifs/auto_mode_unavailable.cppm
        hooks/notifs/deprecation_warning.cppm
        hooks/notifs/fast_mode.cppm
        hooks/notifs/ide_status_indicator.cppm
        hooks/notifs/install_messages.cppm
        hooks/notifs/lsp_initialization.cppm
        hooks/notifs/rate_limit_warning.cppm
        hooks/notifs/remaining_notifs.cppm
        hooks/notifs/startup.cppm
        hooks/paste_handler.cppm
        hooks/permissions.cppm
        hooks/plugin_recommendation.cppm
        hooks/pr_status.cppm
        hooks/prompt_suggestion.cppm
        hooks/queue_processor.cppm
        hooks/remaining_hooks.cppm
        hooks/remote_session.cppm
        hooks/render_placeholder.cppm
        hooks/repl_bridge.cppm
        hooks/scheduled_tasks.cppm
        hooks/search_input.cppm
        hooks/session_backgrounding.cppm
        hooks/settings_hooks.cppm
        hooks/skill_improvement_survey.cppm
        hooks/skills_change.cppm
        hooks/ssh_session.cppm
        hooks/swarm_hooks.cppm
        hooks/task_hooks.cppm
        hooks/tasks.cppm
        hooks/teammate_view_auto_exit.cppm
        hooks/teleport_resume.cppm
        hooks/terminal_size.cppm
        hooks/text_input.cppm
        hooks/timeout_hook.cppm
        hooks/tool_permission/coordinator_handler.cppm
        hooks/tool_permission/interactive_handler.cppm
        hooks/tool_permission/permission_context.cppm
        hooks/tool_permission/swarm_worker_handler.cppm
        hooks/tool_permissions.cppm
        hooks/lifecycle_hooks.cppm
        hooks/turn_diffs.cppm
        hooks/typeahead.cppm
        hooks/unified_suggestions.cppm
        hooks/update_notification.cppm
        hooks/vim_input.cppm
        hooks/virtual_scroll.cppm
        hooks/voice_hooks.cppm
        hooks/tool_permission/permission_logging.cppm
        hooks/exit_ctrl_cd_keybindings.cppm
        hooks/settings_change.cppm
        hooks/swarm_permission_poller.cppm
        hooks/shell_hooks.cppm
        hooks/permission_resolver.cppm
        hooks/tool_permission_gate.cppm
)
target_link_libraries(cc_hooks
    PUBLIC
        cc_utils
        cc_types
        cc_config
        cc_state
        cc_services
        uv_a
)

# ─── cc_cli: CLI Transports ───────────────────────────────────────────────────
add_library(cc_cli)
target_sources(cc_cli
    PUBLIC FILE_SET CXX_MODULES FILES
        cli/bg.cppm
        cli/ccr_client.cppm
        cli/exit.cppm
        cli/handlers/agents.cppm
        cli/handlers/auth.cppm
        cli/handlers/auto_mode.cppm
        cli/handlers/mcp_handler.cppm
        cli/handlers/plugins_handler.cppm
        cli/handlers/template_jobs.cppm
        cli/hybrid_transport.cppm
        cli/ndjson_stringify.cppm
        cli/print.cppm
        cli/remote_io.cppm
        cli/sse_transport.cppm
        cli/structured_io.cppm
        cli/transports.cppm
        cli/update.cppm
        cli/websocket_transport.cppm
        cli/worker_state_uploader.cppm
)
target_link_libraries(cc_cli
    PUBLIC
        cc_utils
        yyjson
        uv_a
        httplib::httplib
)

# ─── cc_bridge: IDE Bridge ────────────────────────────────────────────────────
add_library(cc_bridge)
target_sources(cc_bridge
    PUBLIC FILE_SET CXX_MODULES FILES
        bridge/api.cppm
        bridge/bridge.cppm
        bridge/bridge_debug.cppm
        bridge/bridge_enabled.cppm
        bridge/bridge_main.cppm
        bridge/bridge_messaging.cppm
        bridge/bridge_types.cppm
        bridge/capacity_wake.cppm
        bridge/config.cppm
        bridge/core.cppm
        bridge/debug_utils.cppm
        bridge/envless_config.cppm
        bridge/flush_gate.cppm
        bridge/inbound_attachments.cppm
        bridge/inbound_messages.cppm
        bridge/init.cppm
        bridge/jwt_utils.cppm
        bridge/messages.cppm
        bridge/pointer.cppm
        bridge/poll_config.cppm
        bridge/poll_config_defaults.cppm
        bridge/security.cppm
        bridge/session_api.cppm
        bridge/session_id_compat.cppm
        bridge/session_runner.cppm
        bridge/status_util.cppm
        bridge/transport.cppm
        bridge/trusted_device.cppm
        bridge/ui.cppm
        bridge/work_secret.cppm
)
target_link_libraries(cc_bridge
    PUBLIC
        cc_utils
        cc_types
        cc_cli
        OpenSSL::Crypto
        yyjson
        uv_a
)

# ─── cc_skills: Skill System ─────────────────────────────────────────────────
add_library(cc_skills)
target_sources(cc_skills
    PUBLIC FILE_SET CXX_MODULES FILES
        skills/bundled.cppm
        skills/bundled/batch.cppm
        skills/bundled/claude_api.cppm
        skills/bundled/claude_in_chrome.cppm
        skills/bundled/debug.cppm
        skills/bundled/loop.cppm
        skills/bundled/remember.cppm
        skills/bundled/simplify.cppm
        skills/bundled/skill_keybindings.cppm
        skills/bundled/skillify.cppm
        skills/bundled/stuck.cppm
        skills/bundled/update_config.cppm
        skills/bundled/verify.cppm
        skills/claude_api.cppm
        skills/claude_api_content.cppm
        skills/keybindings.cppm
        skills/load_skills_dir.cppm
        skills/lorem_ipsum.cppm
        skills/mcp_skill_builders.cppm
        skills/remember.cppm
        skills/schedule_remote_agents.cppm
        skills/simplify.cppm
        skills/verify_content.cppm
)
target_link_libraries(cc_skills
    PUBLIC
        cc_utils
        cc_skills_core
        cc_tools
        yyjson
)

# ─── cc_ui: Terminal UI ───────────────────────────────────────────────────────
add_library(cc_ui)
target_sources(cc_ui
    PUBLIC FILE_SET CXX_MODULES FILES
        ui/app.cppm
        ui/autocomplete_sources.cppm
        ui/app_dialog_registration.cppm
        ui/components.cppm
        ui/agents/agent_cards.cppm
        ui/agents/agent_details_dialog.cppm
        ui/agents/agent_editor.cppm
        ui/agents/agent_list.cppm
        ui/agents/agent_shared_widgets.cppm
        ui/agents/agent_wizard.cppm
        ui/agents/agent_utils.cppm
        ui/components/all_components.cppm
        ui/components/agent_view.cppm
        ui/components/code_highlight.cppm
        ui/components/cost_display.cppm
        ui/components/custom_select.cppm
        ui/components/dev_bar.cppm
        ui/components/diff_view.cppm
        ui/components/fast_icon.cppm
        ui/components/feature_dialogs.cppm
        ui/components/figures.cppm
        ui/components/file_tree.cppm
        ui/components/grove.cppm
        ui/components/ink_components.cppm
        ui/components/lsp_recommendation_menu.cppm
        ui/components/notification.cppm
        ui/components/partial_completions.cppm   # UI27 — TreeSelect + ShikiFallback (PARTIAL -> DONE)
        ui/components/passes.cppm
        ui/components/plugin_hint_menu.cppm
        ui/components/pr_badge.cppm
        ui/components/spinner.cppm
        ui/components/spinner_widget.cppm
        ui/components/stats.cppm
        ui/components/structured_diff.cppm
        ui/components/file_edit_tool_diff.cppm
        ui/components/tag_tabs.cppm
        ui/components/task_view.cppm
        ui/components/text_input.cppm
        ui/components/text_input_widget.cppm
        ui/components/user_bash_input_message.cppm
        ui/components/auto_updater.cppm
        ui/components/auth_flows.cppm
        ui/components/bash_mode_progress.cppm
        ui/components/context_visualization.cppm
        ui/components/session_preview.cppm
        ui/components/shell_progress_message.cppm
        ui/components/shell_time_display.cppm
        ui/components/spinner_animations.cppm
        ui/components/spinner_shimmer.cppm
        ui/components/spinner_teammate_tree.cppm
        ui/common/ui_types.cppm
        ui/common/ui_formatting.cppm
        ui/common/small_widgets.cppm
        ui/common/declared_cursor.cppm
        ui/design/dialog.cppm
        ui/design/divider.cppm
        ui/design/list_item.cppm
        ui/design/progress_bar.cppm
        ui/design/status_icon.cppm
        ui/design/tabs.cppm
        ui/design/themed_box.cppm
        ui/design/themed_text.cppm
        ui/dialogs/dialog_system.cppm
        ui/dialogs/dialog_frame.cppm
        ui/dialogs/dialog_launchers.cppm
        ui/dialogs/dialog_default_renderers.cppm
        ui/dialogs/config_dialog.cppm
        ui/dialogs/desktop_upsell.cppm
        ui/dialogs/diff_dialog.cppm
        ui/dialogs/elicitation_dialog.cppm
        ui/dialogs/feedback_survey.cppm
        ui/dialogs/help_view.cppm
        ui/dialogs/settings_view.cppm
        ui/dialogs/about_dialog.cppm
        ui/dialogs/confirmation_dialog.cppm
        ui/dialogs/ide_dialogs.cppm
        ui/dialogs/install_github_app_wizard.cppm
        ui/dialogs/install_slack_app_wizard.cppm
        ui/dialogs/managed_settings_security.cppm
        ui/dialogs/mcp_dialog.cppm
        ui/dialogs/mcp_dialogs.cppm
        ui/dialogs/model_picker.cppm
        ui/dialogs/onboarding.cppm
        ui/dialogs/output_style_picker.cppm
        ui/dialogs/permission_dialog.cppm
        ui/dialogs/permission_prompts.cppm
        ui/dialogs/plugin_dialog.cppm
        ui/dialogs/prompt_dialog.cppm
        ui/dialogs/quick_open.cppm
        ui/dialogs/sandbox_dialog.cppm
        ui/dialogs/settings_dialog.cppm
        ui/dialogs/settings_status_page.cppm
        ui/dialogs/usage_dialog.cppm
        ui/dialogs/teleport_dialogs.cppm
        ui/dialogs/trust_dialog.cppm
        ui/dialogs/trust_utils.cppm
        ui/dialogs/wizard_dialog.cppm
        ui/dialogs/auto_mode_dialog.cppm
        ui/dialogs/bridge_dialog.cppm
        ui/dialogs/cost_threshold_dialog.cppm
        ui/dialogs/export_dialog.cppm
        ui/dialogs/global_search_dialog.cppm
        ui/dialogs/history_search_dialog.cppm
        ui/dialogs/idle_return_dialog.cppm
        ui/dialogs/ide_connect_dialog.cppm
        ui/dialogs/remote_env_dialog.cppm
        ui/dialogs/worktree_exit_dialog.cppm
        ui/dialogs/all_renderers.cppm            # new: DialogRendererRegistry + all renderers
        ui/dialogs/bottom_renderers.cppm         # new: Bottom-band dialog renderers
        ui/dialogs/modal_renderers.cppm          # new: Overlay/Modal dialog renderers
        ui/dialogs/sandbox_permission.cppm       # new: SandboxPermission renderer
        ui/dialogs/triggers.cppm                 # new: Dialog trigger / invocation routing
        # M8 note: ManagedSettingsSecurity / FeedbackSurvey / GlobalSearch /
        # HistorySearch / PluginDialog / DiffDialog — FTXUI chrome not yet ported.
        # Do NOT add placeholder sources into FILE_SET CXX_MODULES: CMake 4.x
        # cascades "not scheduled for compilation" diagnostics across the
        # entire target if any listed source is missing.  Only add module
        # sources that exist AND contain real implementations.  Registration
        # lives in register_default_renderers() in default_renderers.cppm.
        ui/events/event_system.cppm
        ui/layout.cppm
        ui/layout/fullscreen_layout.cppm
        ui/layout/logo.cppm
        ui/layout/logo_v2.cppm          # P0-4: LogoV2 3-mode dispatch + WelcomeV2 + notice stack
        ui/layout/measure.cppm
        ui/layout/wrap_text.cppm
        ui/layout/yoga.cppm
        ui/logo/logo_animated.cppm
        ui/logo/logo_feed.cppm
        ui/logo/logo_notices.cppm
        ui/logo/logo_welcome.cppm
        ui/markdown.cppm
        ui/messages.cppm
        ui/messages/assistant_message.cppm
        ui/messages/error_message.cppm
        ui/messages/message_components.cppm
        ui/messages/message_response.cppm
        ui/messages/message_row.cppm
        ui/messages/message_timestamp.cppm
        ui/messages/message_advisor.cppm
        ui/messages/message_bash_io.cppm
        ui/messages/message_channel.cppm
        ui/messages/message_grouped_tools.cppm
        ui/messages/message_hook_progress.cppm
        ui/messages/message_plan_approval.cppm
        ui/messages/message_rate_limit.cppm
        ui/messages/message_task_assignment.cppm
        ui/messages/message_tool_result.cppm
        ui/messages/structured_diff.cppm
        ui/messages/thinking_message.cppm
        ui/messages/tool_messages.cppm
        ui/messages/tool_use_loader.cppm
        ui/messages/tool_use_message.cppm
        ui/messages/user_message.cppm
        ui/messages/message_redacted_thinking.cppm
        ui/messages/message_image.cppm
        ui/messages/message_compact_boundary.cppm
        ui/messages/message_shutdown.cppm
        ui/messages/message_user_command.cppm
        ui/messages/user_text_message.cppm
        ui/messages/assistant_text_message.cppm
        ui/messages/system_text_message.cppm
        ui/messages/attachment_message.cppm
        ui/messages/api_error_message.cppm
        ui/messages/collapsed_content_message.cppm
        ui/messages/local_command_output_message.cppm
        ui/messages/messages_list.cppm            # UI21 — Messages.tsx + Message.tsx (834+626 → 1308 loc)
        ui/messages/messages_interactions.cppm    # UI25 — MessageSelector + messageActions
        ui/messages/message_pipeline.cppm         # P0-2 — 7-stage message pipeline (dedup/tag-filter/augment/hide/index)
        ui/messages/scroll_keybindings.cppm       # UI22 — ScrollKeybindingHandler (1011 → 752 loc)
        ui/messages/virtual_message_list.cppm     # UI22 — VirtualScroll (1081 → 857 loc)
        ui/panels.cppm
        ui/permissions/permission_diff.cppm
        ui/permissions/permission_request.cppm
        ui/permissions/permission_rules.cppm
        ui/permissions/permission_views.cppm
        ui/permissions/permission_ask_user.cppm
        ui/permissions/permission_bash.cppm
        ui/permissions/permission_computer_use.cppm
        ui/permissions/permission_file_edit.cppm
        ui/permissions/permission_file_write.cppm
        ui/permissions/permission_plan_mode.cppm
        ui/permissions/permission_rules_ui.cppm
        ui/permissions/permission_shell_helpers.cppm
        ui/permissions/permission_worker_badge.cppm
        ui/permissions/permissions_components.cppm
        ui/permissions/permission_scope_editor.cppm
        ui/permissions/permission_rule_list.cppm        # UI24 — PermissionRuleList
        ui/permissions/permission_advanced_prompts.cppm # UI24b — AskUserQuestion+Skill+Fallback advanced prompts
        ui/permissions/permission_single_prompt.cppm
        ui/permissions/permission_batch_panel.cppm
        ui/permissions/sandbox_config_dialog.cppm
        ui/prompt/at_attachments.cppm
        ui/prompt/file_index.cppm
        ui/prompt/fuzzy_rank_nucleo.cppm
        ui/prompt/footer.cppm
        ui/prompt/mode_indicator.cppm
        ui/prompt/notifications.cppm
        ui/prompt/prompt_input_footer.cppm  # M5 — faithful TS PromptInputFooter port
        ui/prompt/prompt_input_full.cppm
        ui/prompt/prompt_help_menu.cppm
        ui/prompt/prompt_history_search.cppm
        ui/prompt/prompt_paste_handler.cppm
        ui/prompt/prompt_queued_commands.cppm
        ui/prompt/prompt_stash_notice.cppm
        ui/prompt/shimmer.cppm
        ui/prompt/vim_input.cppm
        ui/prompt/voice_indicator.cppm
        ui/prompt_input.cppm
        ui/plugins/plugin_install_flow.cppm
        ui/plugins/plugin_manage_panel.cppm
        ui/plugins/plugin_marketplace_browse.cppm
        ui/plugins/plugin_settings_dialog.cppm
        ui/mcp/mcp_capabilities.cppm
        ui/mcp/mcp_add_server_wizard.cppm
        ui/mcp/mcp_security_dialog.cppm
        ui/mcp/mcp_server_details.cppm
        ui/mcp/mcp_server_list.cppm
        ui/mcp/mcp_elicitation.cppm
        ui/mcp/mcp_reconnect.cppm
        ui/mcp/mcp_settings_panel.cppm
        ui/mcp/mcp_tool_browser.cppm
        ui/renderer/ink_utils.cppm
        ui/renderer/renderer.cppm
        ui/renderer/text_measure.cppm
        ui/tasks/task_components.cppm
        ui/tasks/task_details_dialog.cppm
        ui/tasks/task_list_view.cppm
        ui/tasks/task_wizard.cppm
        ui/tasks/task_list_ui.cppm
        ui/tasks/task_background_status.cppm
        ui/tasks/task_detail_dialog.cppm
        ui/tasks/task_remote_session.cppm
        ui/tasks/task_shell_progress.cppm
        ui/tools/tool_ui_registry.cppm
        ui/tools/tool_ui_init.cppm
        ui/tools/tool_ui_generic.cppm
        ui/tools/tool_ui_bash.cppm
        ui/tools/tool_ui_file_edit.cppm
        ui/tools/tool_ui_file_write.cppm
        ui/tools/tool_ui_file_read.cppm
        ui/tools/tool_ui_grep.cppm
        ui/tools/tool_ui_glob.cppm
        ui/tools/tool_ui_web_fetch.cppm
        ui/tools/tool_ui_web_search.cppm
        ui/tools/tool_ui_skill.cppm
        ui/tools/tool_ui_agent.cppm
        ui/tools/tool_ui_task.cppm
        ui/tools/tool_ui_mcp.cppm
        ui/tools/tool_ui_lsp.cppm
        ui/tools/tool_ui_longtail.cppm
        ui/teams/teams_overview.cppm
        ui/teams/team_details_dialog.cppm
        ui/teams/swarm_collaboration_view.cppm
        ui/teams/team_status.cppm
        ui/termio/terminal_io.cppm
        ui/hooks/hooks_ui.cppm
        ui/screens/doctor_screen.cppm
        ui/screens/repl_screen.cppm
        ui/screens/resume_screen.cppm
        ui/screens/log_selector.cppm              # UI23 — LogSelector (1574 → 1730 loc)
        ui/terminal.cppm
        ui/design_system/design_tokens.cppm       # UI20 (merged from cc_ui_design to resolve module-import cycle)
        ui/design_system/figures.cppm             # Shared glyph + prompt-mode constants (TS utils/figures.ts + inputModes.ts)
        ui/design_system/theme_provider.cppm
        ui/design_system/logo.cppm
        ui/design_system/component_primitives.cppm
        ui/design_system/design_extras.cppm       # UI27 — fuzzy picker + theme picker + design extras
)
# Module implementation units for cc.ui.app_dialog_registration — one per
# dialog-renderer aggregator, so no single TU imports more than one aggregator's
# closure (importing all four at once crashes Clang codegen). This keeps the
# ~44 dialog implementations out of app.cppm's BMI (source-location budget).
# See app_dialog_registration.cppm / *_impl.cpp for the rationale.
target_sources(cc_ui PRIVATE
    ui/autocomplete_sources_impl.cpp
    ui/app_prompt_suggestion_wiring.cpp
    ui/prompt/at_attachments_impl.cpp
    ui/app_dialog_registration_default.cpp
    ui/app_dialog_registration_modal.cpp
    ui/app_dialog_registration_bottom.cpp
    ui/app_dialog_registration_all.cpp
)
target_link_libraries(cc_ui
    PUBLIC
        cc_utils
        cc_types
        cc_query
        cc_commands
        cc_vim
        cc_hooks
        cc_plugins
        cc_session
        cc_history
        # U1: cc.ui.prompt.suggestion_provider imports cc.services.prompt_suggestion
        # directly. cc_ui already consumed cc.services.* transitively via cc_hooks;
        # making the dependency explicit is correct (no cycle: cc_services never
        # imports cc.ui.*).
        cc_services
        cc_ui_messages       # Phase 4B-UI21/22/25: list + virtual scroll + interactions
        cc_ui_screens        # Phase 4B-UI23: log selector
        cc_ui_permissions    # Phase 4B-UI24: rule list (+ advanced prompts)
        cc_ui_components     # Phase 4B-UI26/27: feature dialogs + PARTIAL completions
        ftxui::screen
        ftxui::dom
        ftxui::component
)

# ─── cc_ui_messages: Phase 4B Messages (list / virtual scroll / interactions)
# UI21 (Messages), UI22 (VirtualScroll + Keybindings), UI25 (Interactions)
#
# NOTE: Sources are registered directly in the parent `cc_ui` FILE_SET so that
# C++23 module-import lookup (BMI) can resolve intra-`cc.ui.*` imports without
# cross-OBJECT cycles.  This INTERFACE target is provided for downstream
# consumers that want to link "just the messages layer" explicitly.
add_library(cc_ui_messages INTERFACE)
target_link_libraries(cc_ui_messages INTERFACE cc_ui)

# ─── cc_ui_screens: Phase 4B Additional Screens (beyond doctor / repl / resume)
# UI23 (LogSelector)
add_library(cc_ui_screens INTERFACE)
target_link_libraries(cc_ui_screens INTERFACE cc_ui)

# ─── cc_ui_permissions: Phase 4B Permissions (rule list / advanced prompts)
# UI24 (PermissionRuleList + PermissionAdvancedPrompts)
add_library(cc_ui_permissions INTERFACE)
target_link_libraries(cc_ui_permissions INTERFACE cc_ui)

# ─── cc_ui_components: Phase 4B Feature-Gated Components
# UI26 (MemoryFileSelector / Feedback / Grove), UI27 PARTIAL (TreeSelect / ShikiFallback)
add_library(cc_ui_components INTERFACE)
target_link_libraries(cc_ui_components INTERFACE cc_ui)

# ─── cc_ui_design: Design System (tokens / theme / logo / primitives) ────────
# Phase 4-UI20: mirrors the TS-side design-system package
#   (src/components/design-system/* + src/utils/theme.ts + upstream logo components).
# Phase 4B REFACTOR: Sources moved into parent `cc_ui`'s FILE_SET so that C++23
#   named-module BMI lookup can resolve intra-module imports both ways
#   (cc.ui.design.* ↔ cc.ui.messages.* / cc.ui.components.*).
#   This INTERFACE target remains for downstream consumers (cc_all / cc_repl)
#   that link `cc_ui_design` as the canonical design-system surface.
add_library(cc_ui_design INTERFACE)
target_link_libraries(cc_ui_design INTERFACE cc_ui)

# ─── cc_all: Convenience aggregator (all production targets) ─────────────────
# INTERFACE library that transitively pulls in every subsystem plus the
# newly-added cc_ui_design.  Downstream targets can link cc_all instead of
# enumerating subsystems.
add_library(cc_all INTERFACE)
target_link_libraries(cc_all
    INTERFACE
        cc_core          # existing subsystem aggregator
        cc_ui_design     # design system (added Phase 4-UI20)
)

# ─── cc_vim: Vim Mode ─────────────────────────────────────────────────────────
add_library(cc_vim)
target_sources(cc_vim
    PUBLIC FILE_SET CXX_MODULES FILES
        vim/vim_commands.cppm
        vim/vim_mode.cppm
        vim/vim_motions.cppm
        vim/vim_operators.cppm
        vim/vim_text_objects.cppm
)
target_link_libraries(cc_vim PUBLIC cc_utils)

# ─── cc_daemon: Daemon System ─────────────────────────────────────────────────
add_library(cc_daemon)
target_sources(cc_daemon
    PUBLIC FILE_SET CXX_MODULES FILES
        daemon/daemon_client.cppm
        daemon/daemon_server.cppm
        daemon/worker_registry.cppm
)
target_link_libraries(cc_daemon PUBLIC cc_utils cc_bridge)

# ─── cc_remote: Remote Connections ────────────────────────────────────────────
add_library(cc_remote)
target_sources(cc_remote
    PUBLIC FILE_SET CXX_MODULES FILES
        remote/remote_auth.cppm
        remote/remote_connection.cppm
        remote/remote_permission_bridge.cppm
        remote/remote_session.cppm
        remote/sdk_message_adapter.cppm
)
target_link_libraries(cc_remote PUBLIC cc_utils cc_bridge OpenSSL::SSL OpenSSL::Crypto)

# ─── cc_buddy: Buddy Companion ────────────────────────────────────────────────
add_library(cc_buddy)
target_sources(cc_buddy
    PUBLIC FILE_SET CXX_MODULES FILES
        buddy/buddy_companion.cppm
        buddy/buddy_hooks.cppm
        buddy/buddy_prompt.cppm
        buddy/buddy_sprites.cppm
        buddy/buddy_types.cppm
)
target_link_libraries(cc_buddy PUBLIC cc_utils)

# ─── cc_migrations: Schema Migrations ─────────────────────────────────────────
add_library(cc_migrations)
target_sources(cc_migrations
    PRIVATE
        migrations/concrete_migrations.cpp
    PUBLIC FILE_SET CXX_MODULES FILES
        migrations/concrete_migrations.cppm
        migrations/config_orchestrator.cppm
        migrations/migration_registry.cppm
        migrations/migration_runner.cppm
        migrations/schema_versions.cppm
)
target_link_libraries(cc_migrations PUBLIC cc_utils)

# ─── cc_server: Server ────────────────────────────────────────────────────────
add_library(cc_server)
target_sources(cc_server
    PUBLIC FILE_SET CXX_MODULES FILES
        server/server_main.cppm
        server/server_routes.cppm
        server/types.cppm
)
target_link_libraries(cc_server PUBLIC cc_utils cc_session cc_query cc_tools OpenSSL::Crypto)

# ─── cc_screens: Screen System ────────────────────────────────────────────────
add_library(cc_screens)
target_sources(cc_screens
    PUBLIC FILE_SET CXX_MODULES FILES
        screens/doctor_screen.cppm
        screens/resume_screen.cppm
        screens/screens.cppm
)
target_link_libraries(cc_screens PUBLIC cc_ui)

# ─── cc_schemas: Validation Schemas ───────────────────────────────────────────
add_library(cc_schemas)
target_sources(cc_schemas
    PUBLIC FILE_SET CXX_MODULES FILES
        schemas/validation_schemas.cppm
)
target_link_libraries(cc_schemas PUBLIC cc_utils)

# ─── cc_benchmarks: Pare Benchmark System ─────────────────────────────────────
add_library(cc_benchmarks)
target_sources(cc_benchmarks
    PUBLIC FILE_SET CXX_MODULES FILES
        benchmarks/pare/case_loader.cppm
        benchmarks/pare/cli.cppm
        benchmarks/pare/evaluator.cppm
        benchmarks/pare/execute_ref.cppm
        benchmarks/pare/metrics.cppm
        benchmarks/pare/run.cppm
        benchmarks/pare/schema.cppm
        benchmarks/pare/workspace.cppm
)
target_link_libraries(cc_benchmarks PUBLIC cc_utils yyjson)

# ─── cc_plugins: Plugin System ────────────────────────────────────────────────
add_library(cc_plugins)
target_sources(cc_plugins
    PUBLIC FILE_SET CXX_MODULES FILES
        plugins/loader.cppm
        plugins/marketplace.cppm
        plugins/plugin.cppm
)
target_link_libraries(cc_plugins PUBLIC cc_utils cc_types yyjson uv_a)

# ─── cc_core: Aggregator (links ALL sub-libraries) ────────────────────────────
# Convenience INTERFACE library that links all subsystems together.
add_library(cc_core INTERFACE)
target_link_libraries(cc_core
    INTERFACE
        cc_utils
        cc_state
        cc_constants
        cc_types
        cc_config
        cc_context
        cc_keybindings
        cc_memdir
        cc_coordinator
        cc_query
        cc_session
        cc_entrypoints
        cc_bootstrap
        cc_tasks
        cc_tools
        cc_commands
        cc_services
        cc_hooks
        cc_cli
        cc_bridge
        cc_skills_core
        cc_skills
        cc_ui
        cc_vim
        cc_daemon
        cc_remote
        cc_buddy
        cc_migrations
        cc_server
        cc_screens
        cc_schemas
        cc_benchmarks
        cc_plugins
        cc_task_types
        cc_history
        CURL::libcurl
        yyjson
        uv_a
)

# ─── cc_repl: Main Executable ─────────────────────────────────────────────────
add_executable(cc_repl)
target_sources(cc_repl
    PRIVATE
        main.cpp
)
target_link_libraries(cc_repl
    PRIVATE
        cc_core
        cc_daemon
        cc_server
        cc_ui_design
)
set_target_properties(cc_repl PROPERTIES
    OUTPUT_NAME "cc-repl"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ─── Pare Benchmark Executable ────────────────────────────────────────────────
add_executable(pare_benchmark)
target_sources(pare_benchmark
    PRIVATE
        benchmarks/pare/pare_benchmark_main.cpp
)
target_link_libraries(pare_benchmark
    PRIVATE
        cc_benchmarks
)
set_target_properties(pare_benchmark PROPERTIES
    OUTPUT_NAME "pare-benchmark"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
