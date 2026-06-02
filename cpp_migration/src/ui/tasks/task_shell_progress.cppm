/// @file task_shell_progress.cppm
/// @brief Shell task progress rendering with output streaming
module;
#include <string>
#include <optional>
#include <format>
#include <ftxui/dom/elements.hpp>
export module cc.ui.tasks.task_shell_progress;
export namespace cc::ui::tasks {
using namespace ftxui;
struct ShellTaskProgress { std::string command; std::optional<std::string> last_output_line; bool is_running{true}; std::optional<int> exit_code; };
[[nodiscard]] inline Element render_shell_progress(const ShellTaskProgress& task) {
    std::vector<Element> elements;
    auto status_color = task.is_running ? Color::Yellow : (task.exit_code.value_or(1) == 0 ? Color::Green : Color::Red);
    elements.push_back(hbox({text("$ ") | dim, text(task.command) | color(status_color)}));
    if (task.last_output_line) elements.push_back(text(*task.last_output_line) | dim);
    if (task.exit_code) elements.push_back(text(std::format("exit: {}", *task.exit_code)) | dim);
    return vbox(elements);
}
} // namespace
