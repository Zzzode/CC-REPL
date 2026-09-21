/// @file tool_ui_init.cppm
/// @brief Register all built-in tool UIs in the global registry.
///
/// Call `register_builtin_tool_uis()` once at startup to populate the
/// global tool UI registry with all built-in tool UI renderers.
///
/// MODULE:   cc.ui.tools.init
/// LICENCE:  Exported.  Imported by app initialization code.
module;

export module cc.ui.tools.init;

import cc.ui.tools.registry;
import cc.ui.tools.generic;
import cc.ui.tools.bash;
import cc.ui.tools.file_edit;
import cc.ui.tools.file_write;
import cc.ui.tools.file_read;
import cc.ui.tools.grep;
import cc.ui.tools.glob;
import cc.ui.tools.web_fetch;
import cc.ui.tools.web_search;
import cc.ui.tools.skill;
import cc.ui.tools.agent;
import cc.ui.tools.task;
import cc.ui.tools.mcp;
import cc.ui.tools.lsp;
import cc.ui.tools.longtail;

export namespace cc::ui::tools {

/// Register all built-in tool UIs in the global registry.
/// Safe to call multiple times (idempotent — checks for existing entries).
inline void register_builtin_tool_uis() {
    auto& reg = global_tool_ui_registry();
    if (reg.size() > 0) {
        // Already initialized — skip.
        // NOTE: this check is approximate (someone could have registered
        // only some tools), but it's good enough for startup-time safety.
        return;
    }

    // Core tools (highest usage)
    bash_ui::register_bash_ui();
    file_edit_ui::register_file_edit_ui();
    file_write_ui::register_file_write_ui();
    file_read_ui::register_file_read_ui();

    // Search & web tools
    grep_ui::register_grep_ui();
    glob_ui::register_glob_ui();
    web_fetch_ui::register_web_fetch_ui();
    web_search_ui::register_web_search_ui();

    // Agent / skill / task tools
    skill_ui::register_skill_ui();
    agent_ui::register_agent_ui();
    task_ui::register_task_create_ui();
    task_ui::register_task_update_ui();

    // Protocol tools
    mcp_ui::register_mcp_ui();
    lsp_ui::register_lsp_ui();

    // Long-tail tools (all remaining)
    longtail_ui::register_longtail_tool_uis();
}

}  // namespace cc::ui::tools
