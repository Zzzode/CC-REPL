/// @file tool_ui_lsp.cppm
/// @brief LSP tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.lsp
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/LSPTool/LSPTool.tsx
///   - userFacingName: "LSP"
///   - renderToolUseMessage: method name
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.lsp;

import cc.ui.tools.registry;

export namespace cc::ui::tools::lsp_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "method" field from input JSON.
[[nodiscard]] inline std::string extract_method(std::string_view input_json) {
    auto pos = input_json.find("\"method\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"operation\"");
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

/// Extract "server" / "language_server" field from input JSON.
[[nodiscard]] inline std::string extract_server(std::string_view input_json) {
    auto pos = input_json.find("\"server\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"language_server\"");
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
// LSP tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_lsp_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        return std::string{"LSP"};
    };

    fns.message = [](std::string_view input_json) {
        std::string method = detail::extract_method(input_json);
        std::string server = detail::extract_server(input_json);

        if (!method.empty()) {
            std::string result = method;
            if (!server.empty()) {
                result += " (" + server + ")";
            }
            if (result.size() > 60) {
                return result.substr(0, 57) + "\xE2\x80\xA6";
            }
            return result;
        }
        if (!server.empty()) {
            return server;
        }
        return std::string{"language server request…"};
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::string{"LSP"};
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        std::string method = detail::extract_method(input_json);

        if (!partial_result.empty()) {
            auto nl = partial_result.find('\n');
            std::string line = (nl == std::string_view::npos)
                ? std::string(partial_result)
                : std::string(partial_result.substr(0, nl));
            if (line.size() > 60) {
                line = line.substr(0, 57) + "\xE2\x80\xA6";
            }
            if (!method.empty()) {
                return method + ": " + line;
            }
            return line;
        }
        if (!method.empty()) {
            return "Calling " + method + "…";
        }
        return std::string{"LSP request in progress…"};
    };

    fns.queued = [](std::string_view input_json) {
        std::string method = detail::extract_method(input_json);
        if (!method.empty()) {
            return "Waiting for " + method + "…";
        }
        return std::string{"Waiting for LSP…"};
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register LSP tool UI in the global registry.
inline void register_lsp_ui() {
    global_tool_ui_registry().register_tool_ui("lsp", make_lsp_ui());
    global_tool_ui_registry().register_tool_ui("LSP", make_lsp_ui());
    global_tool_ui_registry().register_tool_ui("LSPTool", make_lsp_ui());
    global_tool_ui_registry().register_tool_ui("lsp_request", make_lsp_ui());
}

}  // namespace cc::ui::tools::lsp_ui
