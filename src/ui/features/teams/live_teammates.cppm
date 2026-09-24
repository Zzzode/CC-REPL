/// @file live_teammates.cppm
/// @brief Live leader-side teammates strip + TeamsView modal body.
///
/// Faithful port of the TS live teams surface:
///   - src/components/teams/CoordinatorAgentStatus.tsx AgentLine
///     (per-teammate live status dot + output tail, pinned above the prompt)
///   - src/components/teams/TeamStatus.tsx footer count (the "N teams" pill
///     stays owned by prompt_input_footer; this module only renders rows)
///   - src/components/teams/TeamsDialog.tsx TeamDetailView (roster modal)
///
/// All state is state-owned by ReplScreenState (live_teammates vector +
/// teams_overview_selected_index). The functions here are pure Element
/// builders: they read their arguments, retain nothing, and may only touch
/// palette tokens plus the per-teammate identity color resolved through the
/// same agent_color_manager + shared resolver agent_cards uses. No RGB
/// literals are introduced.
module;

#include <cstddef>
#include <cstdint>

#include <ftxui/dom/elements.hpp>

export module cc.ui.features.teams.live_teammates;

import std;

import cc.ui.foundation.theme_provider;
import cc.ui.features.agents.agent_shared_widgets;
import cc.tools.agent_color_manager;

export namespace cc::ui::teams::live {

using namespace ftxui;

/// Live projection of one teammate (leader view). 1:1 with the task brief's
/// {agent_id,name,color,status,last_output_tail,pane_id}.
struct LiveTeammate {
    std::string agent_id;
    std::string name;
    std::string color;                 // agent color NAME (e.g. "cyan"), resolved at render time
    std::string status = "unknown";    // "running" | "idle" | "unknown"
    std::string last_output_tail;      // single-line tail of pane/transcript output
    std::string pane_id;               // tmux pane id; empty for in-process teammates
};

namespace live_detail {

namespace thm = cc::ui::design::theme;

/// Palette-only status token: running => success, idle => muted, anything
/// else (unknown / a future awaiting-approval token) => warning.
[[nodiscard]] inline const ftxui::Color& status_palette_color(
    std::string_view status) {
    const auto& pal = *thm::current_theme().palette;
    if (status == "running") return pal.success;
    if (status == "idle") return pal.muted;
    return pal.warning;
}

}  // namespace live_detail

/// Public status color helper (palette tokens only).
[[nodiscard]] inline Color status_color(std::string_view status) {
    return live_detail::status_palette_color(status);
}

/// Per-teammate identity color. The ONLY non-palette color allowed on a
/// teammate row, and even it goes through the existing name parser + shared
/// resolver (same path agent_cards.cppm uses); unparseable names fall back to
/// the palette text token.
[[nodiscard]] inline Color identity_color(const LiveTeammate& t) {
    auto parsed = cc::tools::agent_color_manager::parse_color_name(t.color);
    if (parsed) return cc::ui::agents::shared::agent_color_to_ftxui(*parsed);
    return cc::ui::design::theme::current_theme().palette->text;
}

/// Collapse pane/transcript text to one trimmed line and hard-truncate to
/// max_cols-1 with a trailing ellipsis.
[[nodiscard]] inline std::string one_line_tail(
    std::string_view raw, std::size_t max_cols = 160) {
    std::string out;
    out.reserve(raw.size());
    bool last_was_space = false;
    for (char ch : raw) {
        if (ch == '\r' || ch == '\n') ch = ' ';
        if (ch == ' ') {
            if (last_was_space) continue;
            last_was_space = true;
        } else {
            last_was_space = false;
        }
        out.push_back(ch);
    }

    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(' ');
    out = out.substr(first, last - first + 1);

    if (max_cols <= 1 || out.size() <= max_cols) return out;
    // Byte-truncate, then back off any UTF-8 continuation bytes (10xxxxxx) so
    // we never emit half a multibyte sequence before the ellipsis.
    std::size_t cut = max_cols - 1;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    out.resize(cut);
    out += "…";
    return out;
}

/// One CoordinatorAgentStatus AgentLine row:
///   ● @name status  output-tail
/// Pure render of state-owned data.
[[nodiscard]] inline Element RenderLiveTeammateRow(
    const LiveTeammate& t, int avail_cols) {
    const auto& pal = *cc::ui::design::theme::current_theme().palette;
    const int tail_cols =
        std::max(8, avail_cols - static_cast<int>(t.name.size()) - 10);
    return hbox({
        text("● ") | color(status_color(t.status)),
        text("@" + t.name) | bold | color(identity_color(t)),
        text(" " + t.status) | color(status_color(t.status)) | dim,
        text("  " + one_line_tail(t.last_output_tail,
                                  static_cast<std::size_t>(tail_cols)))
            | color(pal.subtle),
        filler(),
    });
}

/// Pinned chrome strip: one status + output-tail row per teammate. Returns an
/// empty element when there are no teammates (so callers can skip it).
[[nodiscard]] inline Element RenderLiveTeammateStrip(
    const std::vector<LiveTeammate>& ts, int term_cols = 120) {
    if (ts.empty()) return text("");
    Elements rows;
    rows.reserve(ts.size());
    for (const auto& t : ts) {
        rows.push_back(RenderLiveTeammateRow(t, term_cols));
    }
    return vbox(std::move(rows));
}

/// TeamsView modal body (TS TeamsDialog.tsx TeamDetailView). Selection index
/// lives in the queue-owned payload; it is clamped here for rendering only.
/// The borrowed state pointer never escapes this call.
[[nodiscard]] inline Element RenderTeamsOverview(
    std::string_view team_name,
    const std::vector<LiveTeammate>& ts,
    int selected_index,
    int term_cols = 120,
    int term_rows = 40) {
    const auto& pal = *cc::ui::design::theme::current_theme().palette;

    Elements body;
    if (ts.empty()) {
        body.push_back(text(" No teammates") | dim);
    } else {
        const int clamped = std::clamp(
            selected_index, 0, static_cast<int>(ts.size()) - 1);
        body.reserve(ts.size());
        for (std::size_t i = 0; i < ts.size(); ++i) {
            const auto& t = ts[i];
            const bool selected = static_cast<int>(i) == clamped;
            Elements row;
            row.reserve(5);
            row.push_back(
                text(selected ? "❯ " : "  ")
                | color(selected ? pal.text : pal.muted));
            row.push_back(
                text("@" + t.name)
                | color(identity_color(t))
                | (selected ? bold : nothing));
            if (t.status == "idle") {
                row.push_back(text(" [idle]") | dim | color(pal.muted));
            } else {
                row.push_back(text(""));
            }
            row.push_back(text("  " + t.status)
                          | color(status_color(t.status)) | dim);
            row.push_back(
                text("  " + one_line_tail(
                        t.last_output_tail,
                        static_cast<std::size_t>(
                            std::max(16, term_cols - 24))))
                | dim);
            body.push_back(hbox(std::move(row)));
        }
    }

    const std::string subtitle =
        std::to_string(ts.size()) + (ts.size() == 1 ? " teammate" : " teammates");

    Element content = vbox({
        hbox({
            text("Team " + std::string(team_name)) | bold | color(pal.primary),
            filler(),
            text(subtitle) | dim,
        }),
        separator(),
        vbox(std::move(body)),
        separator(),
        text(" ↑/↓ select · Enter view · Esc close") | dim,
    });
    return window(text(""), std::move(content))
        | size(HEIGHT, LESS_THAN, std::max(6, term_rows - 4));
}

}  // namespace cc::ui::teams::live
