/// @file tool_ui_grep.cppm
/// @brief Grep tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.grep
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/GrepTool/GrepTool.tsx
///   - userFacingName: "Search"
///   - renderToolUseMessage: pattern + path summary
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.grep;

import cc.ui.tools.registry;

export namespace cc::ui::tools::grep_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "pattern" field from input JSON.
[[nodiscard]] inline std::string extract_pattern(std::string_view input_json) {
    auto pos = input_json.find("\"pattern\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"query\"");
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

/// Extract "path" / "glob" field from input JSON.
[[nodiscard]] inline std::string extract_path(std::string_view input_json) {
    auto pos = input_json.find("\"path\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"glob\"");
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

/// Truncate pattern for display.
[[nodiscard]] inline std::string display_pattern(std::string_view pattern,
                                                  std::size_t max_len = 40) {
    if (pattern.size() <= max_len) return std::string{pattern};
    return std::string(pattern.substr(0, max_len)) + "\xE2\x80\xA6";  // …
}

}  // namespace detail

// ============================================================
// Grep tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_grep_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        return std::string{"Search"};
    };

    fns.message = [](std::string_view input_json) {
        std::string pattern = detail::extract_pattern(input_json);
        std::string path = detail::extract_path(input_json);

        std::string result;
        if (!pattern.empty()) {
            result = "\"" + detail::display_pattern(pattern, 30) + "\"";
        }
        if (!path.empty()) {
            if (!result.empty()) result += " in ";
            if (path.size() > 30) {
                result += "\xE2\x80\xA6" + path.substr(path.size() - 28);  // … + tail
            } else {
                result += path;
            }
        }
        if (result.empty()) result = "searching files…";
        return result;
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        if (!partial_result.empty()) {
            // Count lines in partial output as a rough match count
            std::size_t lines = 0;
            for (char c : partial_result) {
                if (c == '\n') ++lines;
            }
            if (lines > 0) {
                return "Found " + std::to_string(lines) + " matches so far…";
            }
        }
        std::string pattern = detail::extract_pattern(input_json);
        if (!pattern.empty()) {
            return "Searching for \"" + detail::display_pattern(pattern, 25) + "\"…";
        }
        return std::string{"Searching files…"};
    };

    fns.queued = [](std::string_view) {
        return std::string{"Waiting to search…"};
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register Grep tool UI in the global registry.
inline void register_grep_ui() {
    global_tool_ui_registry().register_tool_ui("grep", make_grep_ui());
    global_tool_ui_registry().register_tool_ui("Grep", make_grep_ui());
}

}  // namespace cc::ui::tools::grep_ui
