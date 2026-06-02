/// @file prompt_queued_commands.cppm
/// @brief Display for queued/pending commands
module;
#include <string>
#include <vector>
#include <format>
#include <ftxui/dom/elements.hpp>
export module cc.ui.prompt.prompt_queued_commands;
export namespace cc::ui::prompt {
using namespace ftxui;
struct QueuedCommand { std::string text; bool is_executing{false}; };
[[nodiscard]] inline Element render_queued_commands(const std::vector<QueuedCommand>& commands) {
    if (commands.empty()) return text("");
    std::vector<Element> elements;
    elements.push_back(text(std::format("Queued ({})", commands.size())) | dim);
    for (const auto& cmd : commands) {
        auto style = cmd.is_executing ? bold : dim;
        elements.push_back(text("  " + cmd.text) | style);
    }
    return vbox(elements);
}
} // namespace
