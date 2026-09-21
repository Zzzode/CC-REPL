/// @file settings_status_page.cppm
/// @brief Status tab content — subsystem health (API / MCP / LSP / Bridge) with
/// traffic-light markers, last-check timestamps and messages.
/// Migrated from Settings/Status.tsx diagnostics rendering.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <chrono>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.settings_status_page;

import cc.types.types;

export namespace cc::ui::dialogs::settings_status_page {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Subsystem health state
enum class HealthState : std::uint8_t {
    OK,
    Degraded,
    Down,
};

/// A single row describing one subsystem
struct SubsystemHealth {
    std::string name;
    HealthState state;
    std::chrono::system_clock::time_point last_check;
    std::string message;
};

/// Props for the status page
struct StatusPageOptions {
    std::vector<SubsystemHealth> rows;
    /// Called on [r] to refresh rows in-place
    std::function<void(std::vector<SubsystemHealth>&)> refresh;
};

// ============================================================
// Helpers
// ============================================================

[[nodiscard]] inline std::pair<std::string, Color> health_icon(HealthState s) {
    switch (s) {
        case HealthState::OK:       return {"●", Color::Green};
        case HealthState::Degraded: return {"●", Color::Yellow};
        case HealthState::Down:     return {"●", Color::Red};
    }
    return {"?", Color::White};
}

[[nodiscard]] inline std::string_view state_label(HealthState s) {
    switch (s) {
        case HealthState::OK:       return "OK";
        case HealthState::Degraded: return "Degraded";
        case HealthState::Down:     return "Down";
    }
    return "Unknown";
}

[[nodiscard]] inline std::string format_time(
    std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(tp);
    std::tm buf{};
#ifdef _WIN32
    localtime_s(&buf, &t);
#else
    localtime_r(&t, &buf);
#endif
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &buf);
    return out;
}

// ============================================================
// Rendering
// ============================================================

/// Render the status page content as a vbox of Elements.
/// The caller can embed this inside its own window / pane.
[[nodiscard]] inline Element RenderStatusPage(
    const std::vector<SubsystemHealth>& rows,
    const std::string& footer_toast = {}) {

    Elements el;
    el.push_back(hbox({
        text(" Subsystem Health") | bold | color(Color::Cyan),
        filler(),
        text(" [r]efresh ") | dim,
    }));
    el.push_back(separator());

    if (rows.empty()) {
        el.push_back(text("  Running diagnostics…") | dim);
        el.push_back(text("  (press [r] once subsystems are wired up)") | dim);
    } else {
        // Header
        el.push_back(hbox({
            text(" ") | size(WIDTH, EQUAL, 3),
            text(" Subsystem") | bold | dim | size(WIDTH, EQUAL, 20),
            text(" Status") | bold | dim | size(WIDTH, EQUAL, 12),
            text(" Last check") | bold | dim | size(WIDTH, EQUAL, 22),
            text(" Message") | bold | dim,
        }));
        el.push_back(separator());

        for (const auto& r : rows) {
            auto [icon, col] = health_icon(r.state);
            auto row_el = hbox({
                text(" " + icon + " ") | color(col) | size(WIDTH, EQUAL, 3),
                text(r.name) | bold | color(Color::White)
                    | size(WIDTH, EQUAL, 20),
                text(std::string(state_label(r.state)))
                    | color(col) | size(WIDTH, EQUAL, 12),
                text(format_time(r.last_check))
                    | dim | color(Color::GrayLight) | size(WIDTH, EQUAL, 22),
                text("  "),
                text(r.message) | dim,
            });
            if (r.state == HealthState::Down) {
                row_el = row_el | bgcolor(Color::RGB(50, 15, 15));
            } else if (r.state == HealthState::Degraded) {
                row_el = row_el | bgcolor(Color::RGB(50, 45, 10));
            }
            el.push_back(row_el);
        }
    }

    if (!footer_toast.empty()) {
        el.push_back(text(""));
        el.push_back(hbox({text(" "), text(footer_toast) | color(Color::Green)}));
    }

    return vbox(el) | yframe;
}

/// Render a standalone Status dialog (wrapped in window).
[[nodiscard]] inline Element RenderStatusDialog(
    const std::vector<SubsystemHealth>& rows,
    const std::string& toast = {}) {
    return window(
        text(" Status ") | bold | color(Color::Cyan),
        RenderStatusPage(rows, toast)
    ) | color(Color::Cyan);
}

// ============================================================
// Interactive component — standalone status page
// ============================================================

[[nodiscard]] inline Component MakeStatusPage(StatusPageOptions opts,
                                              std::function<void()> on_close) {
    struct State {
        StatusPageOptions opts;
        std::function<void()> on_close;
        std::string toast;
        std::chrono::steady_clock::time_point toast_until;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(opts);
    state->on_close = std::move(on_close);

    auto show_toast = [state](std::string m) {
        state->toast = std::move(m);
        state->toast_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds(3);
    };

    return Renderer([state] {
        std::string visible;
        if (std::chrono::steady_clock::now() < state->toast_until) {
            visible = state->toast;
        }
        return RenderStatusDialog(state->opts.rows, visible);
    }) | CatchEvent([state, show_toast](Event event) -> bool {
        if (event == Event::Escape) {
            if (state->on_close) state->on_close();
            return true;
        }
        if (event == Event::Character('r')) {
            if (state->opts.refresh) {
                state->opts.refresh(state->opts.rows);
            }
            show_toast("↻ Diagnostics refreshed");
            return true;
        }
        if (event == Event::Character('q')) {
            if (state->on_close) state->on_close();
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::dialogs::settings_status_page
