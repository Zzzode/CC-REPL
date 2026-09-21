/// @file task_detail_dialog.cppm
/// @brief Task detail dialog showing full task information
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.tasks.task_detail_dialog;
export namespace cc::ui::tasks {
using namespace ftxui;
struct TaskDetail { std::string id; std::string title; std::string status; std::optional<std::string> assignee; std::vector<std::string> subtasks; };
[[nodiscard]] inline Element render_task_detail(const TaskDetail& task) {
    std::vector<Element> elements;
    elements.push_back(hbox({text(task.title) | bold, text(" [" + task.status + "]") | dim}));
    if (task.assignee) elements.push_back(text("Assignee: " + *task.assignee) | dim);
    if (!task.subtasks.empty()) {
        elements.push_back(separator());
        for (const auto& st : task.subtasks) elements.push_back(text("  - " + st));
    }
    return vbox(elements) | border;
}
} // namespace
