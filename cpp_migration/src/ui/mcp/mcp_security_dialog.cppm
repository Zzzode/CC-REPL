/// @file mcp_security_dialog.cppm
/// @brief MCP-scoped security confirmation dialog with 3 severity tiers.
///
///   Info      (blue)   – First-time adding a server; list the command it
///                        will execute. User just confirms "I expect this".
///   Warning   (yellow) – Unknown URL domain / self-signed cert / missing
///                        trust record. Checkbox "Remember this domain".
///   Critical  (red)    – Cert expiry / revocation / high-risk tool set
///                        (bash, filesystem root, credentials tool).
///                        Requires explicit checkbox tick + 8-second countdown
///                        (similar UX pattern to UI8 TrustDialog's confirmation
///                        gate).  Engine delegate: cc.services.mcp.channel_*
///                        trust + cc.ui.trust_dialog trust_utils.
///
/// Content is MCP-specific (process spawn warning, unknown domain, critical
/// tool access).  Trust persistence and cert validation are NOT implemented
/// here — they live in services/mcp/channel_permissions.cppm + mcp_server_approval.cppm.
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <format>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.mcp.mcp_security_dialog;

export namespace cc::ui::mcp {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

enum class SecuritySeverity : std::uint8_t {
    Info     = 0,   // Blue
    Warning  = 1,   // Yellow
    Critical = 2,   // Red — needs checkbox + countdown
};

enum class SecurityDecision : std::uint8_t {
    Accept,
    Reject,
    Cancel,
};

/// A single risk line shown in the body (bullet with optional reason).
struct SecurityConcern {
    std::string summary;
    std::optional<std::string> detail;
    bool is_critical = false;
};

struct SecurityDialogProps {
    SecuritySeverity severity = SecuritySeverity::Info;
    std::string server_name;
    std::string server_display;   // short tag (e.g. "stdio command", "https://…")
    std::string explanation;      // one-paragraph intro

    // Info tier: commands the server will spawn (stdio).
    std::vector<std::string> spawn_commands;

    // Warning tier: domain / certificate details.
    std::optional<std::string> domain;
    std::optional<std::string> cert_summary;    // e.g. CN=…, issuer=…, notAfter=…

    // Critical tier: risk bullets.
    std::vector<SecurityConcern> concerns;

    // Checkbox defaults / visibility.
    bool show_remember_domain = false;   // Warning / Critical with domain
    bool show_explicit_ack = false;      // Critical tier
    std::chrono::seconds countdown{0};   // 0 disables; 8s for Critical

    // Engine hooks.
    std::function<void(const std::string& domain)> on_remember_domain;
    std::function<void()> on_view_certificate;

    // Completion.
    std::function<void(SecurityDecision decision, bool remember_domain)>
        on_done;
};

// ============================================================
// Color / icon helpers
// ============================================================

inline Color accent(SecuritySeverity s) {
    switch (s) {
        case SecuritySeverity::Info:     return Color::BlueLight;
        case SecuritySeverity::Warning:  return Color::Yellow;
        case SecuritySeverity::Critical: return Color::Red;
    }
    return Color::White;
}
inline std::string icon(SecuritySeverity s) {
    switch (s) {
        case SecuritySeverity::Info:     return "ℹ";
        case SecuritySeverity::Warning:  return "⚠";
        case SecuritySeverity::Critical: return "🛑";
    }
    return "?";
}
inline std::string heading(SecuritySeverity s) {
    switch (s) {
        case SecuritySeverity::Info:     return "Confirm server setup";
        case SecuritySeverity::Warning:  return "Unknown server";
        case SecuritySeverity::Critical: return "Security alert";
    }
    return "Security check";
}

// ============================================================
// Section renderers
// ============================================================

[[nodiscard]] inline Element RenderServerCard(const SecurityDialogProps& p,
                                               Color c) {
    return hbox({
        text(" Server: ") | dim,
        text(p.server_name) | bold,
        filler(),
        text(p.server_display) | color(c) | dim,
    }) | border | color(c);
}

[[nodiscard]] inline Element RenderSpawnCommands(
    const std::vector<std::string>& cmds) {
    if (cmds.empty()) return text("");
    Elements lines;
    lines.push_back(text(" This server will run the following commands:") | bold);
    for (const auto& c : cmds) {
        lines.push_back(hbox({
            text("   $ ") | color(Color::Cyan) | bold,
            text(c) | color(Color::CyanLight),
        }));
    }
    return vbox(lines);
}

[[nodiscard]] inline Element RenderDomainWarning(
    const std::optional<std::string>& domain,
    const std::optional<std::string>& cert,
    Color c) {
    Elements lines;
    if (domain) {
        lines.push_back(hbox({
            text(" Domain: ") | dim,
            text(*domain) | color(c) | bold,
            text(" is not in your trusted list.") | dim,
        }));
    }
    if (cert) {
        lines.push_back(text(""));
        lines.push_back(text(" Certificate details:") | dim);
        lines.push_back(text("   " + *cert) | dim);
    }
    lines.push_back(hbox({
        text("   [V]") | color(c) | bold,
        text("iew full certificate") | dim,
    }));
    return vbox(lines);
}

[[nodiscard]] inline Element RenderConcerns(const std::vector<SecurityConcern>& cs) {
    if (cs.empty()) return text("");
    Elements lines;
    lines.push_back(text(" The following risks have been detected:") | bold);
    for (const auto& c : cs) {
        Color bc = c.is_critical ? Color::Red : Color::Yellow;
        lines.push_back(hbox({
            text(c.is_critical ? "  !! " : "   * ") | color(bc) | bold,
            text(c.summary) | (c.is_critical ? bold : nothing),
        }));
        if (c.detail) {
            lines.push_back(paragraph("      " + *c.detail) | dim);
        }
    }
    return vbox(lines);
}

// ============================================================
// Dialog body assembly
// ============================================================

[[nodiscard]] inline Element RenderSecurityDialog(
    const SecurityDialogProps& p,
    bool remember_checked,
    bool ack_checked,
    std::chrono::seconds countdown_remaining) {

    Color c = accent(p.severity);
    std::string ico = icon(p.severity);
    std::string hdr = heading(p.severity);

    Elements body;
    body.push_back(hbox({
        text(" " + ico + " ") | color(c) | bold,
        text(hdr) | bold | color(c),
        filler(),
    }));
    body.push_back(RenderServerCard(p, c));
    body.push_back(separator());

    if (!p.explanation.empty()) {
        body.push_back(paragraph(" " + p.explanation));
        body.push_back(text(""));
    }

    if (!p.spawn_commands.empty()) {
        body.push_back(RenderSpawnCommands(p.spawn_commands));
        body.push_back(text(""));
    }

    if (p.domain || p.cert_summary) {
        body.push_back(RenderDomainWarning(p.domain, p.cert_summary, c));
        body.push_back(text(""));
    }

    if (!p.concerns.empty()) {
        body.push_back(RenderConcerns(p.concerns));
        body.push_back(text(""));
    }

    // --- Gate controls ---
    if (p.show_remember_domain) {
        body.push_back(hbox({
            text(" " + std::string(remember_checked ? "☑" : "☐") + " ")
                | color(remember_checked ? Color::Green : Color::GrayDark),
            text("Remember this domain") | (remember_checked ? bold : nothing),
        }));
    }

    if (p.show_explicit_ack) {
        body.push_back(text(""));
        body.push_back(hbox({
            text(" " + std::string(ack_checked ? "☑" : "☐") + " ")
                | color(ack_checked ? Color::RedLight : Color::GrayDark),
            text("I understand the risks and accept responsibility for "
                 "running this server.")
                | (ack_checked ? bold : color(Color::RedLight)),
        }));
    }

    if (p.countdown.count() > 0) {
        bool ready = countdown_remaining.count() <= 0;
        body.push_back(text(""));
        if (ready) {
            body.push_back(hbox({
                text(" ✓") | color(Color::Green),
                text(" Countdown complete — you may proceed.") | color(Color::Green),
            }));
        } else {
            body.push_back(hbox({
                text(" ◐ ") | color(Color::Red) | blink,
                text(std::format(" Please review for {} more second{}…",
                                 countdown_remaining.count(),
                                 countdown_remaining.count() == 1 ? "" : "s"))
                    | color(Color::Red),
            }));
        }
    }

    body.push_back(text(""));
    body.push_back(separatorLight());

    // --- Action buttons ---
    bool can_accept = true;
    if (p.show_explicit_ack && !ack_checked) can_accept = false;
    if (p.countdown.count() > 0 && countdown_remaining.count() > 0) can_accept = false;

    auto accept = hbox({
        text(" [") | dim,
        text("Enter") | bold | color(can_accept ? Color::Green : Color::GrayDark),
        text("] ") | dim,
        text("Continue") | color(can_accept ? Color::Green : Color::GrayDark)
            | (can_accept ? bold : nothing),
    });
    auto reject = hbox({
        text(" [") | dim,
        text("Esc") | bold | color(Color::RedLight),
        text("] ") | dim,
        text("Cancel") | color(Color::RedLight),
    });
    body.push_back(hbox({accept, filler(), reject}));

    // Key hints
    body.push_back(text(""));
    Elements hints;
    if (p.show_remember_domain) {
        hints.push_back(text("[M] ") | color(c));
        hints.push_back(text("remember ") | dim);
    }
    if (p.show_explicit_ack) {
        hints.push_back(text("[A] ") | color(c));
        hints.push_back(text("acknowledge ") | dim);
    }
    if (p.cert_summary) {
        hints.push_back(text("[V] ") | color(c));
        hints.push_back(text("view cert") | dim);
    }
    if (!hints.empty()) body.push_back(hbox(std::move(hints)));

    return window(
        text(" " + ico + " MCP Security ") | bold | color(c),
        vbox(std::move(body)) | xflex
    ) | color(c) | size(WIDTH, LESS_THAN, 78);
}

// ============================================================
// Interactive component
// ============================================================

/// Build an interactive MCP security dialog.  The 8-second countdown is
/// approximated using an `Enter` event counter: on each key press a tiny
/// fraction is decremented; in production builds a periodic animation
/// event (ftxui::ScreenInteractive::Loop with PostEvent) drives it instead.
/// We expose `AdvanceCountdown()` style state mutation via `CatchEvent`
/// fall-through on any Event so that parent render loops can pump ticks.
[[nodiscard]] inline Component McpSecurityDialog(SecurityDialogProps props) {
    struct State {
        SecurityDialogProps props;
        bool remember = false;
        bool ack = false;
        std::chrono::seconds remaining{0};
        std::chrono::steady_clock::time_point started_at;
        bool started = false;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);
    if (state->props.countdown.count() > 0) {
        state->remaining = state->props.countdown;
    }

    auto maybe_start_countdown = [state] {
        if (!state->started) {
            state->started = true;
            state->started_at = std::chrono::steady_clock::now();
        }
    };

    auto remaining_now = [state]() -> std::chrono::seconds {
        if (state->props.countdown.count() <= 0) return std::chrono::seconds{0};
        maybe_start_countdown();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - state->started_at);
        auto left = state->props.countdown - elapsed;
        if (left.count() < 0) left = std::chrono::seconds{0};
        state->remaining = left;
        return left;
    };

    auto emit = [state](SecurityDecision d) {
        if (state->props.on_done) {
            state->props.on_done(d, state->remember);
        }
    };

    return Renderer([state, remaining_now] {
        return RenderSecurityDialog(
            state->props,
            state->remember,
            state->ack,
            remaining_now());
    }) | CatchEvent([state, emit, remaining_now, maybe_start_countdown](Event event) -> bool {

        // Pump countdown on any event — cheap.
        remaining_now();

        // --- Checkbox toggles ---
        if (event == Event::Character('m') || event == Event::Character('M')) {
            if (state->props.show_remember_domain) {
                state->remember = !state->remember;
                if (state->remember && state->props.on_remember_domain
                    && state->props.domain) {
                    state->props.on_remember_domain(*state->props.domain);
                }
                return true;
            }
        }
        if (event == Event::Character('a') || event == Event::Character('A')) {
            if (state->props.show_explicit_ack) {
                state->ack = !state->ack;
                maybe_start_countdown();
                return true;
            }
        }

        // --- View certificate ---
        if (event == Event::Character('v') || event == Event::Character('V')) {
            if (state->props.cert_summary && state->props.on_view_certificate) {
                state->props.on_view_certificate();
                return true;
            }
        }

        // --- Accept / Reject ---
        if (event == Event::Return) {
            bool can_accept = true;
            if (state->props.show_explicit_ack && !state->ack) can_accept = false;
            if (state->props.countdown.count() > 0 &&
                remaining_now().count() > 0) can_accept = false;
            if (can_accept) {
                emit(SecurityDecision::Accept);
                return true;
            }
            return true;   // swallow — user must wait / check
        }
        if (event == Event::Escape) {
            emit(SecurityDecision::Reject);
            return true;
        }

        return false;
    });
}

/// Convenience factory — Info tier for first-time server addition.
[[nodiscard]] inline Component MakeMcpFirstAddDialog(
    std::string server_name,
    std::string command_line,
    std::function<void(SecurityDecision, bool remember)> on_done) {

    SecurityDialogProps p;
    p.severity = SecuritySeverity::Info;
    p.server_name = std::move(server_name);
    p.server_display = "local stdio";
    p.explanation = "You are adding a new MCP server for the first time. "
                    "It will run as a local process with access to files and "
                    "network on your machine.";
    p.spawn_commands.push_back(std::move(command_line));
    p.on_done = std::move(on_done);
    return McpSecurityDialog(std::move(p));
}

/// Convenience factory — Critical tier with 8s countdown + explicit ack.
[[nodiscard]] inline Component MakeMcpCriticalDialog(
    std::string server_name,
    std::string display,
    std::vector<SecurityConcern> concerns,
    std::function<void(SecurityDecision, bool remember)> on_done) {

    SecurityDialogProps p;
    p.severity = SecuritySeverity::Critical;
    p.server_name = std::move(server_name);
    p.server_display = std::move(display);
    p.explanation = "This server requires elevated permissions. Take a "
                    "moment to review before continuing.";
    p.concerns = std::move(concerns);
    p.show_explicit_ack = true;
    p.countdown = std::chrono::seconds{8};
    p.on_done = std::move(on_done);
    return McpSecurityDialog(std::move(p));
}

/// Generic factory for custom severity.
[[nodiscard]] inline Component MakeMcpSecurityDialog(SecurityDialogProps props) {
    return McpSecurityDialog(std::move(props));
}

} // namespace cc::ui::mcp
