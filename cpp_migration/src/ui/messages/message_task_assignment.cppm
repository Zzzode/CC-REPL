/// @file message_task_assignment.cppm
/// @brief Task assignment message rendering for multi-agent
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_task_assignment;

export namespace cc::ui::messages {

using namespace ftxui;

/// Task assignment status
enum class TaskAssignmentStatus {
    Assigned,
    InProgress,
    Completed,
    Failed,
    Cancelled,
};

/// Task assignment display options
struct TaskAssignmentOptions {
    std::string task_id;
    std::string description;
    std::string assignee;
    TaskAssignmentStatus status{TaskAssignmentStatus::Assigned};
    std::optional<std::string> agent_color;
};

/// Render task assignment message
[[nodiscard]] inline Element render_task_assignment(const TaskAssignmentOptions& opts) {
    auto status_text = [&]() -> std::pair<std::string, Color> {
        switch (opts.status) {
            case TaskAssignmentStatus::Assigned: return {"ASSIGNED", Color::Cyan};
            case TaskAssignmentStatus::InProgress: return {"IN PROGRESS", Color::Yellow};
            case TaskAssignmentStatus::Completed: return {"DONE", Color::Green};
            case TaskAssignmentStatus::Failed: return {"FAILED", Color::Red};
            case TaskAssignmentStatus::Cancelled: return {"CANCELLED", Color::GrayDark};
        }
        return {"UNKNOWN", Color::White};
    }();

    return hbox({
        text("[" + status_text.first + "]") | color(status_text.second) | bold,
        text(" "),
        text(opts.assignee) | bold,
        text(": "),
        text(opts.description),
    });
}

} // namespace cc::ui::messages
