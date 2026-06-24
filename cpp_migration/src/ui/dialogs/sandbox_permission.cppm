/// @file sandbox_permission.cppm
/// @brief Faithful SandboxPermission dialog renderer.
///
/// The SandboxPermission dialog prompts for network sandbox approval.
/// This module provides RenderDefault / HandleSandboxPermissionEvent
/// that default_renderers delegates to.
module;
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.sandbox_permission;

export namespace cc::ui::dialogs::sandbox_permission {

using namespace ftxui;

/// Simple inline rendering used when the caller has not supplied a
/// custom SandboxPermission renderer.  Keeps default_renderers lean.
[[nodiscard]] inline Element RenderDefault(
    std::string_view origin,
    bool sandbox_toggle) {
    auto body = vbox({
        text("Network access requested from:"),
        text(std::string{origin}) | bold | color(Color::Cyan),
        text(""),
        hbox({
            text(sandbox_toggle ? "[x] " : "[  ] "),
            text("Run in isolated sandbox"),
        }),
        text(""),
        hbox({
            text(" [y] Allow once ") | color(Color::Green),
            text(" [a] Always allow ") | color(Color::Cyan),
            text(" [n] Deny ") | color(Color::Red),
            text(" [s] Toggle sandbox ") | color(Color::Yellow),
            text(" [Esc] Abort") | color(Color::GrayDark),
        }),
    });
    return window(text(" Sandbox Permission ") | color(Color::Yellow), body);
}

/// Overload matching the default_renderers delegate signature.
[[nodiscard]] inline Element RenderDefault(
    const auto& p, const auto& /*ctx*/) {
    return RenderDefault(p.origin, p.initial_sandbox_toggle);
}

/// Event handler — y/a/n/s + Esc/Enter shortcuts.  Mutates
/// `p.initial_sandbox_toggle` when 's' is pressed.
template <typename PayloadT>
inline bool HandleSandboxPermissionEvent(PayloadT& p, const Event& event) {
    if (event == Event::Character('y') || event == Event::Character('Y') ||
        event == Event::Return) {
        if (p.on_response) p.on_response(/*Decision::AllowOnce*/ 0, p.initial_sandbox_toggle);
        return true;
    }
    if (event == Event::Character('a') || event == Event::Character('A')) {
        if (p.can_always_allow && p.on_response) {
            p.on_response(/*Decision::AlwaysAllow*/ 2, p.initial_sandbox_toggle);
        }
        return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (p.on_response) p.on_response(/*Decision::Deny*/ 1, p.initial_sandbox_toggle);
        return true;
    }
    if (event == Event::Character('s') || event == Event::Character('S')) {
        p.initial_sandbox_toggle = !p.initial_sandbox_toggle;
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_abort) p.on_abort();
        if (p.on_response) p.on_response(/*Decision::Abort*/ 3, p.initial_sandbox_toggle);
        return true;
    }
    return false;
}

} // namespace cc::ui::dialogs::sandbox_permission
