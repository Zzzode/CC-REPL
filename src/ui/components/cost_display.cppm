/// @file cost_display.cppm
/// @brief Cost display component - shows token usage, API costs, budget tracking,
/// and session/lifetime spending summaries.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <cmath>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.cost_display;

import cc.types.types;

export namespace cc::ui::components::cost_display {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Cost data for a single model
struct ModelCostData {
    std::string model_name;
    std::uint32_t input_tokens = 0;
    std::uint32_t output_tokens = 0;
    std::uint32_t cache_read_tokens = 0;
    std::uint32_t cache_write_tokens = 0;
    double cost_usd = 0.0;
    int request_count = 0;
};

/// Budget configuration
struct BudgetConfig {
    std::optional<double> session_limit_usd;    // Per-session budget
    std::optional<double> daily_limit_usd;      // Daily budget
    std::optional<double> monthly_limit_usd;    // Monthly budget
    double warning_threshold = 0.8;             // Warn at 80%
};

/// Aggregate cost data
struct CostData {
    // Session totals
    std::uint32_t session_input_tokens = 0;
    std::uint32_t session_output_tokens = 0;
    std::uint32_t session_cache_read = 0;
    std::uint32_t session_cache_write = 0;
    double session_cost_usd = 0.0;
    int session_requests = 0;
    std::chrono::steady_clock::time_point session_start;

    // Lifetime totals
    double lifetime_cost_usd = 0.0;
    std::uint64_t lifetime_tokens = 0;
    int lifetime_sessions = 0;

    // Per-model breakdown
    std::vector<ModelCostData> model_breakdown;

    // Budget
    BudgetConfig budget;

    // Rate info
    double avg_tokens_per_request = 0.0;
    double avg_cost_per_request = 0.0;
};

/// Display mode for cost component
enum class CostDisplayMode : std::uint8_t {
    Compact,    // Single line summary
    Detailed,   // Multi-line with breakdown
    Full,       // Full page with charts
};

/// Options for the cost display component
struct CostDisplayOptions {
    CostData data;
    CostDisplayMode mode = CostDisplayMode::Compact;
    bool show_budget = true;
    bool show_breakdown = true;
    bool animate_updates = true;
    std::function<void()> on_toggle_mode;
    std::function<void()> on_close;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Format USD cost with appropriate precision
[[nodiscard]] inline std::string format_cost(double usd) {
    if (usd < 0.001) return "$0.00";
    if (usd < 0.01) return std::format("${:.4f}", usd);
    if (usd < 1.0) return std::format("${:.3f}", usd);
    return std::format("${:.2f}", usd);
}

/// Format token count with K/M suffix
[[nodiscard]] inline std::string format_tokens(std::uint64_t tokens) {
    if (tokens < 1000) return std::to_string(tokens);
    if (tokens < 1000000) return std::format("{:.1f}K", tokens / 1000.0);
    return std::format("{:.2f}M", tokens / 1000000.0);
}

/// Get cost color based on budget utilization
[[nodiscard]] inline Color cost_color(double spent, std::optional<double> limit) {
    if (!limit) return Color::Cyan;
    double ratio = spent / *limit;
    if (ratio >= 1.0) return Color::Red;
    if (ratio >= 0.8) return Color::Yellow;
    if (ratio >= 0.5) return Color::Cyan;
    return Color::Green;
}

/// Render a mini bar chart (horizontal)
[[nodiscard]] inline Element mini_bar(double value, double max_val, Color bar_color) {
    double ratio = max_val > 0 ? std::clamp(value / max_val, 0.0, 1.0) : 0.0;
    return gauge(ratio) | color(bar_color) | size(WIDTH, EQUAL, 15);
}

// ============================================================
// Element Rendering
// ============================================================

/// Render compact cost display (single line, for status bar)
[[nodiscard]] inline Element RenderCostCompact(const CostData& data) {
    auto col = cost_color(data.session_cost_usd, data.budget.session_limit_usd);

    Elements parts = {
        text("💰 ") | dim,
        text(format_cost(data.session_cost_usd)) | color(col) | bold,
        text(" │ ") | dim,
        text(format_tokens(data.session_input_tokens)) | color(Color::Cyan) | dim,
        text("↓ ") | dim,
        text(format_tokens(data.session_output_tokens)) | color(Color::Green) | dim,
        text("↑") | dim,
    };

    if (data.budget.session_limit_usd) {
        double pct = (data.session_cost_usd / *data.budget.session_limit_usd) * 100;
        parts.push_back(text(" │ ") | dim);
        parts.push_back(text(std::format("{:.0f}%", pct))
                        | color(col) | dim);
    }

    return hbox(parts);
}

/// Render detailed cost display (multi-line)
[[nodiscard]] inline Element RenderCostDetailed(const CostData& data) {
    Elements elements;

    // Header
    elements.push_back(hbox({
        text(" 💰 Cost Summary ") | bold | color(Color::Cyan),
        filler(),
        text(std::format("{} requests", data.session_requests)) | dim,
        text(" "),
    }));
    elements.push_back(separator());

    // Token breakdown
    elements.push_back(hbox({
        text("  Input:       ") | dim,
        text(format_tokens(data.session_input_tokens)) | color(Color::Cyan),
        text(" tokens") | dim,
    }));
    elements.push_back(hbox({
        text("  Output:      ") | dim,
        text(format_tokens(data.session_output_tokens)) | color(Color::Green),
        text(" tokens") | dim,
    }));
    if (data.session_cache_read > 0) {
        elements.push_back(hbox({
            text("  Cache Read:  ") | dim,
            text(format_tokens(data.session_cache_read)) | color(Color::Yellow),
            text(" tokens") | dim,
        }));
    }
    if (data.session_cache_write > 0) {
        elements.push_back(hbox({
            text("  Cache Write: ") | dim,
            text(format_tokens(data.session_cache_write)) | color(Color::Magenta),
            text(" tokens") | dim,
        }));
    }

    elements.push_back(text(""));

    // Cost
    auto col = cost_color(data.session_cost_usd, data.budget.session_limit_usd);
    elements.push_back(hbox({
        text("  Session:     ") | dim,
        text(format_cost(data.session_cost_usd)) | color(col) | bold,
    }));
    elements.push_back(hbox({
        text("  Lifetime:    ") | dim,
        text(format_cost(data.lifetime_cost_usd)) | color(Color::GrayLight),
    }));

    // Budget bar
    if (data.budget.session_limit_usd) {
        double ratio = data.session_cost_usd / *data.budget.session_limit_usd;
        elements.push_back(text(""));
        elements.push_back(hbox({
            text("  Budget: ") | dim,
            gauge(std::clamp(ratio, 0.0, 1.0)) | color(col) | flex,
            text(std::format(" {}/{}", format_cost(data.session_cost_usd),
                format_cost(*data.budget.session_limit_usd))) | dim,
        }));
    }

    // Averages
    if (data.session_requests > 0) {
        elements.push_back(text(""));
        elements.push_back(hbox({
            text("  Avg/request: ") | dim,
            text(format_cost(data.avg_cost_per_request)) | color(Color::GrayLight),
            text(" (") | dim,
            text(format_tokens(static_cast<uint64_t>(data.avg_tokens_per_request))) | dim,
            text(" tokens)") | dim,
        }));
    }

    return vbox(elements) | borderRounded;
}

/// Render full cost display with model breakdown
[[nodiscard]] inline Element RenderCostFull(const CostData& data) {
    auto detailed = RenderCostDetailed(data);

    Elements breakdown_elements;
    breakdown_elements.push_back(text(" Model Breakdown") | bold);
    breakdown_elements.push_back(separator());

    if (data.model_breakdown.empty()) {
        breakdown_elements.push_back(text("  No model data") | dim);
    } else {
        // Find max for bar scaling
        double max_cost = 0.0;
        for (const auto& m : data.model_breakdown) {
            max_cost = std::max(max_cost, m.cost_usd);
        }

        for (const auto& model : data.model_breakdown) {
            breakdown_elements.push_back(hbox({
                text("  " + model.model_name) | bold
                    | size(WIDTH, EQUAL, 20),
                mini_bar(model.cost_usd, max_cost, Color::Cyan),
                text(" ") | dim,
                text(format_cost(model.cost_usd)) | color(Color::Cyan),
                text(std::format(" ({}×)", model.request_count)) | dim,
            }));
            breakdown_elements.push_back(hbox({
                text(std::string(22, ' ')) | dim,
                text(format_tokens(model.input_tokens) + "↓") | dim | color(Color::GrayLight),
                text(" ") | dim,
                text(format_tokens(model.output_tokens) + "↑") | dim | color(Color::GrayLight),
            }));
        }
    }

    auto breakdown = vbox(breakdown_elements) | borderRounded;

    return vbox({
        detailed,
        text(""),
        breakdown,
    });
}

/// Render the cost display based on mode
[[nodiscard]] inline Element RenderCostDisplay(const CostDisplayOptions& opts) {
    switch (opts.mode) {
        case CostDisplayMode::Compact:
            return RenderCostCompact(opts.data);
        case CostDisplayMode::Detailed:
            return RenderCostDetailed(opts.data);
        case CostDisplayMode::Full:
            return RenderCostFull(opts.data);
    }
    return text("?");
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a cost display component
[[nodiscard]] inline Component CostDisplay(CostDisplayOptions options) {
    auto state = std::make_shared<CostDisplayOptions>(std::move(options));

    return Renderer([state] {
        return RenderCostDisplay(*state);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::Character('m') || event == Event::Return) {
            // Cycle mode
            int m = static_cast<int>(state->mode);
            state->mode = static_cast<CostDisplayMode>((m + 1) % 3);
            if (state->on_toggle_mode) state->on_toggle_mode();
            return true;
        }
        if (event == Event::Escape) {
            if (state->mode != CostDisplayMode::Compact) {
                state->mode = CostDisplayMode::Compact;
                return true;
            }
            if (state->on_close) state->on_close();
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::components::cost_display
