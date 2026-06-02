/// @file session_preview.cppm
/// @brief Session preview card for session list
module;
#include <format>
#include <string>
#include <optional>
#include <ftxui/dom/elements.hpp>
export module cc.ui.components.session_preview;
export namespace cc::ui::components {
using namespace ftxui;
struct SessionPreview { std::string id; std::string title; std::string timestamp; std::optional<std::string> project; uint32_t message_count{0}; };
[[nodiscard]] inline Element render_session_preview(const SessionPreview& session) {
    return vbox({
        hbox({text(session.title) | bold, text(" (" + session.timestamp + ")") | dim}),
        text(std::format("  {} messages", session.message_count)) | dim,
        session.project ? text("  Project: " + *session.project) | dim : text(""),
    });
}
} // namespace
