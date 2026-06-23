/// @file tool_ui_file_read.cppm
/// @brief FileRead tool UI — userFacingName, renderToolUseMessage, etc.
///
/// Faithful TS port of ReadTool UI methods.
///
/// MODULE:   cc.ui.tools.file_read
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/ReadTool/ReadTool.tsx
///   - userFacingName: "Read"
///   - renderToolUseMessage: file path (+ line range if specified)
///   - renderToolUseTag: null
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.file_read;

import cc.ui.tools.registry;

export namespace cc::ui::tools::file_read_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "file_path" / "path" field from input JSON.
[[nodiscard]] inline std::string extract_file_path(std::string_view input_json) {
    auto pos = input_json.find("\"file_path\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"path\"");
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

/// Extract "offset" / "limit" numeric fields for line range display.
/// Returns empty string if not found.
[[nodiscard]] inline std::string extract_line_range(std::string_view input_json) {
    // Look for "offset": N and "limit": M
    auto off_pos = input_json.find("\"offset\"");
    auto lim_pos = input_json.find("\"limit\"");

    std::string offset_str, limit_str;

    if (off_pos != std::string_view::npos) {
        auto colon = input_json.find(':', off_pos);
        if (colon != std::string_view::npos) {
            // Skip whitespace and find digits
            std::size_t i = colon + 1;
            while (i < input_json.size() &&
                   (input_json[i] == ' ' || input_json[i] == '\t')) ++i;
            std::size_t start = i;
            while (i < input_json.size() &&
                   std::isdigit(static_cast<unsigned char>(input_json[i]))) ++i;
            if (i > start) {
                offset_str = std::string(input_json.substr(start, i - start));
            }
        }
    }

    if (lim_pos != std::string_view::npos) {
        auto colon = input_json.find(':', lim_pos);
        if (colon != std::string_view::npos) {
            std::size_t i = colon + 1;
            while (i < input_json.size() &&
                   (input_json[i] == ' ' || input_json[i] == '\t')) ++i;
            std::size_t start = i;
            while (i < input_json.size() &&
                   std::isdigit(static_cast<unsigned char>(input_json[i]))) ++i;
            if (i > start) {
                limit_str = std::string(input_json.substr(start, i - start));
            }
        }
    }

    if (offset_str.empty() && limit_str.empty()) return {};

    std::string result;
    if (!offset_str.empty()) {
        result += offset_str;
        if (!limit_str.empty()) {
            result += "–";
            int off = std::stoi(offset_str);
            int lim = std::stoi(limit_str);
            result += std::to_string(off + lim - 1);
        }
    }
    return result;
}

/// Display path with middle ellipsis.
[[nodiscard]] inline std::string display_path(std::string_view path,
                                               std::size_t max_len = 60) {
    if (path.size() <= max_len) return std::string(path);
    auto last_slash = path.rfind('/');
    if (last_slash == std::string_view::npos) last_slash = path.rfind('\\');
    if (last_slash == std::string_view::npos || last_slash == 0) {
        return "\xE2\x80\xA6" + std::string(path.substr(path.size() - max_len + 1));
    }
    std::string_view filename = path.substr(last_slash + 1);
    if (filename.size() >= max_len - 3) {
        return std::string(path.substr(0, max_len - 1)) + "\xE2\x80\xA6";
    }
    std::size_t head_len = max_len - 3 - filename.size() - 2;
    return std::string(path.substr(0, head_len)) + "\xE2\x80\xA6/" +
           std::string(filename);
}

}  // namespace detail

// ============================================================
// FileRead tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_file_read_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        return std::string{"Read"};
    };

    fns.message = [](std::string_view input_json) {
        std::string path = detail::extract_file_path(input_json);
        if (path.empty()) return std::string{"file"};

        std::string range = detail::extract_line_range(input_json);
        std::string display = detail::display_path(path, 60);

        if (!range.empty()) {
            return display + " :" + range;
        }
        return display;
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view /*partial*/) {
        std::string path = detail::extract_file_path(input_json);
        if (!path.empty()) {
            return "Reading " + detail::display_path(path, 50) + "\342\200\246";  // …
        }
        return std::string{"Reading file\342\200\246"};  // Reading file…
    };

    fns.queued = [](std::string_view input_json) {
        std::string path = detail::extract_file_path(input_json);
        if (!path.empty()) {
            return "Will read " + detail::display_path(path, 50);
        }
        return std::string{"Waiting to read file\342\200\246"};  // …
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register FileRead tool UI in the global registry.
inline void register_file_read_ui() {
    global_tool_ui_registry().register_tool_ui(
        "read_file", make_file_read_ui());
    global_tool_ui_registry().register_tool_ui(
        "Read", make_file_read_ui());
    global_tool_ui_registry().register_tool_ui(
        "FileRead", make_file_read_ui());
}

}  // namespace cc::ui::tools::file_read_ui
