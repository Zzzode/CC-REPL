/// @file tool_ui_mcp.cppm
/// @brief MCP tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.mcp
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/MCPTool/MCPTool.tsx
///   - userFacingName: server name
///   - renderToolUseMessage: tool name + args summary
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.mcp;

import cc.ui.tools.registry;

export namespace cc::ui::tools::mcp_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "server_name" field from input JSON.
[[nodiscard]] inline std::string extract_server_name(std::string_view input_json) {
    auto pos = input_json.find("\"server_name\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"server\"");
        if (pos == std::string_view::npos) return {};
    }
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = input_json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

/// Extract "tool_name" field from input JSON.
[[nodiscard]] inline std::string extract_tool_name(std::string_view input_json) {
    auto pos = input_json.find("\"tool_name\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"tool\"");
        if (pos == std::string_view::npos) return {};
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
// MCP tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_mcp_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view input_json) {
        std::string server = detail::extract_server_name(input_json);
        if (!server.empty()) {
            return server;  // e.g. "mcp-server-fetch"
        }
        return std::string{"MCP"};
    };

    fns.message = [](std::string_view input_json) {
        std::string tool = detail::extract_tool_name(input_json);
        std::string server = detail::extract_server_name(input_json);

        if (!tool.empty()) {
            if (!server.empty()) {
                return tool + " (" + server + ")";
            }
            return tool;
        }
        if (!server.empty()) {
            return server;
        }
        return std::string{"MCP tool…"};
    };

    fns.tag = [](std::string_view input_json) -> std::optional<std::string> {
        std::string server = detail::extract_server_name(input_json);
        if (!server.empty()) {
            return "MCP";
        }
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        std::string tool = detail::extract_tool_name(input_json);
        if (!partial_result.empty()) {
            auto nl = partial_result.find('\n');
            std::string line = (nl == std::string_view::npos)
                ? std::string(partial_result)
                : std::string(partial_result.substr(0, nl));
            if (line.size() > 60) {
                line = line.substr(0, 57) + "\xE2\x80\xA6";
            }
            if (!tool.empty()) {
                return tool + ": " + line;
            }
            return line;
        }
        if (!tool.empty()) {
            return "Running " + tool + "…";
        }
        return std::string{"Running MCP tool…"};
    };

    fns.queued = [](std::string_view input_json) {
        std::string tool = detail::extract_tool_name(input_json);
        if (!tool.empty()) {
            return "Waiting to run " + tool + "…";
        }
        return std::string{"Waiting for MCP…"};
    };

    fns.is_transparent_wrapper = false;

    // TS REF: MCPTool — renderToolResultMessage shows MCP tool output
    // (text content from MCP server responses).  Output IS visible on
    // screen; index it for search.
    fns.extract_search_text = [](
        std::string_view output_text,
        std::string_view /*error_text*/) -> std::optional<std::string>
    {
        return std::string{output_text};
    };

    return fns;
}

/// Register MCP tool UI in the global registry.
inline void register_mcp_ui() {
    global_tool_ui_registry().register_tool_ui("mcp", make_mcp_ui());
    global_tool_ui_registry().register_tool_ui("MCP", make_mcp_ui());
    global_tool_ui_registry().register_tool_ui("MCPTool", make_mcp_ui());
    global_tool_ui_registry().register_tool_ui("mcp_call", make_mcp_ui());
}

}  // namespace cc::ui::tools::mcp_ui
