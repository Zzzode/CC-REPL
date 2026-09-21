/// @file task_view.cppm
/// @brief Task list and progress view - displays todo items, their status,
/// dependencies, and progress with interactive management.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.task_view;

import cc.types.types;

export namespace cc::ui::components::task_view {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Task status
enum class TaskStatus : std::uint8_t {
    Pending,        // Not started
    InProgress,     // Currently being worked on
    Completed,      // Done
    Failed,         // Failed
    Blocked,        // Waiting on dependencies
    Cancelled,      // Cancelled
};

/// Task priority
enum class TaskPriority : std::uint8_t {
    Low,
    Medium,
    High,
    Critical,
};

/// A single task entry
struct TaskEntry {
    std::string id;
    std::string subject;
    std::string description;
    TaskStatus status;
    TaskPriority priority = TaskPriority::Medium;
    std::optional<std::string> assignee;    // Agent or user
    std::optional<std::string> active_form; // Current activity description
    std::vector<std::string> blocked_by;    // IDs of blocking tasks
    std::vector<std::string> blocks;        // IDs of tasks this blocks
    std::chrono::steady_clock::time_point created_at;
    std::optional<std::chrono::steady_clock::time_point> completed_at;
    std::optional<double> progress;         // 0.0 - 1.0
};

/// Summary statistics
struct TaskSummary {
    int total = 0;
    int pending = 0;
    int in_progress = 0;
    int completed = 0;
    int failed = 0;
    int blocked = 0;
};

/// Options for the task view
struct TaskViewOptions {
    std::vector<TaskEntry> tasks;
    int selected_index = 0;
    int scroll_offset = 0;
    bool show_completed = true;
    bool show_description = false;
    std::optional<TaskStatus> filter_status;

    std::function<void(const std::string& task_id, TaskStatus new_status)> on_status_change;
    std::function<void(const std::string& task_id)> on_select;
    std::function<void(const std::string& task_id)> on_delete;
    std::function<void()> on_add;
    std::function<void()> on_close;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get status icon and color
[[nodiscard]] inline std::pair<std::string, Color> status_display(TaskStatus status) {
    switch (status) {
        case TaskStatus::Pending:    return {"○", Color::GrayLight};
        case TaskStatus::InProgress: return {"◐", Color::Cyan};
        case TaskStatus::Completed:  return {"✓", Color::Green};
        case TaskStatus::Failed:     return {"✗", Color::Red};
        case TaskStatus::Blocked:    return {"⊗", Color::Yellow};
        case TaskStatus::Cancelled:  return {"⊘", Color::GrayDark};
    }
    return {"?", Color::White};
}

/// Get priority indicator
[[nodiscard]] inline std::pair<std::string, Color> priority_display(TaskPriority p) {
    switch (p) {
        case TaskPriority::Low:      return {"↓", Color::GrayLight};
        case TaskPriority::Medium:   return {"→", Color::Blue};
        case TaskPriority::High:     return {"↑", Color::Yellow};
        case TaskPriority::Critical: return {"⚠", Color::Red};
    }
    return {"?", Color::White};
}

/// Compute summary from task list
[[nodiscard]] inline TaskSummary compute_summary(const std::vector<TaskEntry>& tasks) {
    TaskSummary s;
    s.total = static_cast<int>(tasks.size());
    for (const auto& t : tasks) {
        switch (t.status) {
            case TaskStatus::Pending:    ++s.pending; break;
            case TaskStatus::InProgress: ++s.in_progress; break;
            case TaskStatus::Completed:  ++s.completed; break;
            case TaskStatus::Failed:     ++s.failed; break;
            case TaskStatus::Blocked:    ++s.blocked; break;
            case TaskStatus::Cancelled:  break;
        }
    }
    return s;
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single task line
[[nodiscard]] inline Element RenderTaskLine(
    const TaskEntry& task, bool selected, bool show_desc) {

    auto [status_icon, status_color] = status_display(task.status);
    auto [priority_icon, priority_color] = priority_display(task.priority);

    // Main line
    Elements parts = {
        text(" " + status_icon + " ") | color(status_color),
        text(priority_icon + " ") | color(priority_color),
        text(task.subject) | (selected ? bold : nothing)
            | color(task.status == TaskStatus::Completed ? Color::GrayDark : Color::White),
    };

    // Assignee badge
    if (task.assignee) {
        parts.push_back(filler());
        parts.push_back(text(" @" + *task.assignee + " ")
                        | dim | color(Color::Magenta));
    } else {
        parts.push_back(filler());
    }

    // Blocked indicator
    if (!task.blocked_by.empty()) {
        parts.push_back(text(std::format(" ⊗{}", task.blocked_by.size()))
                        | color(Color::Yellow) | dim);
    }

    // ID
    parts.push_back(text(" #" + task.id) | dim | color(Color::GrayDark));
    parts.push_back(text(" "));

    auto line = hbox(parts);

    // Optional progress bar
    Elements result_parts = {line};
    if (task.progress && task.status == TaskStatus::InProgress) {
        result_parts.push_back(hbox({
            text("   ") | dim,
            gauge(*task.progress) | color(Color::Cyan)
                | size(WIDTH, LESS_THAN, 30),
            text(std::format(" {:.0f}%", *task.progress * 100)) | dim,
        }));
    }

    // Active form (what the agent is currently doing)
    if (task.active_form && task.status == TaskStatus::InProgress) {
        result_parts.push_back(hbox({
            text("   ") | dim,
            text("⟳ " + *task.active_form) | dim | color(Color::Cyan),
        }));
    }

    // Description (if expanded)
    if (show_desc && selected && !task.description.empty()) {
        result_parts.push_back(hbox({
            text("   ") | dim,
            paragraph(task.description) | dim | color(Color::GrayLight)
                | size(WIDTH, LESS_THAN, 70),
        }));
    }

    auto result = vbox(result_parts);
    if (selected) {
        result = result | bgcolor(Color::RGB(25, 30, 45));
    }
    return result;
}

/// Render task summary bar
[[nodiscard]] inline Element RenderTaskSummary(const TaskSummary& s) {
    double progress = s.total > 0
        ? static_cast<double>(s.completed) / s.total : 0.0;

    return hbox({
        text(" 📋 Tasks ") | bold | color(Color::Blue),
        text(std::format("{}/{}", s.completed, s.total)) | dim,
        text("  ") | dim,
        gauge(progress) | color(Color::Green) | size(WIDTH, EQUAL, 15),
        text("  ") | dim,
        text(std::format("{}⏳", s.in_progress)) | color(Color::Cyan) | dim,
        text("  ") | dim,
        text(std::format("{}⊗", s.blocked)) | color(Color::Yellow) | dim,
        text("  ") | dim,
        text(std::format("{}✗", s.failed)) | color(Color::Red) | dim,
        filler(),
    });
}

/// Render the full task view
[[nodiscard]] inline Element RenderTaskView(const TaskViewOptions& opts) {
    auto summary = compute_summary(opts.tasks);

    // Summary bar
    auto summary_bar = RenderTaskSummary(summary);

    // Task list
    Elements task_elements;
    for (int i = 0; i < static_cast<int>(opts.tasks.size()); ++i) {
        const auto& task = opts.tasks[i];

        // Apply filter
        if (opts.filter_status && task.status != *opts.filter_status) continue;
        if (!opts.show_completed && task.status == TaskStatus::Completed) continue;

        task_elements.push_back(
            RenderTaskLine(task, i == opts.selected_index, opts.show_description));
    }

    if (task_elements.empty()) {
        task_elements.push_back(text(" No tasks matching filter") | dim | center);
    }

    auto list = vbox(task_elements) | vscroll_indicator | yframe | flex;

    // Action bar
    auto actions = hbox({
        text(" [Enter]") | color(Color::Cyan), text(" details "),
        text("[space]") | color(Color::Cyan), text(" toggle "),
        text("[a]") | color(Color::Cyan), text("dd "),
        text("[d]") | color(Color::Cyan), text("elete "),
        text("[f]") | color(Color::Cyan), text("ilter "),
        text("[h]") | color(Color::Cyan), text("ide done "),
        text("[Esc]") | color(Color::Cyan), text(" close"),
    }) | dim;

    return vbox({
        summary_bar,
        separator(),
        list,
        separator(),
        actions,
    }) | borderRounded;
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a task view component
[[nodiscard]] inline Component TaskView(TaskViewOptions options) {
    auto state = std::make_shared<TaskViewOptions>(std::move(options));

    return Renderer([state] {
        return RenderTaskView(*state);
    }) | CatchEvent([state](Event event) -> bool {
        int count = static_cast<int>(state->tasks.size());

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

        if (event == Event::Character(' ')) {
            // Toggle status (pending -> in_progress -> completed)
            if (count > 0 && state->selected_index < count) {
                auto& task = state->tasks[state->selected_index];
                TaskStatus next;
                switch (task.status) {
                    case TaskStatus::Pending: next = TaskStatus::InProgress; break;
                    case TaskStatus::InProgress: next = TaskStatus::Completed; break;
                    case TaskStatus::Failed: next = TaskStatus::InProgress; break;
                    default: next = task.status; break;
                }
                if (state->on_status_change) {
                    state->on_status_change(task.id, next);
                }
            }
            return true;
        }
        if (event == Event::Return) {
            if (count > 0 && state->on_select) {
                state->on_select(state->tasks[state->selected_index].id);
            }
            return true;
        }
        if (event == Event::Character('a')) {
            if (state->on_add) state->on_add();
            return true;
        }
        if (event == Event::Character('d')) {
            if (count > 0 && state->on_delete) {
                state->on_delete(state->tasks[state->selected_index].id);
            }
            return true;
        }
        if (event == Event::Character('h')) {
            state->show_completed = !state->show_completed;
            return true;
        }
        if (event == Event::Character('f')) {
            // Cycle filter
            if (!state->filter_status) {
                state->filter_status = TaskStatus::InProgress;
            } else if (*state->filter_status == TaskStatus::InProgress) {
                state->filter_status = TaskStatus::Pending;
            } else if (*state->filter_status == TaskStatus::Pending) {
                state->filter_status = TaskStatus::Blocked;
            } else {
                state->filter_status = std::nullopt;
            }
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::components::task_view
