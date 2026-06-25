/// @file tool_ui_file_write.cppm
/// @brief FileWrite tool UI — userFacingName, renderToolUseMessage, etc.
///
/// Faithful TS port of FileWriteTool UI methods.
///
/// MODULE:   cc.ui.tools.file_write
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/FileWriteTool/FileWriteTool.tsx
///   - userFacingName: "Write"
///   - renderToolUseMessage: file path
///   - renderToolUseTag: null
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.file_write;

import cc.ui.tools.registry;

export namespace cc::ui::tools::file_write_ui {

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
// FileWrite tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_file_write_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        return std::string{"Write"};
    };

    fns.message = [](std::string_view input_json) {
        std::string path = detail::extract_file_path(input_json);
        if (path.empty()) return std::string{"file"};
        return detail::display_path(path, 60);
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view /*partial*/) {
        std::string path = detail::extract_file_path(input_json);
        if (!path.empty()) {
            return "Writing " + detail::display_path(path, 50) + "\342\200\246";  // …
        }
        return std::string{"Writing file\342\200\246"};  // Writing file…
    };

    fns.queued = [](std::string_view input_json) {
        std::string path = detail::extract_file_path(input_json);
        if (!path.empty()) {
            return "Will write " + detail::display_path(path, 50);
        }
        return std::string{"Waiting to write file\342\200\246"};  // …
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register FileWrite tool UI in the global registry.
inline void register_file_write_ui() {
    global_tool_ui_registry().register_tool_ui(
        "write_file", make_file_write_ui());
    global_tool_ui_registry().register_tool_ui(
        "Write", make_file_write_ui());
    global_tool_ui_registry().register_tool_ui(
        "FileWrite", make_file_write_ui());
}

}  // namespace cc::ui::tools::file_write_ui
