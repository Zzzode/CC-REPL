/// @file usage_dialog.cppm
/// @brief Standalone API usage viewer — total cost, token counts, per-model
/// breakdown, and progress gauges for daily / rate limits.
/// Migrated from Settings/Usage.tsx. Data is supplied by the caller via the
/// options struct (freshly computed from AppState / cost tracker).
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.usage_dialog;

import cc.types.types;

export namespace cc::ui::dialogs::usage_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Per-model usage row
struct ModelUsageRow {
    std::string model_name;
    std::uint64_t requests = 0;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    double cost_usd = 0.0;
};

/// Full usage snapshot passed into the dialog
struct UsageSnapshot {
    double total_cost_usd = 0.0;
    std::uint64_t total_input_tokens = 0;
    std::uint64_t total_output_tokens = 0;
    std::uint64_t total_requests = 0;

    // Limit utilisation (0..1)
    double daily_budget_pct = 0.0;
    double rate_limit_pct = 0.0;

    // Optional resets-at timestamps
    std::optional<std::chrono::system_clock::time_point> daily_resets_at;
    std::optional<std::chrono::system_clock::time_point> rate_resets_at;

    // Per-model breakdown
    std::vector<ModelUsageRow> per_model;
};

/// Props for the standalone usage dialog
struct UsageDialogOptions {
    std::function<void()> on_close;
    /// Called on [r] to repopulate the snapshot
    std::function<void(UsageSnapshot&)> refresh;
    UsageSnapshot initial;
};

// ============================================================
// Helpers
// ============================================================

[[nodiscard]] inline std::string format_reset_in(
    std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto diff = tp - now;
    if (diff <= system_clock::duration::zero()) return "now";
    auto secs = duration_cast<seconds>(diff).count();
    if (secs < 60) return std::format("{}s", secs);
    auto mins = secs / 60;
    secs %= 60;
    if (mins < 60) return std::format("{}m{}s", mins, secs);
    auto hours = mins / 60;
    mins %= 60;
    return std::format("{}h{}m", hours, mins);
}

[[nodiscard]] inline Element Card(
    const std::string& label,
    const std::string& value,
    Color accent = Color::White) {
    return vbox({
        hbox({text(" "), text(label) | dim, filler()}),
        separator(),
        hbox({text(" "), text(value) | bold | color(accent), filler()}),
    }) | border | size(WIDTH, GREATER_THAN, 22);
}

[[nodiscard]] inline Element LimitBar(
    const std::string& title, double pct,
    Color fill,
    const std::optional<std::chrono::system_clock::time_point>& resets = std::nullopt) {
    pct = std::clamp(pct, 0.0, 1.0);
    auto label_el = hbox({
        text(" " + title + " ") | bold | size(WIDTH, EQUAL, 18),
        gauge(pct) | color(fill) | size(WIDTH, EQUAL, 40),
        text(std::format(" {}%", static_cast<int>(pct * 100)))
            | color(Color::White) | dim,
    });
    Elements els;
    els.push_back(label_el);
    if (resets) {
        els.push_back(hbox({
            text("   "),
            text("resets in " + format_reset_in(*resets))
                | dim | color(Color::GrayLight),
        }));
    }
    return vbox(els);
}

// ============================================================
// Rendering
// ============================================================

[[nodiscard]] inline Element RenderUsageDialog(
    const UsageSnapshot& snap,
    const std::string& toast) {

    // ----- Summary cards -----
    Elements cards = {
        Card("Total Cost",
             std::format("${:.4f}", snap.total_cost_usd),
             Color::Green),
        Card("Input Tokens",
             std::format("{}", snap.total_input_tokens),
             Color::Cyan),
        Card("Output Tokens",
             std::format("{}", snap.total_output_tokens),
             Color::Cyan),
        Card("Requests",
             std::format("{}", snap.total_requests),
             Color::Yellow),
    };

    // ----- Limit bars -----
    auto daily_fill = snap.daily_budget_pct > 0.9 ? Color::Red
                     : snap.daily_budget_pct > 0.7 ? Color::Yellow
                                                    : Color::Green;
    auto rate_fill  = snap.rate_limit_pct > 0.9 ? Color::Red
                     : snap.rate_limit_pct > 0.7 ? Color::Yellow
                                                  : Color::YellowLight;

    Elements body;
    body.push_back(hbox({
        text(" API Usage ") | bold | color(Color::Cyan),
        filler(),
        text(" [r]efresh  [Esc]close ") | dim,
    }));
    body.push_back(separator());
    body.push_back(hbox(std::move(cards)));
    body.push_back(text(""));
    body.push_back(text(" Limits") | bold);
    body.push_back(separator());
    body.push_back(LimitBar("Daily budget", snap.daily_budget_pct,
                            daily_fill, snap.daily_resets_at));
    body.push_back(text(""));
    body.push_back(LimitBar("Rate limit",  snap.rate_limit_pct,
                            rate_fill, snap.rate_resets_at));
    body.push_back(text(""));

    // ----- Per-model table -----
    body.push_back(text(" Per-model breakdown") | bold);
    body.push_back(separator());
    if (snap.per_model.empty()) {
        body.push_back(text("  (no requests recorded yet)") | dim);
    } else {
        // Header
        body.push_back(hbox({
            text(" Model") | bold | dim | size(WIDTH, EQUAL, 32),
            text(" Requests") | bold | dim | size(WIDTH, EQUAL, 12),
            text(" In") | bold | dim | size(WIDTH, EQUAL, 12),
            text(" Out") | bold | dim | size(WIDTH, EQUAL, 12),
            text(" Cost") | bold | dim | size(WIDTH, EQUAL, 14),
        }));
        body.push_back(separator());

        std::uint64_t req_total = 0;
        std::uint64_t in_total = 0;
        std::uint64_t out_total = 0;
        double cost_total = 0.0;
        for (const auto& r : snap.per_model) {
            req_total += r.requests;
            in_total  += r.input_tokens;
            out_total += r.output_tokens;
            cost_total += r.cost_usd;
            body.push_back(hbox({
                text(" " + r.model_name)
                    | color(Color::White) | size(WIDTH, EQUAL, 32),
                text(std::format(" {}", r.requests))
                    | color(Color::Yellow) | size(WIDTH, EQUAL, 12),
                text(std::format(" {}", r.input_tokens))
                    | color(Color::Cyan) | size(WIDTH, EQUAL, 12),
                text(std::format(" {}", r.output_tokens))
                    | color(Color::Cyan) | size(WIDTH, EQUAL, 12),
                text(std::format(" ${:.4f}", r.cost_usd))
                    | color(Color::Green) | size(WIDTH, EQUAL, 14),
            }));
        }
        // Totals row
        body.push_back(separator());
        body.push_back(hbox({
            text(" TOTAL") | bold | dim | size(WIDTH, EQUAL, 32),
            text(std::format(" {}", req_total)) | bold
                | color(Color::Yellow) | size(WIDTH, EQUAL, 12),
            text(std::format(" {}", in_total)) | bold
                | color(Color::Cyan) | size(WIDTH, EQUAL, 12),
            text(std::format(" {}", out_total)) | bold
                | color(Color::Cyan) | size(WIDTH, EQUAL, 12),
            text(std::format(" ${:.4f}", cost_total)) | bold
                | color(Color::Green) | size(WIDTH, EQUAL, 14),
        }));
    }

    if (!toast.empty()) {
        body.push_back(text(""));
        body.push_back(hbox({text(" "), text(toast) | color(Color::Green)}));
    }

    return window(
        text(" Usage ") | bold | color(Color::Cyan),
        vbox(body) | yframe
    ) | color(Color::Cyan);
}

// ============================================================
// Interactive component
// ============================================================

/// Build a standalone usage dialog.
[[nodiscard]] inline Component UsageDialog(UsageDialogOptions opts) {
    struct State {
        UsageDialogOptions opts;
        UsageSnapshot snap;
        std::string toast;
        std::chrono::steady_clock::time_point toast_until;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(opts);
    state->snap = state->opts.initial;

    auto show_toast = [state](std::string msg) {
        state->toast = std::move(msg);
        state->toast_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds(3);
    };

    return Renderer([state] {
        std::string visible_toast;
        if (std::chrono::steady_clock::now() < state->toast_until) {
            visible_toast = state->toast;
        }
        return RenderUsageDialog(state->snap, visible_toast);
    }) | CatchEvent([state, show_toast](Event event) -> bool {
        if (event == Event::Escape) {
            if (state->opts.on_close) state->opts.on_close();
            return true;
        }
        if (event == Event::Character('r')) {
            if (state->opts.refresh) {
                state->opts.refresh(state->snap);
            }
            show_toast("↻ Usage data refreshed");
            return true;
        }
        // q also closes (terminal app convention)
        if (event == Event::Character('q')) {
            if (state->opts.on_close) state->opts.on_close();
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::dialogs::usage_dialog
