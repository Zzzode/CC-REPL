/// @file bridge_dialog.cppm
/// @brief IDE bridge connection dialog
module;
#include <string>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.bridge_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
struct BridgeStatus { std::string ide_name; bool is_connected{false}; std::optional<std::string> version; std::optional<std::string> error; };
[[nodiscard]] inline Element render_bridge_dialog(const BridgeStatus& status) {
    auto status_color = status.is_connected ? Color::Green : Color::Red;
    std::vector<Element> elements;
    elements.push_back(text("IDE Bridge") | bold);
    elements.push_back(separator());
    elements.push_back(hbox({text(status.ide_name), text(status.is_connected ? " [connected]" : " [disconnected]") | color(status_color)}));
    if (status.version) elements.push_back(text("Version: " + *status.version) | dim);
    if (status.error) elements.push_back(text("Error: " + *status.error) | color(Color::Red));
    return vbox(elements) | border;
}
} // namespace
