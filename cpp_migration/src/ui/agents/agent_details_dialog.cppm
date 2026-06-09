/// @file agent_details_dialog.cppm
/// @brief Fullscreen modal agent details: 4 tabs, editable header, per-step
/// run timeline with collapsible logs, ASCII stats, 4-button footer.
///
/// Consolidates migration of:
///   - src/components/agents/AgentDetail.tsx    (~600 lines, 4-tab modal)
///   - per-run timeline + log streaming widgets inside AgentsMenu / Detail
///   - stats ASCII-bar panel (replaces React chart libs)
///
/// 4 Tabs:
///   1. Overview — description, tool chips, Permissions summary, env vars
///   2. Config   — full YAML/JSON with syntax highlight (reuses code_highlight),
///                 Edit raw / Import JSON / Export buttons
///   3. Runs     — table of recent N runs + click to open StepTimeline log view
///                 (left: step#/icon/status, right: output, >100 lines collapsed)
///   4. Stats    — run-count ASCII bar chart, avg-time gauge, cost trend list
///
/// Footer: Save / Edit raw / Delete (→ TrustDialog critical) / Close
///
/// Reuses:
///   - cc.ui.agents.shared_widgets  (AgentAvatar, StatusDot, RoleTags,
///                                    RunStatsBar, ToolChips, StepTimeline,
///                                    TimelineStep, SharedAnimState)
///   - cc.ui.agents.agent_cards     (StatusBadge)
///   - cc.ui.code_highlight         (YAML/JSON syntax highlight)
///   - ui.components.tag_tabs       (4-tab switcher)
/// NOTE: Delete button delegates to a TrustDialog (caller wires it). We
///       annotate the footer with a comment pointing at the integration.
module;

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.agents.agent_details_dialog;

import cc.ui.agents.shared_widgets;
import cc.ui.agents.agent_cards;
import cc.ui.code_highlight;
import ui.components.tag_tabs;

export namespace cc::ui::agents::details {
using namespace ftxui;

using shared::AgentAvatar;
using shared::AvatarOptions;
using shared::AgentStatus;
using shared::StatusDot;
using shared::StatusDotOptions;
using shared::RoleTags;
using shared::RunStatsBar;
using shared::RunStats;
using shared::ToolChipsElement;
using shared::ToolChipsOptions;
using shared::StepTimeline;
using shared::StepTimelineOptions;
using shared::TimelineStep;
using shared::SharedAnimState;
using shared::status_color;
using shared::status_label;
using cards::AgentCardData;
using cards::StatusBadge;
using ui::components::Tab;
using ui::components::TagTabsComponent;
using ui::components::TagTabsOptions;
using cc::ui::code_highlight::HighlightedLine;
using cc::ui::code_highlight::TokenType;

// ============================================================
// Types
// ============================================================

/// Which detail tab is active.
enum class DetailTab : std::uint8_t {
    Overview = 0,
    Config   = 1,
    Runs     = 2,
    Stats    = 3,
};

/// A single past agent run, used by Tab 3 (Runs).
struct AgentRunSummary {
    std::string run_id;
    std::string timestamp;        // ISO, shown verbatim
    std::chrono::milliseconds duration{0};
    int input_tokens = 0;
    int output_tokens = 0;
    double cost_usd = 0.0;
    AgentStatus status = AgentStatus::Idle;
    int step_count = 0;
    /// Full steps (may be > 2000; StepTimeline truncates).
    std::vector<TimelineStep> steps;
};

/// Run-count bucket used by the ASCII bar chart in Tab 4 (Stats).
struct StatsBucket {
    std::string label;   // e.g. "Mon", "Week 23"
    int count = 0;
};

/// Stats gauge data (avg time, avg cost).
struct StatsGauge {
    double value = 0.0;
    double max = 1.0;
    std::string unit;
    std::string label;
};

/// Full data payload consumed by the dialog.
struct AgentDetailsData {
    AgentCardData card;
    /// Raw agent definition serialized as JSON (for Tab 2).
    std::string raw_json;
    /// Raw YAML version (if available; otherwise falls back to JSON).
    std::optional<std::string> raw_yaml;

    std::vector<AgentRunSummary> recent_runs;
    std::vector<StatsBucket> run_count_bars;   // 7-14 buckets
    StatsGauge avg_time_gauge;
    StatsGauge avg_cost_gauge;
    std::vector<std::pair<std::string, double>> cost_trend; // label + $

    /// Currently selected run index (opened inside Runs tab).
    std::optional<int> open_run_index;
    /// Show-all flag for the StepTimeline of the opened run.
    bool show_all_steps = false;
};

/// Callbacks fired by the dialog footer / tab actions.
struct AgentDetailsCallbacks {
    std::function<void(const AgentCardData&)> on_save;
    std::function<void(const AgentCardData&)> on_edit_raw;
    /// NOTE: callers should open a TrustDialog for critical confirmation.
    /// Example: TrustDialog{.title="Delete agent " + data.card.name,
    ///                      .severity=Critical, .on_confirm=on_delete_impl}
    std::function<void(const AgentCardData&)> on_delete;
    std::function<void()> on_close;
    std::function<void(const AgentCardData&)> on_export_json;
    std::function<void(const AgentCardData&)> on_import_json;
    std::function<void(const AgentCardData&)> on_run;
};

// ============================================================
// Tab 1: Overview
// ============================================================

/// Render the Overview tab: description, tools, permissions summary, env vars.
[[nodiscard]] inline Element RenderOverviewTab(const AgentDetailsData& data) {
    const auto& c = data.card;

    // --- Description ---
    std::string desc = c.description_long.empty() ? c.description : c.description_long;
    Element desc_el;
    if (desc.empty()) {
        desc_el = text("(no description)") | dim;
    } else {
        auto lines = shared::wrap_lines(desc, 12, 90);
        Elements els;
        for (auto& ln : lines) els.push_back(text(ln));
        desc_el = vbox(std::move(els));
    }

    // --- Tools ---
    ToolChipsOptions tc;
    tc.tools = c.tools;
    tc.collapse_threshold = 12;
    tc.start_collapsed = false;
    auto tools = vbox({
        text("Enabled tools") | bold | color(Color::Magenta),
        text(""),
        ToolChipsElement(tc),
    });

    // --- Permissions summary ---
    std::string pm = c.permission_mode.value_or("Ask (default)");
    int denied_count = 0;  // simplified (AgentCardData tracks enabled only)
    Elements perm_rows = {
        hbox({text("  Default mode : ") | dim,
              text(pm) | color(Color::Cyan)}),
        hbox({text("  Allow tools  : ") | dim,
              text(std::format("{} tools", c.tools.size()))
                  | color(Color::Green)}),
        hbox({text("  Deny patterns: ") | dim,
              text(std::format("{} patterns", denied_count))
                  | color(Color::Red)}),
    };
    auto perms = vbox({
        text("Permissions") | bold | color(Color::Cyan),
        text(""),
        vbox(std::move(perm_rows)),
    });

    // --- Env vars (key=value) ---
    Elements env_rows;
    if (c.env_vars.empty()) {
        env_rows.push_back(text("  (none configured)") | dim);
    } else {
        for (const auto& kv : c.env_vars) {
            // Split KEY=VALUE if possible.
            auto eq = kv.find('=');
            if (eq == std::string::npos) {
                env_rows.push_back(text("  " + kv) | dim);
                continue;
            }
            std::string k = kv.substr(0, eq);
            std::string v = kv.substr(eq + 1);
            if (v.size() > 40) v = v.substr(0, 37) + "...";
            env_rows.push_back(hbox({
                text("  " + k + "=") | color(Color::BlueLight),
                text(v) | dim,
            }));
        }
    }
    auto env = vbox({
        text("Environment variables") | bold | color(Color::Blue),
        text(""),
        vbox(std::move(env_rows)) | vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, 8),
    });

    return vbox({
        text("Description") | bold | color(Color::Green),
        text(""),
        desc_el,
        separator() | dim,
        tools,
        separator() | dim,
        hbox({
            perms | flex,
            separator() | dim,
            env | flex,
        }),
    }) | borderEmpty;
}

// ============================================================
// Tab 2: Config (syntax-highlighted YAML/JSON + action buttons)
// ============================================================

/// Very small syntax-highlight pass for JSON/YAML.
/// Reuses TokenType from code_highlight so theming stays consistent.
[[nodiscard]] inline std::vector<HighlightedLine> SimpleHighlightJson(
    std::string_view src)
{
    using cc::ui::code_highlight::HighlightToken;
    std::vector<HighlightedLine> result;
    int line_no = 1;

    std::istringstream iss(std::string(src));
    std::string line;
    while (std::getline(iss, line)) {
        HighlightedLine hl;
        hl.line_number = line_no++;

        // Tokenize character-by-character (very small parser).
        int col = 0;
        for (size_t i = 0; i < line.size(); ) {
            char ch = line[i];
            if (std::isspace(static_cast<unsigned char>(ch))) {
                ++i; ++col;
                continue;
            }
            if (ch == '"') {
                // String token.
                size_t j = i + 1;
                while (j < line.size() && line[j] != '"') {
                    if (line[j] == '\\' && j + 1 < line.size()) j += 2;
                    else ++j;
                }
                if (j < line.size()) ++j;
                std::string tok = line.substr(i, j - i);
                // Check whether followed by ':' -> it's a key.
                bool is_key = (j < line.size() &&
                               std::string_view(line).substr(j).find_first_not_of(" \t")
                                   != std::string_view::npos &&
                               [&] {
                                   auto rest = std::string_view(line).substr(j);
                                   auto p = rest.find_first_not_of(" \t");
                                   return p != std::string_view::npos && rest[p] == ':';
                               }());
                HighlightToken t;
                t.text = std::move(tok);
                t.type = is_key ? TokenType::Variable : TokenType::String;
                t.start_col = col;
                t.end_col = static_cast<int>(col + (j - i));
                hl.tokens.push_back(std::move(t));
                col += static_cast<int>(j - i);
                i = j;
                continue;
            }
            if (ch == '{' || ch == '}' || ch == '[' || ch == ']'
                || ch == ',' || ch == ':') {
                HighlightToken t;
                t.text = std::string(1, ch);
                t.type = TokenType::Punctuation;
                t.start_col = col;
                t.end_col = col + 1;
                hl.tokens.push_back(std::move(t));
                ++i; ++col;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '-') {
                // Number (or keyword like -Infinity; keep simple).
                size_t j = i;
                while (j < line.size() &&
                       (std::isdigit(static_cast<unsigned char>(line[j]))
                        || line[j] == '.' || line[j] == 'e' || line[j] == 'E'
                        || line[j] == '+' || line[j] == '-')) {
                    ++j;
                }
                if (j > i) {
                    HighlightToken t;
                    t.text = line.substr(i, j - i);
                    t.type = TokenType::Number;
                    t.start_col = col;
                    t.end_col = col + static_cast<int>(j - i);
                    hl.tokens.push_back(std::move(t));
                    col += static_cast<int>(j - i);
                    i = j;
                    continue;
                }
            }
            // Keyword-ish (true/false/null).
            if (line.compare(i, 4, "true") == 0
                || line.compare(i, 5, "false") == 0
                || line.compare(i, 4, "null") == 0) {
                size_t len = (line[i] == 't' || line[i] == 'n') ? 4 : 5;
                HighlightToken t;
                t.text = line.substr(i, len);
                t.type = TokenType::Constant;
                t.start_col = col;
                t.end_col = col + static_cast<int>(len);
                hl.tokens.push_back(std::move(t));
                col += static_cast<int>(len);
                i += len;
                continue;
            }
            // Default: one character plain.
            HighlightToken t;
            t.text = std::string(1, ch);
            t.type = TokenType::Plain;
            t.start_col = col;
            t.end_col = col + 1;
            hl.tokens.push_back(std::move(t));
            ++i; ++col;
        }
        result.push_back(std::move(hl));
    }
    return result;
}

/// Render the Config tab. Returns a pair: {body element, footer hint bar}.
[[nodiscard]] inline Element RenderConfigTab(const AgentDetailsData& data) {
    std::string_view raw = data.raw_yaml ? *data.raw_yaml : data.raw_json;
    auto lines = SimpleHighlightJson(raw);

    // Map TokenType -> FTXUI color (matches code_highlight defaults).
    auto token_color = [](TokenType t) -> Color {
        switch (t) {
            case TokenType::String:      return Color::Green;
            case TokenType::Number:      return Color::Cyan;
            case TokenType::Constant:    return Color::Red;
            case TokenType::Punctuation: return Color::GrayLight;
            case TokenType::Variable:    return Color::CyanLight;
            case TokenType::Keyword:     return Color::Magenta;
            default:                     return Color::GrayLight;
        }
    };

    Elements code_rows;
    for (const auto& line : lines) {
        Elements row;
        row.push_back(
            text(std::format("{:>4} │ ", line.line_number)) | dim | color(Color::GrayDark));
        for (const auto& tok : line.tokens) {
            row.push_back(text(tok.text) | color(token_color(tok.type)));
        }
        if (line.tokens.empty()) row.push_back(text(""));
        code_rows.push_back(hbox(std::move(row)));
    }

    // Limit display height to ~20 rows + scroll.
    auto code_block = vbox(std::move(code_rows))
        | vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, 20) | flex;

    // Action pills (actual buttons live in the footer).
    auto actions = hbox({
        text(" [E]") | color(Color::Cyan), text("dit raw  ") | dim,
        text(" [I]") | color(Color::Cyan), text("mport JSON  ") | dim,
        text(" [X]") | color(Color::Cyan), text("port  ") | dim,
        filler(),
        text(data.raw_yaml ? "format: YAML" : "format: JSON") | dim,
    });

    return vbox({
        text("Full agent definition") | bold | color(Color::Yellow),
        text(""),
        code_block | borderLight,
        text(""),
        actions,
    }) | borderEmpty;
}

// ============================================================
// Tab 3: Runs + per-run StepTimeline
// ============================================================

/// Format a duration in ms as "12.3s" / "2m 04s".
[[nodiscard]] inline std::string fmt_dur(std::chrono::milliseconds ms) {
    auto s = ms.count() / 1000;
    if (s < 60) return std::format("{}.{}s", s, (ms.count() % 1000) / 100);
    return std::format("{}m {:02}s", s / 60, s % 60);
}

/// Render the Runs tab: header summary + run table + optional opened timeline.
[[nodiscard]] inline Element RenderRunsTab(
    const AgentDetailsData& data,
    std::shared_ptr<SharedAnimState> anim)
{
    const auto& runs = data.recent_runs;

    // --- Summary ---
    int total_steps = 0;
    double total_cost = 0.0;
    std::chrono::milliseconds total_dur{0};
    for (const auto& r : runs) {
        total_steps += r.step_count;
        total_cost += r.cost_usd;
        total_dur += r.duration;
    }
    auto summary = hbox({
        text(std::format(" {} runs", runs.size())) | bold,
        text(" · ") | dim,
        text(std::format("{} steps", total_steps)),
        text(" · ") | dim,
        text(fmt_dur(total_dur)) | dim,
        text(" · ") | dim,
        text(std::format("${:.3f}", total_cost)) | color(Color::Yellow),
        filler(),
        text("([o]pen run  [v]iew all steps)") | dim,
    });

    // --- Table header ---
    auto header = hbox({
        text("  #") | dim,
        separator() | dim,
        text(" Timestamp       ") | dim,
        separator() | dim,
        text(" Dur  ") | dim,
        separator() | dim,
        text(" InTk  ") | dim,
        separator() | dim,
        text(" OutTk ") | dim,
        separator() | dim,
        text(" Cost   ") | dim,
        separator() | dim,
        text(" St") | dim,
        separator() | dim,
        text(" Status") | dim,
    }) | size(WIDTH, EQUAL, 100);  // rough

    // --- Table rows ---
    Elements rows;
    rows.push_back(header);
    rows.push_back(separator());

    int n = static_cast<int>(runs.size());
    int selected = data.open_run_index.value_or(-1);
    for (int i = 0; i < n; ++i) {
        const auto& r = runs[i];
        bool is_sel = (i == selected);
        Color sel_bg = is_sel ? Color::RGB(25, 35, 50) : Color::Default;

        auto row = hbox({
            text(std::format(" {:>2}", i + 1)) | bold | color(Color::Cyan),
            separator() | dim,
            text(" " + r.timestamp.substr(0, 19) + " ") | dim,
            separator() | dim,
            text(" " + fmt_dur(r.duration) + " "),
            separator() | dim,
            text(std::format(" {:>5} ", r.input_tokens)) | color(Color::Cyan),
            separator() | dim,
            text(std::format(" {:>5} ", r.output_tokens)) | color(Color::Green),
            separator() | dim,
            text(std::format(" ${:>5.3f} ", r.cost_usd)) | color(Color::Yellow),
            separator() | dim,
            text(std::format(" {:>2} ", r.step_count)),
            separator() | dim,
            StatusDot({.status = r.status, .with_label = true,
                       .spinner_frame = anim ? anim->frame : 0}),
        });

        if (is_sel) row = row | bgcolor(sel_bg);
        rows.push_back(row);
        rows.push_back(separator() | dim);

        // If this run is open, append its StepTimeline.
        if (is_sel) {
            StepTimelineOptions sto;
            sto.steps = r.steps;
            sto.show_all = data.show_all_steps;
            rows.push_back(hbox({
                text("   "),
                vbox({
                    hbox({
                        text(std::format(" ┌─ Run #{} — {} steps — {} — ${:.3f}",
                                         i + 1, r.step_count,
                                         fmt_dur(r.duration), r.cost_usd))
                            | color(Color::Cyan),
                        filler(),
                    }),
                    StepTimeline(sto) | size(HEIGHT, LESS_THAN, 15),
                    hbox({
                        text(" └─"),
                        text(std::format(" (press [v] to {} all steps)",
                                         data.show_all_steps ? "collapse" : "show"))
                            | dim,
                    }),
                }) | border | color(Color::Cyan) | flex,
            }));
            rows.push_back(text(""));
        }
    }

    if (n == 0) {
        rows.push_back(text("  (no runs yet — press [r] to run)") | dim | center);
    }

    auto table = vbox(std::move(rows))
        | vscroll_indicator | yframe | size(HEIGHT, GREATER_THAN, 10) | flex;

    return vbox({
        summary,
        separator(),
        table,
    }) | borderEmpty;
}

// ============================================================
// Tab 4: Stats (ASCII bar chart + gauges + cost trend)
// ============================================================

/// Render a horizontal ASCII bar of `len` cells filled proportional to
/// value/max. Uses the block characters ▏▎▍▌▋▊▉█ for partial cells.
[[nodiscard]] inline std::string ascii_bar(double value, double max, int len) {
    if (max <= 0) return std::string(len, ' ');
    double frac = std::clamp(value / max, 0.0, 1.0);
    double cells = frac * len;
    int full = static_cast<int>(cells);
    int part = static_cast<int>((cells - full) * 8);
    static const char* parts = " ▏▎▍▌▋▊▉";
    std::string out(full, '█');
    if (part > 0 && full < len) out.push_back(parts[part]);
    while (static_cast<int>(out.size()) < len) out.push_back(' ');
    return out;
}

/// Render the Stats tab: run-count bars + avg-time gauge + cost trend.
[[nodiscard]] inline Element RenderStatsTab(const AgentDetailsData& data) {
    // --- Run-count ASCII bar chart ---
    int max_count = 1;
    for (const auto& b : data.run_count_bars) {
        max_count = std::max(max_count, b.count);
    }
    int bar_width = 30;
    Elements bars;
    bars.push_back(hbox({
        text(" Runs per bucket") | bold | color(Color::Cyan),
        filler(),
        text(std::format("max: {} runs", max_count)) | dim,
    }));
    bars.push_back(text(""));
    for (const auto& b : data.run_count_bars) {
        bars.push_back(hbox({
            text(std::format(" {:>6} ", b.label)) | color(Color::CyanLight),
            text(ascii_bar(b.count, max_count, bar_width))
                | color(Color::Cyan),
            text(std::format(" {:>3}", b.count)) | dim,
        }));
    }
    auto chart = vbox(std::move(bars)) | borderLight;

    // --- Two gauges: avg time + avg cost ---
    auto gauge = [](const StatsGauge& g, Color c) -> Element {
        int len = 30;
        std::string bar = ascii_bar(g.value, g.max, len);
        return hbox({
            text(std::format(" {:<16}", g.label)) | dim,
            text(bar) | color(c),
            text(std::format(" {:.2f}{}", g.value, g.unit)) | color(c) | bold,
        });
    };
    auto gauges = vbox({
        text(" Performance") | bold | color(Color::Yellow),
        text(""),
        gauge(data.avg_time_gauge, Color::Green),
        gauge(data.avg_cost_gauge, Color::Red),
    }) | borderLight;

    // --- Cost trend list ---
    Elements trend_rows;
    trend_rows.push_back(hbox({
        text(" Cost trend (recent)") | bold | color(Color::Magenta),
        filler(),
    }));
    trend_rows.push_back(text(""));
    for (size_t i = 0; i < data.cost_trend.size(); ++i) {
        const auto& [lbl, cost] = data.cost_trend[i];
        double max_trend = 1.0;
        for (const auto& [_, c2] : data.cost_trend)
            max_trend = std::max(max_trend, c2);
        trend_rows.push_back(hbox({
            text(std::format(" {:>14} ", lbl)) | dim,
            text(ascii_bar(cost, max_trend, 20)) | color(Color::Magenta),
            text(std::format(" ${:.4f}", cost)) | color(Color::Magenta),
        }));
    }
    if (data.cost_trend.empty()) {
        trend_rows.push_back(text("  (no cost data yet)") | dim);
    }
    auto trend = vbox(std::move(trend_rows)) | borderLight;

    return vbox({
        chart | flex,
        text(" "),
        hbox({
            gauges | flex,
            text(" "),
            trend | flex,
        }),
    }) | borderEmpty;
}

// ============================================================
// Modal header + footer + full assembly
// ============================================================

/// Render the editable header: avatar + name + status badge + role tags
/// + model override + big Run button.
[[nodiscard]] inline Element RenderModalHeader(
    const AgentDetailsData& data,
    bool editing_name,
    std::shared_ptr<SharedAnimState> anim)
{
    const auto& c = data.card;

    AvatarOptions av;
    av.name = c.name;
    av.agent_type = c.agent_type;
    av.force_general = !c.is_subagent && c.agent_type == "general-purpose";
    av.size_cells = 4;

    // Name row.
    Element name_el;
    if (editing_name) {
        name_el = hbox({
            text("▶ ") | color(Color::Cyan),
            text(c.name) | bgcolor(Color::RGB(30, 40, 60)) | bold | underlined,
            text(" ◀ (edit)") | dim | color(Color::Cyan),
        });
    } else {
        name_el = text(c.name) | bold | size(HEIGHT, EQUAL, 1);
    }

    // Tag row (role tags + model override).
    Elements tag_row;
    if (!c.role_tags.empty()) {
        tag_row.push_back(RoleTags(c.role_tags, 6));
    } else {
        tag_row.push_back(text(c.agent_type) | dim);
    }
    if (c.model_override) {
        tag_row.push_back(text("  model: ") | dim);
        tag_row.push_back(text(*c.model_override) | color(Color::Yellow));
    }

    // Big Run button.
    auto run_btn = hbox({
        text(" ▶ "),
        text("RUN") | bold | color(Color::White),
        text(" "),
    }) | color(Color::White) | bgcolor(Color::RGB(40, 160, 70)) | borderRounded;

    return hbox({
        AgentAvatar(av),
        text("  "),
        vbox({
            hbox({
                name_el,
                text("  "),
                StatusBadge(c.status, anim ? anim->frame : 0),
            }),
            text(""),
            hbox(std::move(tag_row)),
        }) | flex,
        run_btn,
    });
}

/// Render the footer: Save / Edit raw / Delete / Close.
/// The Delete button triggers on_delete (caller must wire TrustDialog).
[[nodiscard]] inline Element RenderModalFooter() {
    auto pill = [](std::string_view k, std::string_view label, Color c, bool danger = false) {
        return hbox({
            text(" [") | dim,
            text(std::string(k)) | color(c) | bold,
            text("] ") | dim,
            text(std::string(label)) | (danger ? color(Color::Red) : color(c)),
        });
    };
    return hbox({
        pill("S", "Save",      Color::Green),
        pill("E", "Edit raw",  Color::Cyan),
        // NOTE: "D" calls on_delete → wire a TrustDialog{severity=Critical}
        //       in the caller before calling delete_registry_entry().
        pill("D", "Delete",    Color::Red, /*danger=*/true),
        filler(),
        pill("Esc", "Close",   Color::GrayLight),
    });
}

// ============================================================
// Interactive AgentDetailsDialog component
// ============================================================

/// Props for AgentDetailsDialog.
struct AgentDetailsDialogOptions {
    AgentDetailsData initial_data;
    AgentDetailsCallbacks callbacks;
};

/// Build the interactive fullscreen agent-details modal.
/// Keyboard:
///   Tab / Shift+Tab   cycle tabs
///   1..4              jump to tab
///   n                 next run
///   p                 prev run
///   o                 open / close selected run timeline
///   v                 toggle show-all-steps inside timeline
///   e / E             edit raw config (fires on_edit_raw)
///   x / X             export JSON
///   i / I             import JSON
///   r                 run agent
///   S                 save
///   D                 delete  (→ TrustDialog in caller)
///   Esc               close
[[nodiscard]] inline Component AgentDetailsDialog(AgentDetailsDialogOptions opts) {
    struct State {
        AgentDetailsData data;
        DetailTab tab = DetailTab::Overview;
        bool editing_name = false;
        std::shared_ptr<SharedAnimState> anim;
        AgentDetailsCallbacks cb;
    };

    auto s = std::make_shared<State>();
    s->data = std::move(opts.initial_data);
    s->anim = std::make_shared<SharedAnimState>();
    s->cb = std::move(opts.callbacks);

    auto renderer = Renderer([s] {
        s->anim->tick();

        // 4-tab row.
        std::vector<Tab> tabs = {
            {.label = "Overview", .id = "overview", .is_all_tab = true},
            {.label = "Config",   .id = "config",   .is_all_tab = true},
            {.label = "Runs",     .id = "runs",     .is_all_tab = true},
            {.label = "Stats",    .id = "stats",    .is_all_tab = true},
        };
        TagTabsOptions tto;
        tto.tabs = tabs;
        tto.active_tab = static_cast<int>(s->tab);
        tto.show_resume_label = false;
        auto tabs_el = TagTabsComponent(tto);

        // Body depends on tab.
        Element body;
        switch (s->tab) {
            case DetailTab::Overview: body = RenderOverviewTab(s->data); break;
            case DetailTab::Config:   body = RenderConfigTab(s->data);   break;
            case DetailTab::Runs:     body = RenderRunsTab(s->data, s->anim); break;
            case DetailTab::Stats:    body = RenderStatsTab(s->data);    break;
        }

        return vbox({
            RenderModalHeader(s->data, s->editing_name, s->anim),
            separator(),
            tabs_el->Render(),
            separator() | dim,
            body | flex,
            separator(),
            RenderModalFooter(),
        }) | borderRounded | size(WIDTH, GREATER_THAN, 80);
    });

    return renderer | CatchEvent([s](Event event) -> bool {
        int n = static_cast<int>(s->data.recent_runs.size());

        // --- Close ---
        if (event == Event::Escape) {
            if (s->cb.on_close) s->cb.on_close();
            return true;
        }

        // --- Tab navigation ---
        if (event == Event::Tab) {
            auto cur = static_cast<int>(s->tab);
            s->tab = static_cast<DetailTab>((cur + 1) % 4);
            return true;
        }
        if (event == Event::TabReverse) {
            auto cur = static_cast<int>(s->tab);
            s->tab = static_cast<DetailTab>((cur + 3) % 4);
            return true;
        }
        if (event == Event::Character('1')) { s->tab = DetailTab::Overview; return true; }
        if (event == Event::Character('2')) { s->tab = DetailTab::Config;   return true; }
        if (event == Event::Character('3')) { s->tab = DetailTab::Runs;     return true; }
        if (event == Event::Character('4')) { s->tab = DetailTab::Stats;    return true; }

        // --- Footer actions ---
        if (event == Event::Character('S')) {
            if (s->cb.on_save) s->cb.on_save(s->data.card);
            return true;
        }
        if (event == Event::Character('e') || event == Event::Character('E')) {
            if (s->cb.on_edit_raw) s->cb.on_edit_raw(s->data.card);
            return true;
        }
        if (event == Event::Character('D')) {
            // NOTE: caller must wrap this in a TrustDialog critical-confirm.
            if (s->cb.on_delete) s->cb.on_delete(s->data.card);
            return true;
        }
        if (event == Event::Character('x') || event == Event::Character('X')) {
            if (s->cb.on_export_json) s->cb.on_export_json(s->data.card);
            return true;
        }
        if (event == Event::Character('i') || event == Event::Character('I')) {
            if (s->cb.on_import_json) s->cb.on_import_json(s->data.card);
            return true;
        }
        if (event == Event::Character('r')) {
            if (s->cb.on_run) s->cb.on_run(s->data.card);
            return true;
        }

        // --- Runs-tab: open run + show-all-steps ---
        if (s->tab == DetailTab::Runs && n > 0) {
            if (event == Event::Character('n')) {
                int cur = s->data.open_run_index.value_or(-1);
                cur = (cur + 1 + n) % n;
                s->data.open_run_index = cur;
                s->data.show_all_steps = false;
                return true;
            }
            if (event == Event::Character('p')) {
                int cur = s->data.open_run_index.value_or(0);
                cur = (cur - 1 + n) % n;
                s->data.open_run_index = cur;
                s->data.show_all_steps = false;
                return true;
            }
            if (event == Event::Character('o')) {
                if (s->data.open_run_index) s->data.open_run_index.reset();
                else                        s->data.open_run_index = 0;
                s->data.show_all_steps = false;
                return true;
            }
            if (event == Event::Character('v')) {
                s->data.show_all_steps = !s->data.show_all_steps;
                return true;
            }
        }

        return false;
    });
}

} // namespace cc::ui::agents::details
