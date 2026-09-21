/// @file ide_connect_dialog.cppm
/// @brief IDE connection setup dialog
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.ide_connect_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
struct IdeOption { std::string name; std::string description; bool is_available{true}; };
[[nodiscard]] inline Element render_ide_connect(const std::vector<IdeOption>& options, std::size_t selected) {
    std::vector<Element> elements;
    elements.push_back(text("Connect to IDE") | bold);
    elements.push_back(separator());
    for (std::size_t i = 0; i < options.size(); ++i) {
        auto style = i == selected ? inverted : nothing;
        auto avail = options[i].is_available ? "" : " (not found)";
        elements.push_back(text(options[i].name + avail) | style);
    }
    return vbox(elements) | border;
}
} // namespace
