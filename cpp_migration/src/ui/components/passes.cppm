/// @file passes.cppm
/// @brief Query progress (passes) panel - shows pass index, description,
/// history, token consumption and cost during a multi-pass query loop.
/// Mirrors the TS "Passes" sidebar panel and the /passes command output.
module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.passes;

import cc.ui.design.primitives;
import cc.ui.design.tokens;
import cc.ui.design.theme;
import cc.types.types;

export namespace cc::ui::components::passes {

using namespace ftxui;
using namespace cc::ui::design::primitives;
using namespace cc::ui::design::tokens;
using namespace cc::ui::design::theme;

// ─── State ───────────────────────────────────────────────────────────────────

/// Runtime snapshot of the query pass loop.  Mirrors TS PassesViewState.
struct PassesViewState {
    int total_passes = 0;
    int current_pass = 0;
    std::string pass_name;
    std::string pass_description;
    std::vector<std::string> pass_history;
    uint64_t tokens_consumed = 0;
    double pass_cost_usd = 0.0;
    bool is_thinking = false;
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// Format a uint64_t token count with thousands separators.
[[nodiscard]] inline std::string format_tokens(uint64_t n) {
    if (n < 1000) return std::to_string(n);
    std::string s = std::to_string(n);
    std::string out;
    out.reserve(s.size() + (s.size() - 1) / 3);
    int count = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (count > 0 && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/// Clip pass history to the most recent `max` entries.
[[nodiscard]] inline std::vector<std::string> clip_history(
    const std::vector<std::string>& history, std::size_t max = 10) {
    if (history.size() <= max) return history;
    return std::vector<std::string>(history.end() - static_cast<long long>(max),
                                    history.end());
}

/// Clamp total passes into the safe [1, ...] range for division.
[[nodiscard]] inline double progress_pct(const PassesViewState& s) {
    if (s.total_passes <= 0) return 0.0;
    double ratio = static_cast<double>(std::max(s.current_pass, 0)) /
                   static_cast<double>(s.total_passes);
    return std::clamp(ratio, 0.0, 1.0);
}

/// Build the animated thinking prefix — braille spinner if enabled,
/// otherwise "⋯ " unicode midline ellipsis.
[[nodiscard]] inline Element thinking_prefix(bool is_thinking,
                                             const Theme& theme,
                                             std::uint64_t frame = 0) {
    if (!is_thinking) return text("  ");
    return hbox({
        spinner_glyph(frame, Role::Primary, theme),
        text(" "),
    });
}

// ─── Component Implementation ────────────────────────────────────────────────

class PassesPanelBase : public ftxui::ComponentBase {
public:
    PassesPanelBase(PassesViewState state, Theme theme)
        : state_(std::move(state)), theme_(std::move(theme)),
          start_(std::chrono::steady_clock::now()) {}

    Element Render() override {
        using namespace ftxui;
        const auto& s = state_;

        // ── Header badge ─────────────────────────────────────────────
        auto badge = hbox({
            text(" "),
            text("QUERY PROGRESS") | bold | underlined | inverted,
            text(" "),
        }) | bgcolor(theme_.palette->primary)
           | color(Color::White)
           | size(WIDTH, EQUAL, 30);

        auto header = hbox({ badge | xflex });

        // ── Progress gauge ───────────────────────────────────────────
        double pct = progress_pct(s);
        auto gauge_bar = gauge(pct) | color(theme_.color_for(Role::Primary));
        auto pct_text = text(std::format(" {:3.0f}%", pct * 100.0))
                        | color(theme_.palette->muted);
        auto pass_of = text(std::format("  Pass {} of {}",
                                        std::max(s.current_pass, 0),
                                        std::max(s.total_passes, 0)))
                       | color(theme_.palette->subtle) | bold;
        auto progress_row = hbox({
            gauge_bar | xflex,
            pct_text,
            pass_of,
        });

        // ── Current pass card ────────────────────────────────────────
        auto frame = frame_counter();
        auto thinking = thinking_prefix(s.is_thinking, theme_, frame);
        std::string name = s.pass_name.empty() ? "(unnamed pass)" : s.pass_name;
        auto title = text(name) | bold | color(theme_.palette->primary);
        std::string desc_text = s.pass_description.empty()
            ? std::string("No description available.")
            : s.pass_description;
        auto desc = paragraphAlignLeft(desc_text)
                    | color(theme_.palette->muted) | dim;

        auto current_box = vbox({
            text(""),
            vbox({
                hbox({ thinking, title }),
                hbox({ text("   "), desc | xflex }),
            }) | borderStyled(theme_.color_for(Role::Info)),
            text(""),
        });

        // ── Pass history ─────────────────────────────────────────────
        auto recent = clip_history(s.pass_history, 10);
        Elements history_rows;
        history_rows.reserve(recent.size());
        int idx = static_cast<int>(s.pass_history.size() > recent.size()
                                       ? s.pass_history.size() - recent.size()
                                       : 0);
        for (const auto& entry : recent) {
            ++idx;
            history_rows.push_back(hbox({
                text(" ") | size(WIDTH, EQUAL, 1),
                text("•") | color(theme_.palette->subtle),
                text(" "),
                text(std::format("[{:>3}]", idx)) | color(theme_.palette->muted) | dim,
                text(" "),
                text(entry) | color(theme_.palette->subtle) | dim,
            }) | xflex);
        }
        if (history_rows.empty()) {
            history_rows.push_back(
                hbox({ text("  (no pass history yet)") | dim
                                              | color(theme_.palette->muted) })
            );
        }

        auto history_title = hbox({
            text(" PASS HISTORY ") | bold | color(theme_.palette->subtle),
        });

        auto history_box = vbox({
            text(""),
            vbox({
                history_title,
                separator() | color(theme_.palette->subtle),
                vbox(std::move(history_rows)) | yflex,
            }),
            text(""),
        });

        // ── Footer (tokens + cost right-aligned) ─────────────────────
        auto tokens_str = format_tokens(s.tokens_consumed);
        auto cost_str = std::format("${:.4f}", s.pass_cost_usd);
        auto footer_right = hbox({
            text("T " + tokens_str) | dim | color(theme_.palette->muted),
            text("  "),
            text(cost_str) | dim | color(theme_.palette->muted),
        });

        auto footer = hbox({
            filler(),
            footer_right,
        });

        // ── Assemble ─────────────────────────────────────────────────
        return vbox({
            header,
            separatorEmpty(),
            progress_row,
            separatorEmpty(),
            current_box,
            separatorEmpty(),
            history_box,
            separator(),
            footer,
        }) | borderRounded | color(theme_.palette->chrome);
    }

    /// Replace the stored state snapshot in-place.
    void update_state(PassesViewState s) { state_ = std::move(s); }

private:
    /// Compute a spinner frame counter from wall-clock time.
    [[nodiscard]] std::uint64_t frame_counter() const {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - start_).count();
        return static_cast<std::uint64_t>(ms) / 80u;
    }

    PassesViewState state_;
    Theme theme_;
    std::chrono::steady_clock::time_point start_;
};

// ─── Public Constructors ─────────────────────────────────────────────────────

/// Build the passes panel from a runtime snapshot using the current theme.
[[nodiscard]] inline auto BuildPassesPanel(const PassesViewState& s)
    -> ftxui::Component {
    Theme theme = current_theme();
    return ftxui::Make<PassesPanelBase>(s, std::move(theme));
}

/// Theme-explicit overload — useful in tests and design previews that do
/// not rely on the thread-local theme provider.
[[nodiscard]] inline auto BuildPassesPanel(const PassesViewState& s,
                                           const Theme& theme)
    -> ftxui::Component {
    return ftxui::Make<PassesPanelBase>(s, theme);
}

} // namespace cc::ui::components::passes
