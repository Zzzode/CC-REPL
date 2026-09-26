# ─── cc_tools: Tools System ───────────────────────────────────────────────────
add_library(cc_tools)
target_sources(cc_tools
    PUBLIC FILE_SET CXX_MODULES FILES
        tools/agent_color_manager.cppm
        tools/agent_constants.cppm
        tools/agent_memory.cppm
        tools/agent_memory_snapshot.cppm
        tools/agent_tool.cppm
        tools/agent_sub_utils.cppm
        tools/agent_run.cppm
        tools/agent_resume.cppm
        tools/agent_fork.cppm
        tools/agent_types.cppm
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
        tools/glob_tool.cppm
        tools/grep_tool.cppm
        tools/list_mcp_resources_tool.cppm
        tools/lsp_tool.cppm
        tools/mcp_classify.cppm
        tools/mcp_tool.cppm
        tools/notebook_tool.cppm
        tools/plan_mode_tool.cppm
        tools/powershell_tool.cppm
        tools/repl_tool.cppm
        tools/runtime_computer_use.cppm
        tools/runtime_message_delivery.cppm
        tools/runtime_registry.cppm
        tools/runtime_shared_utils.cppm
        tools/runtime_team_shared.cppm
        tools/script_diagnostics.cppm
        tools/script_tool.cppm
        tools/script_types.cppm
        tools/send_message_tool.cppm
        tools/shared_tool.cppm
        tools/skill_tool.cppm
        tools/sleep_tool.cppm
        tools/spawn_multi_agent.cppm
        tools/synthetic_output_tool.cppm
        tools/task_get.cppm
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
        tools/tool_registry.cppm
        tools/tungsten_tool.cppm
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
        tools/sed_edit_parser.cppm
        tools/sed_validation.cppm
        tools/script_typecheck.cppm
        tools/script_primitives.cppm
        tools/feature_flags.cppm
        # Phase 3-B/C real tool primitives: bash exec + file I/O + glob/grep
        tools/bash/impl_bash.cppm
        tools/files/impl_files.cppm
)
# RFC 0001 Phase C — runtime_registry module implementation units. Never add
# these to the FILE_SET CXX_MODULES list above: they are module impl units
# (`module cc.tools.runtime_registry;`), not interface units.
target_sources(cc_tools
    PRIVATE
        tools/runtime_registry_json.cpp
        tools/runtime_registry_executors.cpp
        tools/runtime_registry_native_agents.cpp
        tools/runtime_registry_computer_use.cpp
        tools/runtime_registry_skills.cpp
        tools/runtime_registry_dispatch.cpp
        tools/runtime_registry_team_dispatch.cpp
        tools/runtime_registry_register.cpp
        # RFC 0001 Phase C batch 3 — agent_runtime module implementation units
        # (`module cc.tools.agent_runtime;`); same PRIVATE-only discipline.
        tools/agent_runtime_text_impl.cpp
        tools/agent_runtime_yaml_impl.cpp
        tools/agent_runtime_json_impl.cpp
        tools/agent_runtime_builtin_impl.cpp
        tools/agent_runtime_sidechain_impl.cpp
        tools/agent_runtime_store_impl.cpp
        # RFC 0001 Phase C batch 4 — agent_sub_utils module implementation
        # units (`module cc.tools.agent.utils;`); same PRIVATE-only discipline.
        tools/agent_sub_utils_json.cpp
        tools/agent_sub_utils_config.cpp
        tools/agent_sub_utils_tools_mcp.cpp
        tools/agent_sub_utils_hooks.cpp
        tools/agent_sub_utils_teammates.cpp
        tools/agent_sub_utils_messages.cpp
        tools/agent_sub_utils_budget.cpp
        # RFC-0001 B4 — cc.tools.mcp loader-sink storage (`module cc.tools.mcp;`).
        tools/mcp_core_settings_loader.cpp
        # RFC-0001 B6 — cc.tools.mcp snapshot-sink storage (same discipline).
        tools/mcp_snapshots_sink.cpp
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

target_link_libraries(cc_query PUBLIC cc_tools cc_hooks cc_session cc_memdir cc_services)
