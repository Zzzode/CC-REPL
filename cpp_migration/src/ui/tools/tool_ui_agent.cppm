/// @file tool_ui_agent.cppm
/// @brief Agent tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.agent
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/AgentTool/AgentTool.tsx
///   - userFacingName: "Agent"
///   - renderToolUseMessage: task description
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.agent;

import cc.ui.tools.registry;

export namespace cc::ui::tools::agent_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "description" field from input JSON.
/// Falls back to "task" or "prompt" if not found.
[[nodiscard]] inline std::string extract_description(std::string_view input_json) {
    auto pos = input_json.find("\"description\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"task\"");
    }
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"prompt\"");
    }
    if (pos == std::string_view::npos) return {};
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = input_json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

/// Extract agent "name" field from input JSON, if present.
[[nodiscard]] inline std::string extract_agent_name(std::string_view input_json) {
    auto pos = input_json.find("\"agent_name\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"name\"");
        if (pos == std::string_view::npos) return {};
        // Make sure this is the agent name, not a task name
        // Crude heuristic: check if the field appears before "description"
        auto desc_pos = input_json.find("\"description\"");
        if (desc_pos != std::string_view::npos && pos > desc_pos) return {};
    }
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = input_json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

}  // namespace detail

// ============================================================
// Agent tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_agent_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view input_json) {
        std::string name = detail::extract_agent_name(input_json);
        if (!name.empty()) {
            return name;  // Named agent
        }
        return std::string{"Agent"};
    };

    fns.message = [](std::string_view input_json) {
        std::string desc = detail::extract_description(input_json);
        if (!desc.empty()) {
            if (desc.size() > 60) {
                return desc.substr(0, 57) + "\xE2\x80\xA6";  // …
            }
            return desc;
        }
        return std::string{"working on a task…"};
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        std::string name = detail::extract_agent_name(input_json);
        if (name.empty()) name = "agent";

        if (!partial_result.empty()) {
            // Show first line of partial output
            auto nl = partial_result.find('\n');
            std::string line = (nl == std::string_view::npos)
                ? std::string(partial_result)
                : std::string(partial_result.substr(0, nl));
            if (line.size() > 50) {
                line = line.substr(0, 47) + "\xE2\x80\xA6";
            }
            return name + ": " + line;
        }
        return name + " is working…";
    };

    fns.queued = [](std::string_view input_json) {
        std::string name = detail::extract_agent_name(input_json);
        if (!name.empty()) {
            return "Waiting for " + name + "…";
        }
        return std::string{"Waiting for agent…"};
    };

    fns.is_transparent_wrapper = true;

    return fns;
}

/// Register Agent tool UI in the global registry.
inline void register_agent_ui() {
    global_tool_ui_registry().register_tool_ui("agent", make_agent_ui());
    global_tool_ui_registry().register_tool_ui("Agent", make_agent_ui());
    global_tool_ui_registry().register_tool_ui("AgentTool", make_agent_ui());
}

}  // namespace cc::ui::tools::agent_ui
