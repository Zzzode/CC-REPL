/// @file task_list_ui.cppm
/// @brief Task list display with status indicators, progress bars, and filtering.
/// Migrated from the upstream task list components.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <ranges>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.task_list_ui;

import cc.ui.layout;

export namespace cc::ui::task_list_ui {
using namespace ftxui;

// ============================================================
// Task Status Utilities (migrated from taskStatusUtils.tsx)
// ============================================================

/// Runtime task status (matches TypeScript TaskStatus union)
enum class RuntimeTaskStatus : std::uint8_t {
    Queued,
    Running,
    Completed,
    Failed,
    Killed,
};

/// Options for determining task display attributes
struct TaskDisplayOptions {
    bool is_idle = false;
    bool awaiting_approval = false;
    bool has_error = false;
    bool shutdown_requested = false;
};

/// Returns true if the status is terminal (finished state)
[[nodiscard]] inline bool is_terminal_status(RuntimeTaskStatus status) {
    return status == RuntimeTaskStatus::Completed
        || status == RuntimeTaskStatus::Failed
        || status == RuntimeTaskStatus::Killed;
}

/// Get the appropriate icon for a runtime task status
[[nodiscard]] inline std::string get_task_status_icon(RuntimeTaskStatus status) {
    switch (status) {
        case RuntimeTaskStatus::Queued:    return "●";
        case RuntimeTaskStatus::Running:   return "▶";
        case RuntimeTaskStatus::Completed: return "✓";
        case RuntimeTaskStatus::Failed:    return "✗";
        case RuntimeTaskStatus::Killed:    return "✗";
    }
    return "?";
}

/// Get the appropriate icon for a runtime task with display options
[[nodiscard]] inline std::string get_task_status_icon(
    RuntimeTaskStatus status, const TaskDisplayOptions& opts) {
    if (opts.has_error) return "✗";
    if (opts.awaiting_approval) return "?";
    if (opts.shutdown_requested) return "⚠";
    if (status == RuntimeTaskStatus::Running) {
        if (opts.is_idle) return "…";
        return "▶";
    }
    return get_task_status_icon(status);
}

/// Semantic color mapping for task status
enum class TaskSemanticColor : std::uint8_t {
    Success,
    Error,
    Warning,
    Background,
};

/// Get semantic color for a task status
[[nodiscard]] inline TaskSemanticColor get_task_status_color(RuntimeTaskStatus status) {
    switch (status) {
        case RuntimeTaskStatus::Completed: return TaskSemanticColor::Success;
        case RuntimeTaskStatus::Failed:    return TaskSemanticColor::Error;
        case RuntimeTaskStatus::Killed:    return TaskSemanticColor::Warning;
        default:                           return TaskSemanticColor::Background;
    }
}

/// Get semantic color for a task with display options
[[nodiscard]] inline TaskSemanticColor get_task_status_color(
    RuntimeTaskStatus status, const TaskDisplayOptions& opts) {
    if (opts.has_error) return TaskSemanticColor::Error;
    if (opts.awaiting_approval) return TaskSemanticColor::Warning;
    if (opts.shutdown_requested) return TaskSemanticColor::Warning;
    if (opts.is_idle) return TaskSemanticColor::Background;
    return get_task_status_color(status);
}

/// Map semantic color to FTXUI Color
[[nodiscard]] inline Color semantic_to_ftxui(TaskSemanticColor sc) {
    switch (sc) {
        case TaskSemanticColor::Success:    return Color::Green;
        case TaskSemanticColor::Error:      return Color::Red;
        case TaskSemanticColor::Warning:    return Color::Yellow;
        case TaskSemanticColor::Background: return Color::GrayLight;
    }
    return Color::White;
}

// ============================================================
// Background Task Types (migrated from BackgroundTask.tsx)
// ============================================================

/// Task type discriminator
enum class BackgroundTaskType : std::uint8_t {
    Shell,
    RemoteSession,
    InProcessTeammate,
    Dream,
    AsyncAgent,
};

/// Activity info for a background task
struct TaskActivity {
    std::string description;
    std::optional<double> progress;  // 0.0 - 1.0
    std::vector<std::string> recent_activities;
};

/// A background task entry for display
struct BackgroundTaskEntry {
    std::string id;
    std::string name;
    BackgroundTaskType type;
    RuntimeTaskStatus status;
    TaskDisplayOptions display_opts;
    std::optional<TaskActivity> activity;
    std::chrono::steady_clock::time_point started_at;
    std::optional<std::chrono::steady_clock::time_point> ended_at;
};

// ============================================================
// Filter Configuration
// ============================================================

/// Filter mode for the task list
enum class TaskFilterMode : std::uint8_t {
    All,
    Running,
    Completed,
    Failed,
    ByType,
};

/// Task list filter options
struct TaskFilterConfig {
    TaskFilterMode mode = TaskFilterMode::All;
    std::optional<BackgroundTaskType> type_filter;
    std::string search_query;
};

// ============================================================
// Rendering Functions
// ============================================================

/// Render a single background task line
[[nodiscard]] inline Element RenderBackgroundTaskLine(
    const BackgroundTaskEntry& task, bool selected) {

    auto icon_str = get_task_status_icon(task.status, task.display_opts);
    auto sc = get_task_status_color(task.status, task.display_opts);
    auto icon_color = semantic_to_ftxui(sc);

    Elements parts = {
        text(" " + icon_str + " ") | color(icon_color),
        text(task.name) | (selected ? bold : nothing)
            | color(is_terminal_status(task.status) ? Color::GrayDark : Color::White),
    };

    // Activity description
    if (task.activity && !task.activity->description.empty()) {
        parts.push_back(text(" · ") | dim);
        parts.push_back(text(task.activity->description) | dim | color(Color::Cyan));
    }

    parts.push_back(filler());

    // Type badge
    std::string type_badge;
    switch (task.type) {
        case BackgroundTaskType::Shell:             type_badge = "shell"; break;
        case BackgroundTaskType::RemoteSession:     type_badge = "remote"; break;
        case BackgroundTaskType::InProcessTeammate: type_badge = "teammate"; break;
        case BackgroundTaskType::Dream:             type_badge = "dream"; break;
        case BackgroundTaskType::AsyncAgent:        type_badge = "agent"; break;
    }
    parts.push_back(text(" [" + type_badge + "] ") | dim | color(Color::Blue));

    auto line = hbox(parts);

    // Optional progress bar
    Elements result_parts = {line};
    if (task.activity && task.activity->progress
        && task.status == RuntimeTaskStatus::Running) {
        result_parts.push_back(hbox({
            text("   ") | dim,
            gauge(*task.activity->progress) | color(Color::Cyan)
                | size(WIDTH, LESS_THAN, 30),
            text(std::format(" {:.0f}%", *task.activity->progress * 100)) | dim,
        }));
    }

    auto result = vbox(result_parts);
    if (selected) {
        result = result | bgcolor(Color::RGB(25, 30, 45));
    }
    return result;
}

/// Render the task list header/summary
[[nodiscard]] inline Element RenderTaskListHeader(
    const std::vector<BackgroundTaskEntry>& tasks, const TaskFilterConfig& filter) {

    int running = 0, completed = 0, failed = 0;
    for (const auto& t : tasks) {
        switch (t.status) {
            case RuntimeTaskStatus::Running: ++running; break;
            case RuntimeTaskStatus::Completed: ++completed; break;
            case RuntimeTaskStatus::Failed:
            case RuntimeTaskStatus::Killed: ++failed; break;
            default: break;
        }
    }

    std::string filter_label;
    switch (filter.mode) {
        case TaskFilterMode::All:       filter_label = "all"; break;
        case TaskFilterMode::Running:   filter_label = "running"; break;
        case TaskFilterMode::Completed: filter_label = "done"; break;
        case TaskFilterMode::Failed:    filter_label = "failed"; break;
        case TaskFilterMode::ByType:    filter_label = "type"; break;
    }

    return hbox({
        text(" Tasks ") | bold | color(Color::Blue),
        text(std::format("({} total", static_cast<int>(tasks.size()))) | dim,
        text(std::format(", {}▶", running)) | color(Color::Cyan) | dim,
        text(std::format(", {}✓", completed)) | color(Color::Green) | dim,
        text(std::format(", {}✗)", failed)) | color(Color::Red) | dim,
        filler(),
        text(" filter: " + filter_label + " ") | dim | color(Color::Yellow),
    });
}

/// Options for the full task list UI
struct TaskListUIOptions {
    std::vector<BackgroundTaskEntry> tasks;
    TaskFilterConfig filter;
    int selected_index = 0;

    std::function<void(const std::string& task_id)> on_select;
    std::function<void(const std::string& task_id)> on_kill;
    std::function<void()> on_close;
    std::function<void(TaskFilterMode)> on_filter_change;
};

/// Apply filter to get visible tasks
[[nodiscard]] inline std::vector<const BackgroundTaskEntry*> apply_filter(
    const std::vector<BackgroundTaskEntry>& tasks, const TaskFilterConfig& filter) {

    std::vector<const BackgroundTaskEntry*> result;
    for (const auto& t : tasks) {
        bool pass = true;
        switch (filter.mode) {
            case TaskFilterMode::Running:
                pass = (t.status == RuntimeTaskStatus::Running); break;
            case TaskFilterMode::Completed:
                pass = (t.status == RuntimeTaskStatus::Completed); break;
            case TaskFilterMode::Failed:
                pass = (t.status == RuntimeTaskStatus::Failed
                     || t.status == RuntimeTaskStatus::Killed); break;
            case TaskFilterMode::ByType:
                pass = (!filter.type_filter || t.type == *filter.type_filter); break;
            case TaskFilterMode::All:
                break;
        }
        if (pass && !filter.search_query.empty()) {
            pass = t.name.find(filter.search_query) != std::string::npos;
        }
        if (pass) result.push_back(&t);
    }
    return result;
}

/// Render the full task list UI
[[nodiscard]] inline Element RenderTaskListUI(const TaskListUIOptions& opts) {
    auto header = RenderTaskListHeader(opts.tasks, opts.filter);
    auto visible = apply_filter(opts.tasks, opts.filter);

    Elements task_elements;
    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        task_elements.push_back(
            RenderBackgroundTaskLine(*visible[i], i == opts.selected_index));
    }

    if (task_elements.empty()) {
        task_elements.push_back(
            text(" No tasks matching filter") | dim | center);
    }

    auto list = vbox(task_elements) | vscroll_indicator | yframe | flex;

    auto actions = hbox({
        text(" [Enter]") | color(Color::Cyan), text(" details "),
        text("[k]") | color(Color::Cyan), text("ill "),
        text("[f]") | color(Color::Cyan), text("ilter "),
        text("[/]") | color(Color::Cyan), text(" search "),
        text("[Esc]") | color(Color::Cyan), text(" close"),
    }) | dim;

    return vbox({
        header,
        separator(),
        list,
        separator(),
        actions,
    }) | borderRounded;
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a full task list UI component
[[nodiscard]] inline Component TaskListUI(TaskListUIOptions options) {
    auto state = std::make_shared<TaskListUIOptions>(std::move(options));

    return Renderer([state] {
        return RenderTaskListUI(*state);
    }) | CatchEvent([state](Event event) -> bool {
        auto visible = apply_filter(state->tasks, state->filter);
        int count = static_cast<int>(visible.size());

        if (event == Event::Escape) {
            if (state->on_close) state->on_close();
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index = std::max(0, state->selected_index - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index = std::min(count - 1, state->selected_index + 1);
            return true;
        }
        if (event == Event::Return) {
            if (count > 0 && state->on_select) {
                state->on_select(visible[state->selected_index]->id);
            }
            return true;
        }
        if (event == Event::Character('K')) {
            if (count > 0 && state->on_kill) {
                state->on_kill(visible[state->selected_index]->id);
            }
            return true;
        }
        if (event == Event::Character('f')) {
            // Cycle filter mode
            auto next = static_cast<TaskFilterMode>(
                (static_cast<int>(state->filter.mode) + 1) % 5);
            state->filter.mode = next;
            state->selected_index = 0;
            if (state->on_filter_change) state->on_filter_change(next);
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::task_list_ui
