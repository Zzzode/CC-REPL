/// @file tool_ui_task.cppm
/// @brief Task tool UI — TaskCreate / TaskUpdate
///
/// MODULE:   cc.ui.tools.task
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/TaskCreateTool/TaskCreateTool.tsx
///   src/tools/TaskUpdateTool/TaskUpdateTool.tsx
///   - userFacingName: "Create task" / "Update task"
///   - renderToolUseMessage: task subject
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.task;

import cc.ui.tools.registry;

export namespace cc::ui::tools::task_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "subject" or "title" field from input JSON.
[[nodiscard]] inline std::string extract_subject(std::string_view input_json) {
    auto pos = input_json.find("\"subject\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"title\"");
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

/// Extract "task_id" field from input JSON.
[[nodiscard]] inline std::string extract_task_id(std::string_view input_json) {
    auto pos = input_json.find("\"task_id\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"taskId\"");
        if (pos == std::string_view::npos) return {};
    }
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    // Could be a string or a number
    auto quote = input_json.find('"', colon + 1);
    if (quote != std::string_view::npos && quote - colon < 3) {
        auto end = input_json.find('"', quote + 1);
        if (end == std::string_view::npos) return {};
        return std::string(input_json.substr(quote + 1, end - quote - 1));
    }
    // Numeric id
    auto start = colon + 1;
    while (start < input_json.size() && input_json[start] == ' ') ++start;
    auto end = start;
    while (end < input_json.size() &&
           (input_json[end] >= '0' && input_json[end] <= '9')) ++end;
    if (end == start) return {};
    return std::string(input_json.substr(start, end - start));
}

/// Extract "status" field from input JSON (for TaskUpdate).
[[nodiscard]] inline std::string extract_status(std::string_view input_json) {
    auto pos = input_json.find("\"status\"");
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
// TaskCreate tool UI
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_task_create_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        // TS: TaskCreateTool.userFacingName = "TaskCreate"
        return std::string{"TaskCreate"};
    };

    fns.message = [](std::string_view input_json) {
        std::string subject = detail::extract_subject(input_json);
        if (!subject.empty()) {
            if (subject.size() > 60) {
                return subject.substr(0, 57) + "\xE2\x80\xA6";
            }
            return subject;
        }
        return std::string{"new task…"};
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view, std::string_view) {
        return std::string{"Creating task…"};
    };

    fns.queued = [](std::string_view) {
        return std::string{"Waiting to create task…"};
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

// ============================================================
// TaskUpdate tool UI
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_task_update_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        // TS: TaskUpdateTool.userFacingName = "TaskUpdate"
        return std::string{"TaskUpdate"};
    };

    fns.message = [](std::string_view input_json) {
        std::string id = detail::extract_task_id(input_json);
        std::string status = detail::extract_status(input_json);
        std::string subject = detail::extract_subject(input_json);

        std::string result;
        if (!id.empty()) {
            result = "#" + id;
        }
        if (!status.empty()) {
            if (!result.empty()) result += " → ";
            result += status;
        }
        if (!subject.empty()) {
            if (!result.empty()) result += ": ";
            if (subject.size() > 40) {
                result += subject.substr(0, 37) + "\xE2\x80\xA6";
            } else {
                result += subject;
            }
        }
        if (result.empty()) result = "updating task…";
        return result;
    };

    fns.tag = [](std::string_view input_json) -> std::optional<std::string> {
        std::string status = detail::extract_status(input_json);
        if (!status.empty()) return status;
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view) {
        std::string id = detail::extract_task_id(input_json);
        if (!id.empty()) {
            return "Updating task #" + id + "…";
        }
        return std::string{"Updating task…"};
    };

    fns.queued = [](std::string_view input_json) {
        std::string id = detail::extract_task_id(input_json);
        if (!id.empty()) {
            return "Waiting to update task #" + id + "…";
        }
        return std::string{"Waiting to update task…"};
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register TaskCreate tool UI in the global registry.
inline void register_task_create_ui() {
    global_tool_ui_registry().register_tool_ui(
        "task_create", make_task_create_ui());
    global_tool_ui_registry().register_tool_ui(
        "TaskCreate", make_task_create_ui());
    global_tool_ui_registry().register_tool_ui(
        "TaskCreateTool", make_task_create_ui());
}

/// Register TaskUpdate tool UI in the global registry.
inline void register_task_update_ui() {
    global_tool_ui_registry().register_tool_ui(
        "task_update", make_task_update_ui());
    global_tool_ui_registry().register_tool_ui(
        "TaskUpdate", make_task_update_ui());
    global_tool_ui_registry().register_tool_ui(
        "TaskUpdateTool", make_task_update_ui());
}

}  // namespace cc::ui::tools::task_ui
