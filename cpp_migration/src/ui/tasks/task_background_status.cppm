/// @file task_background_status.cppm
/// @brief Background task status indicator
module;
#include <string>
#include <vector>
#include <cstdint>
#include <format>
#include <ftxui/dom/elements.hpp>
export module cc.ui.tasks.task_background_status;
export namespace cc::ui::tasks {
using namespace ftxui;
struct BackgroundTaskStatus { std::string name; uint32_t progress_pct{0}; bool is_error{false}; };
[[nodiscard]] inline Element render_background_status(const std::vector<BackgroundTaskStatus>& tasks) {
    if (tasks.empty()) return text("");
    std::vector<Element> elements;
    for (const auto& t : tasks) {
        auto c = t.is_error ? Color::Red : Color::Cyan;
        elements.push_back(hbox({text(t.name) | color(c), text(std::format(" {}%", t.progress_pct)) | dim}));
    }
    return vbox(elements);
}
} // namespace
