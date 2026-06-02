/// @file remote_env_dialog.cppm
/// @brief Remote environment setup dialog
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.remote_env_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
struct RemoteEnvInfo { std::string host; std::optional<std::string> user; std::optional<uint16_t> port; bool is_connected{false}; };
[[nodiscard]] inline Element render_remote_env(const RemoteEnvInfo& info) {
    auto status = info.is_connected ? "connected" : "disconnected";
    auto c = info.is_connected ? Color::Green : Color::Red;
    std::vector<Element> elements;
    elements.push_back(text("Remote Environment") | bold);
    elements.push_back(separator());
    elements.push_back(hbox({text("Host: " + info.host), text(" [" + std::string(status) + "]") | color(c)}));
    if (info.user) elements.push_back(text("User: " + *info.user) | dim);
    if (info.port) elements.push_back(text(std::format("Port: {}", *info.port)) | dim);
    return vbox(elements) | border;
}
} // namespace
