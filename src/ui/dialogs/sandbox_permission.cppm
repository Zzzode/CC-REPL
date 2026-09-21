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
///
/// SFINAE-gated because 3 payloads share this module:
///   ToolPermissionPayload        → .origin      + .initial_sandbox_toggle
///   SandboxPermissionPayload     → .host_pattern (== .origin) + always_bool toggle
///   WorkerSandboxPermissionPayload → .description + on_response only (stub chrome)
[[nodiscard]] inline Element RenderDefault(
    const auto& p, const auto& /*ctx*/) {
    // ToolPermissionPayload: .origin + .initial_sandbox_toggle
    if constexpr (requires { p.origin; p.initial_sandbox_toggle; }) {
        return RenderDefault(std::string_view{p.origin}, p.initial_sandbox_toggle);
    }
    // SandboxPermissionPayload: .host_pattern == origin; no sandbox toggle in
    // bottom variant → sandbox_toggle param is always false for bottom slot.
    else if constexpr (requires { p.host_pattern; }) {
        std::string_view origin{p.host_pattern};
        bool managed_only = false;
        if constexpr (requires { p.managed_domains_only; }) {
            managed_only = p.managed_domains_only.value_or(false);
        }
        auto body = vbox({
            text("Network access requested from:"),
            text(std::string{origin}) | bold | color(Color::Cyan),
            text(""),
            hbox({
                text(" [y] Allow once ") | color(Color::Green),
                managed_only ? text("") :
                    text(" [a] Always allow ") | color(Color::Cyan),
                text(" [n] Deny ") | color(Color::Red),
                text(" [Esc] Cancel") | color(Color::GrayDark),
            }),
        });
        return window(text(" Sandbox Permission ") | color(Color::Yellow), body);
    }
    // WorkerSandboxPermissionPayload / unknown: minimal chrome
    else {
        std::string_view origin{"<unknown>"};
        if constexpr (requires { p.tool_name; }) origin = std::string_view{p.tool_name};
        else if constexpr (requires { p.description; }) origin = std::string_view{p.description};
        auto body = vbox({
            text("Worker requesting network access:"),
            text(std::string{origin}) | bold | color(Color::Cyan),
            text(""),
            hbox({
                text(" [y] Allow ") | color(Color::Green),
                text(" [a] Always allow ") | color(Color::Cyan),
                text(" [n] Deny ") | color(Color::Red),
                text(" [Esc] Cancel") | color(Color::GrayDark),
            }),
        });
        return window(text(" Sandbox Permission ") | color(Color::Yellow), body);
    }
}

/// Event handler — y/a/n/s + Esc/Enter shortcuts.
///
/// The overlay (ToolPermissionPayload) and bottom (SandboxPermissionPayload /
/// WorkerSandboxPermissionPayload) dialogs share this template, but their
/// payload shapes differ substantially:
///   * ToolPermissionPayload     — Decision enum on_response + can_always_allow
///                                 + initial_sandbox_toggle + on_abort
///   * SandboxPermissionPayload  — host_pattern (== origin) + on_response(allow,always)
///                                 + focused_index + managed_domains_only + on_dismiss
///   * WorkerSandboxPermissionPayload — on_response(allow,always) only
/// We branch via SFINAE (requires-clause) so the reader sees every shape
/// explicitly; no unbound member names compile anywhere.

// ── Shape 1: ToolPermissionPayload (overlay) ────────────────────────────────
template <typename PayloadT>
inline bool HandleSandboxPermissionEvent(PayloadT& p, const Event& event)
requires requires (PayloadT& x) {
    x.on_response; x.can_always_allow; x.initial_sandbox_toggle; x.on_abort;
}
{
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

// ── Shape 2: SandboxPermissionPayload / WorkerSandboxPermissionPayload
//       (bottom slot — host_pattern+on_response(allow,always) variant) ────────
template <typename PayloadT>
inline bool HandleSandboxPermissionEvent(PayloadT& p, const Event& event)
requires requires (PayloadT& x) {
    x.host_pattern; x.on_response;
} && (!requires (PayloadT& x) { x.initial_sandbox_toggle; })
{
    // ── Focus navigation (ArrowUp/Down, j/k) ──────────────────────────────
    // TS parity: the bottom-slot SandboxPermission dialog has selectable
    // options (Yes / YesAlways / No) navigable via arrow keys and Vim
    // hjkl.  focused_index tracks which option is highlighted; Enter
    // confirms the focused option.
    auto option_count = [&]() -> std::int8_t {
        bool managed_only = false;
        if constexpr (requires { p.managed_domains_only; }) {
            managed_only = p.managed_domains_only.value_or(false);
        }
        // managed_only → 2 options (Yes, No); else → 3 (Yes, YesAlways, No)
        return managed_only ? 2 : 3;
    };

    auto move_focus = [&](int delta) -> bool {
        if constexpr (!requires { p.focused_index; }) return false;
        const std::int8_t n = option_count();
        if (n <= 0) return false;
        std::int8_t cur = p.focused_index.value_or(0);
        // Wrap: (cur + delta + n) % n — handles both forward and backward
        int next = (static_cast<int>(cur) + delta + n) % n;
        p.focused_index = static_cast<std::int8_t>(next);
        return true;
    };

    if (event == Event::ArrowDown || event == Event::Character('j')) {
        return move_focus(+1);
    }
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        return move_focus(-1);
    }

    // ── Confirmation keys ─────────────────────────────────────────────────
    // canonical field access helpers — host_pattern → origin alias for renderer
    if (event == Event::Character('y') || event == Event::Character('Y') ||
        event == Event::Return) {
        if (p.on_response) p.on_response(/*allow=*/true, /*always=*/false);
        return true;
    }
    if (event == Event::Character('a') || event == Event::Character('A')) {
        bool managed_only = false;
        if constexpr (requires { p.managed_domains_only; }) {
            managed_only = p.managed_domains_only.value_or(false);
        }
        if (!managed_only && p.on_response) {
            p.on_response(/*allow=*/true, /*always=*/true);
        }
        return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (p.on_response) p.on_response(/*allow=*/false, /*always=*/false);
        return true;
    }
    if (event == Event::Escape) {
        if constexpr (requires { p.on_dismiss; }) {
            if (p.on_dismiss) p.on_dismiss();
        }
        if (p.on_response) p.on_response(/*allow=*/false, /*always=*/false);
        return true;
    }
    return false;
}

// ── Shape 3: Any remaining payload — no-op fallback
//       (keeps generic code paths instantiable without link errors) ──────────
template <typename PayloadT>
inline bool HandleSandboxPermissionEvent(PayloadT& /*p*/, const Event& /*event*/)
requires (!requires (PayloadT& x) { x.on_response; })
{
    return false;
}

} // namespace cc::ui::dialogs::sandbox_permission
