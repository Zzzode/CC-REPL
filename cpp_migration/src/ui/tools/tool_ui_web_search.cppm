/// @file tool_ui_web_search.cppm
/// @brief WebSearch tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.web_search
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/WebSearchTool/WebSearchTool.tsx
///   - userFacingName: "Search web"
///   - renderToolUseMessage: query string
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.web_search;

import cc.ui.tools.registry;

export namespace cc::ui::tools::web_search_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "query" field from input JSON.
[[nodiscard]] inline std::string extract_query(std::string_view input_json) {
    auto pos = input_json.find("\"query\"");
    if (pos == std::string_view::npos) return {};
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = input_json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

/// Truncate query for display.
[[nodiscard]] inline std::string display_query(std::string_view query,
                                                std::size_t max_len = 50) {
    if (query.size() <= max_len) return std::string{query};
    return std::string(query.substr(0, max_len)) + "\xE2\x80\xA6";  // …
}

}  // namespace detail

// ============================================================
// WebSearch tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_web_search_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        // TS: WebSearchTool.userFacingName = "Web Search"
        return std::string{"Web Search"};
    };

    fns.message = [](std::string_view input_json) {
        std::string query = detail::extract_query(input_json);
        if (!query.empty()) {
            return "\"" + detail::display_query(query, 40) + "\"";
        }
        return std::string{"web search…"};
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        std::string query = detail::extract_query(input_json);
        if (!partial_result.empty()) {
            // Count results
            std::size_t lines = 0;
            for (char c : partial_result) {
                if (c == '\n') ++lines;
            }
            if (lines > 1) {
                return "Found " + std::to_string(lines) + " results…";
            }
            return std::string{"Reading results…"};
        }
        if (!query.empty()) {
            return "Searching for \"" + detail::display_query(query, 30) + "\"…";
        }
        return std::string{"Searching web…"};
    };

    fns.queued = [](std::string_view) {
        return std::string{"Waiting to search web…"};
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register WebSearch tool UI in the global registry.
inline void register_web_search_ui() {
    global_tool_ui_registry().register_tool_ui("web_search", make_web_search_ui());
    global_tool_ui_registry().register_tool_ui("WebSearch", make_web_search_ui());
    global_tool_ui_registry().register_tool_ui("WebSearchTool", make_web_search_ui());
}

}  // namespace cc::ui::tools::web_search_ui
