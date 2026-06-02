/// @file task_remote_session.cppm
/// @brief Remote session task status display
module;
#include <string>
#include <optional>
#include <chrono>
#include <ftxui/dom/elements.hpp>
export module cc.ui.tasks.task_remote_session;
export namespace cc::ui::tasks {
using namespace ftxui;
struct RemoteSessionInfo { std::string session_id; std::string host; bool is_connected{false}; std::optional<std::string> agent_name; };
[[nodiscard]] inline Element render_remote_session(const RemoteSessionInfo& info) {
    auto status_color = info.is_connected ? Color::Green : Color::Red;
    auto status_text = info.is_connected ? "connected" : "disconnected";
    std::vector<Element> elements;
    elements.push_back(hbox({text(info.host) | bold, text(" [" + std::string(status_text) + "]") | color(status_color)}));
    if (info.agent_name) elements.push_back(text("Agent: " + *info.agent_name) | dim);
    return vbox(elements);
}
} // namespace
