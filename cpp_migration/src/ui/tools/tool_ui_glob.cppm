/// @file tool_ui_glob.cppm
/// @brief Glob tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.glob
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/GlobTool/GlobTool.ts
///   - userFacingName: "Glob"
///   - renderToolUseMessage: pattern + path summary
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.glob;

import cc.ui.tools.registry;

export namespace cc::ui::tools::glob_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "pattern" field from input JSON.
[[nodiscard]] inline std::string extract_pattern(std::string_view input_json) {
    auto pos = input_json.find("\"pattern\"");
    if (pos == std::string_view::npos) return {};
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = input_json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

/// Extract "path" field from input JSON.
[[nodiscard]] inline std::string extract_path(std::string_view input_json) {
    auto pos = input_json.find("\"path\"");
    if (pos == std::string_view::npos) return {};
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
// Glob tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_glob_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        // TS: GlobTool.userFacingName = "Search" (same as Grep)
        return std::string{"Search"};
    };

    fns.message = [](std::string_view input_json) {
        std::string pattern = detail::extract_pattern(input_json);
        std::string path = detail::extract_path(input_json);

        if (!pattern.empty()) {
            std::string result = pattern;
            if (!path.empty()) {
                result += " in " + path;
            }
            if (result.size() > 60) {
                return result.substr(0, 57) + "\xE2\x80\xA6";  // …
            }
            return result;
        }
        if (!path.empty()) return path;
        return std::string{"matching files…"};
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        if (!partial_result.empty()) {
            std::size_t count = 0;
            for (char c : partial_result) {
                if (c == '\n') ++count;
            }
            if (count > 0) {
                return "Found " + std::to_string(count) + " files so far…";
            }
        }
        std::string pattern = detail::extract_pattern(input_json);
        if (!pattern.empty()) {
            if (pattern.size() > 30) {
                return "Globbing " + pattern.substr(0, 27) + "\xE2\x80\xA6";
            }
            return "Globbing " + pattern + "\xE2\x80\xA6";
        }
        return std::string{"Globbing files…"};
    };

    fns.queued = [](std::string_view) {
        return std::string{"Waiting to glob…"};
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register Glob tool UI in the global registry.
inline void register_glob_ui() {
    global_tool_ui_registry().register_tool_ui("glob", make_glob_ui());
    global_tool_ui_registry().register_tool_ui("Glob", make_glob_ui());
    global_tool_ui_registry().register_tool_ui("GlobTool", make_glob_ui());
}

}  // namespace cc::ui::tools::glob_ui
