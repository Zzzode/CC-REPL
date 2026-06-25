/// @file tool_ui_generic.cppm
/// @brief Generic / fallback tool UI renderer.
///
/// Used when a tool has no specific UI registered.  Provides sensible
/// defaults: user-facing name = raw tool name (title-cased), message =
/// first line of input JSON, no tag, generic progress/queued text.
///
/// MODULE:   cc.ui.tools.generic
/// LICENCE:  Exported.  Imported by tool_ui_registry and as fallback.
///
/// TS REFERENCE:
///   Default tool behavior when no specialized UI is provided.
module;

#include <string>
#include <string_view>
#include <optional>
#include <cctype>

export module cc.ui.tools.generic;

import cc.ui.tools.registry;

export namespace cc::ui::tools::generic_tool {

using namespace cc::ui::tools;

// ============================================================
// Helpers
// ============================================================

namespace detail {

/// Convert a snake_case tool name to Title Case for display.
/// e.g. "file_edit" -> "File Edit", "bash" -> "Bash"
[[nodiscard]] inline std::string snake_to_title(std::string_view name) {
    std::string result;
    bool capitalize_next = true;
    for (char c : name) {
        if (c == '_' || c == '-' || c == ' ') {
            result += ' ';
            capitalize_next = true;
        } else {
            if (capitalize_next) {
                result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                capitalize_next = false;
            } else {
                result += c;
            }
        }
    }
    return result;
}

/// Extract first non-empty line of a JSON string, strip quotes if possible.
/// Used as a generic summary fallback.
[[nodiscard]] inline std::string first_line_summary(std::string_view input_json) {
    if (input_json.empty()) return {};

    // Skip leading whitespace/braces
    std::size_t start = 0;
    while (start < input_json.size() &&
           (input_json[start] == ' ' || input_json[start] == '\n' ||
            input_json[start] == '\t' || input_json[start] == '{')) {
        ++start;
    }

    std::size_t end = input_json.find('\n', start);
    std::string_view line = (end == std::string_view::npos)
        ? input_json.substr(start)
        : input_json.substr(start, end - start);

    // Trim trailing whitespace/braces
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' ||
            line.back() == '}' || line.back() == ',')) {
        line.remove_suffix(1);
    }

    // Truncate to ~80 chars
    constexpr std::size_t kMaxLen = 80;
    if (line.size() > kMaxLen) {
        return std::string(line.substr(0, kMaxLen)) + "\xE2\x80\xA6";  // …
    }
    return std::string(line);
}

}  // namespace detail

// ============================================================
// Generic tool UI
// ============================================================

/// Build a generic ToolUIFunctions bundle for a tool with the given name.
///
/// Used as the fallback when no specific UI is registered.
[[nodiscard]] inline ToolUIFunctions make_generic_ui(std::string tool_name) {
    ToolUIFunctions fns;

    fns.user_facing_name = [name = std::move(tool_name)](std::string_view) {
        return detail::snake_to_title(name);
    };

    fns.message = [](std::string_view input_json) {
        return detail::first_line_summary(input_json);
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view, std::string_view partial) {
        if (!partial.empty()) {
            // Show first line of partial output
            auto nl = partial.find('\n');
            std::string line = (nl == std::string_view::npos)
                ? std::string(partial)
                : std::string(partial.substr(0, nl));
            if (line.size() > 80) {
                line = line.substr(0, 80) + "\xE2\x80\xA6";
            }
            return line;
        }
        return std::string{"Running\342\200\246"};  // Running…
    };

    fns.queued = [](std::string_view) {
        return std::string{"Waiting\342\200\246"};  // Waiting…
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

}  // namespace cc::ui::tools::generic_tool

// ============================================================
// Registry: get_tool_ui_or_generic convenience
// ============================================================

export namespace cc::ui::tools {

/// Convenience: look up in the global registry with generic fallback.
/// Returns a copy — registered tools are copied, and unregistered tools
/// get a freshly-constructed generic renderer using the actual tool name.
[[nodiscard]] inline ToolUIFunctions get_tool_ui_or_generic(
    std::string_view name)
{
    auto& reg = global_tool_ui_registry();
    const ToolUIFunctions* found = reg.find(name);
    if (found) return *found;
    return generic_tool::make_generic_ui(std::string{name});
}

}  // namespace cc::ui::tools
