/// @file tool_ui_file_edit.cppm
/// @brief FileEdit tool UI — userFacingName, renderToolUseMessage, etc.
///
/// Faithful TS port of FileEditTool UI methods.
///
/// MODULE:   cc.ui.tools.file_edit
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/FileEditTool/FileEditTool.tsx
///   - userFacingName: "Update" / "Create" / "Updated plan"
///   - renderToolUseMessage: file path
///   - renderToolUseTag: "plan" if in plans directory
///   - isTransparentWrapper: false
///
/// NOTE: The tools layer has helpers in file_edit_prompt.cppm (user_facing_name,
/// get_tool_use_summary).  We re-implement the core logic here in the UI layer
/// to avoid cc_ui -> cc_tools dependency issues (cc_tools is heavier).
module;

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>

export module cc.ui.tools.file_edit;

import cc.ui.tools.registry;

export namespace cc::ui::tools::file_edit_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "file_path" field from input JSON.
/// Lightweight string extraction.
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

/// Extract "old_string" field from input JSON.
/// Returns empty string if not found or empty.
[[nodiscard]] inline std::string extract_old_string(std::string_view input_json) {
    auto pos = input_json.find("\"old_string\"");
    if (pos == std::string_view::npos) return {};
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    // Find matching close quote (handle escaped)
    std::size_t end = quote + 1;
    while (end < input_json.size() && input_json[end] != '"') {
        if (input_json[end] == '\\' && end + 1 < input_json.size()) {
            end += 2;
        } else {
            ++end;
        }
    }
    if (end >= input_json.size()) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

/// Extract basename + truncate middle for display.
/// Preserves filename at end.
[[nodiscard]] inline std::string display_path(std::string_view path,
                                               std::size_t max_len = 60) {
    if (path.size() <= max_len) return std::string(path);

    // Find the filename (last path component)
    auto last_slash = path.rfind('/');
    if (last_slash == std::string_view::npos) last_slash = path.rfind('\\');

    if (last_slash == std::string_view::npos || last_slash == 0) {
        // Just a filename or root-relative — truncate from head
        return "\xE2\x80\xA6" + std::string(path.substr(path.size() - max_len + 1));
    }

    std::string_view filename = path.substr(last_slash + 1);
    if (filename.size() >= max_len - 3) {
        // Filename alone is too long — truncate tail
        return std::string(path.substr(0, max_len - 1)) + "\xE2\x80\xA6";
    }

    // Keep start + …/ + filename
    std::size_t head_len = max_len - 3 - filename.size() - 2;  // -"…/" "-filename"
    return std::string(path.substr(0, head_len)) + "\xE2\x80\xA6/" +
           std::string(filename);
}

/// Check if path is in a plans directory.
[[nodiscard]] inline bool is_plan_file(std::string_view path) {
    // TS: getPlansDirectory() — typically .claude/plans or similar
    return path.find("plans/") != std::string_view::npos ||
           path.find(".plan") != std::string_view::npos;
}

}  // namespace detail

// ============================================================
// FileEdit tool UI functions
// ============================================================

/// Build ToolUIFunctions for the FileEdit tool.
[[nodiscard]] inline ToolUIFunctions make_file_edit_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view input_json) {
        std::string old_str = detail::extract_old_string(input_json);
        std::string path = detail::extract_file_path(input_json);

        // Check if it's a plan file
        if (detail::is_plan_file(path)) {
            return std::string{"Updated plan"};
        }
        // If old_string is empty, it's a create operation
        if (old_str.empty() && !path.empty()) {
            // Could be either create or replace_all; we can't tell for sure
            // from JSON alone.  Default to "Update" which is the common case.
            // TS checks input.oldString === undefined || input.oldString === ""
            // but also whether replace_all is true.  We approximate.
            if (input_json.find("\"replace_all\"") != std::string_view::npos) {
                if (input_json.find("true") != std::string_view::npos) {
                    return std::string{"Update"};
                }
            }
        }
        if (old_str.empty()) {
            return std::string{"Create"};
        }
        return std::string{"Update"};
    };

    fns.message = [](std::string_view input_json) {
        std::string path = detail::extract_file_path(input_json);
        if (path.empty()) return std::string{"file"};
        return detail::display_path(path, 60);
    };

    fns.tag = [](std::string_view input_json) -> std::optional<std::string> {
        std::string path = detail::extract_file_path(input_json);
        if (detail::is_plan_file(path)) {
            return std::string{"plan"};
        }
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view /*partial_result*/) {
        std::string path = detail::extract_file_path(input_json);
        if (!path.empty()) {
            return "Updating " + detail::display_path(path, 50) + "\342\200\246";  // …
        }
        return std::string{"Updating file\342\200\246"};  // Updating file…
    };

    fns.queued = [](std::string_view input_json) {
        std::string path = detail::extract_file_path(input_json);
        if (!path.empty()) {
            return "Will edit " + detail::display_path(path, 50);
        }
        return std::string{"Waiting to edit file\342\200\246"};  // …
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register FileEdit tool UI in the global registry.
inline void register_file_edit_ui() {
    global_tool_ui_registry().register_tool_ui(
        "edit_file", make_file_edit_ui());
    global_tool_ui_registry().register_tool_ui(
        "Edit", make_file_edit_ui());
    global_tool_ui_registry().register_tool_ui(
        "FileEdit", make_file_edit_ui());
}

}  // namespace cc::ui::tools::file_edit_ui
