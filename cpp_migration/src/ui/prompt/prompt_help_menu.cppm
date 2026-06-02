/// @file prompt_help_menu.cppm
/// @brief Help menu overlay showing available commands
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.prompt.prompt_help_menu;
export namespace cc::ui::prompt {
using namespace ftxui;
struct HelpMenuItem { std::string command; std::string description; };
[[nodiscard]] inline Element render_help_menu(const std::vector<HelpMenuItem>& items) {
    std::vector<Element> elements;
    elements.push_back(text("Available Commands") | bold);
    elements.push_back(separator());
    for (const auto& item : items) {
        elements.push_back(hbox({text("/" + item.command) | bold | color(Color::Cyan), text(" - " + item.description) | dim}));
    }
    return vbox(elements) | border;
}
} // namespace
