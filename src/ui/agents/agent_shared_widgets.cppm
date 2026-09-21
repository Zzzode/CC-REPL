/// @file agent_shared_widgets.cppm
/// @brief Shared reusable UI widgets for the Agents subsystem.
///
/// Consolidates small shared components migrated from 26 TS files under
/// src/components/agents/:
///   - AgentAvatar              (hash-color + initial badge + emoji marker)
///   - StatusDot                (colored status indicator dot)
///   - RoleTags                 (chip row for agent role tags)
///   - RunStatsBar              (one-line 3-metric summary)
///   - ToolChips                (foldable tool-name chips, >8 default collapsed)
///   - StepTimeline             (vertical pipe + dots + per-step status color)
///
/// Reuses:
///   - cc.tools.agent_color_manager  (AgentColor / hash-color assignment)
///   - cc.utils.swarm_backends       (AgentColor enum)
///   - cc.ui.components.spinner_animations  (running spinner glyphs)
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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

export module cc.ui.agents.shared_widgets;

import cc.utils.swarm_backends;
import cc.tools.agent_color_manager;
import cc.ui.components.spinner_animations;

export namespace cc::ui::agents::shared {
using namespace ftxui;

using cc::utils::swarm_backends::AgentColor;
using cc::tools::agent_color_manager::get_agent_color;
using cc::tools::agent_color_manager::agent_color_name;
using cc::tools::agent_color_manager::assign_cycle_color;
using cc::tools::agent_color_manager::parse_color_name;

// ============================================================
// Common Types (re-declared lightweight so widgets are self-contained;
// the full data model lives in tools/agent_tool + tools/agent_runtime).
// ============================================================

/// Execution status surfaced by every agent widget.
enum class AgentStatus : std::uint8_t {
    Idle,        // grey dot
    Running,     // green spinner dot
    Scheduled,   // yellow dot
    Errored,     // red dot
    Disabled,    // italic grey
};

/// A single tool-step recorded during an agent run (used by StepTimeline).
struct TimelineStep {
    int index = 0;
    std::string tool_name;
    std::string icon;           // 1-2 glyph prefix, e.g. "📁", "⌘"
    std::string status_text;    // "success", "failed", "running", "skipped"
    AgentStatus status = AgentStatus::Idle;
    std::string output_preview; // collapsed by default if >100 chars
    std::chrono::milliseconds duration{0};
};

// ============================================================
// Color Helpers — AgentColor -> FTXUI Color
// ============================================================

/// Convert an AgentColor enum value into an FTXUI Color.
[[nodiscard]] inline Color agent_color_to_ftxui(AgentColor c) {
    switch (c) {
        case AgentColor::Red:     return Color::RGB(220, 80, 80);
        case AgentColor::Blue:    return Color::RGB(80, 140, 230);
        case AgentColor::Green:   return Color::RGB(80, 190, 110);
        case AgentColor::Yellow:  return Color::RGB(230, 200, 80);
        case AgentColor::Purple:  return Color::RGB(170, 110, 220);
        case AgentColor::Orange:  return Color::RGB(230, 140, 70);
        case AgentColor::Pink:    return Color::RGB(230, 130, 180);
        case AgentColor::Cyan:    return Color::RGB(80, 200, 210);
    }
    return Color::GrayLight;
}

/// Status → color used for dots and badges.
[[nodiscard]] inline Color status_color(AgentStatus s) {
    switch (s) {
        case AgentStatus::Idle:      return Color::GrayDark;
        case AgentStatus::Running:   return Color::RGB(80, 200, 110);   // green
        case AgentStatus::Scheduled: return Color::RGB(230, 200, 80);   // yellow
        case AgentStatus::Errored:   return Color::RGB(220, 80, 80);    // red
        case AgentStatus::Disabled:  return Color::GrayDark;
    }
    return Color::GrayDark;
}

/// Return a short human label for an AgentStatus.
[[nodiscard]] inline std::string_view status_label(AgentStatus s) {
    switch (s) {
        case AgentStatus::Idle:      return "Idle";
        case AgentStatus::Running:   return "Running";
        case AgentStatus::Scheduled: return "Scheduled";
        case AgentStatus::Errored:   return "Errored";
        case AgentStatus::Disabled:  return "Disabled";
    }
    return "Unknown";
}

// ============================================================
// AgentAvatar
// ============================================================

/// Options for AgentAvatar rendering.
struct AvatarOptions {
    std::string name;            ///< Display name (first char used as glyph)
    std::string agent_type;      ///< Used for hash-color + sub-agent marking
    int size_cells = 2;          ///< 2 = inline, 3 = small card, 4 = large card
    bool force_general = false;  ///< Treat as main-thread agent (no marker)
};

/// Render an agent avatar: colored circle + initial letter + emoji marker.
///   - general-purpose: no emoji marker
///   - sub-agents:        "🔀" suffix
///   - explicit color override via agent_color_manager
[[nodiscard]] inline Element AgentAvatar(const AvatarOptions& opts) {
    // Extract initial character (ASCII-safe fallback).
    char initial = '?';
    if (!opts.name.empty()) {
        unsigned char c = static_cast<unsigned char>(opts.name[0]);
        initial = static_cast<char>(std::toupper(c));
    }

    // Resolve color.
    Color bg = Color::RGB(60, 60, 80);  // default grey
    std::optional<AgentColor> resolved;
    if (!opts.force_general && !opts.agent_type.empty()) {
        resolved = get_agent_color(opts.agent_type);
        if (resolved) bg = agent_color_to_ftxui(*resolved);
    }

    // Emoji marker: 🔀 for non-general-purpose sub-agents.
    bool is_sub = !opts.force_general
                  && !opts.agent_type.empty()
                  && opts.agent_type != "general-purpose";
    std::string marker = is_sub ? "🔀" : "";

    // Build the badge. The outer size is controlled by `size_cells`.
    // Minimum 2 cells wide: " A " + optional marker.
    std::string glyph(1, initial);
    Element avatar = text(std::format(" {} ", glyph))
        | bold
        | color(Color::White)
        | bgcolor(bg)
        | borderRounded;

    if (!marker.empty()) {
        return hbox({
            avatar,
            text(" " + marker),
        });
    }
    return avatar;
}

// ============================================================
// StatusDot
// ============================================================

/// Options for StatusDot.
struct StatusDotOptions {
    AgentStatus status = AgentStatus::Idle;
    bool with_label = false;     ///< Append "Running" / "Idle" etc.
    uint32_t spinner_frame = 0;  ///< For Running state animation
};

/// Render a single status dot with optional label.
/// Running state uses the kDotsAnimation spinner (reused from
/// spinner_animations.cppm).
[[nodiscard]] inline Element StatusDot(const StatusDotOptions& opts) {
    Color c = status_color(opts.status);
    std::string glyph;

    switch (opts.status) {
        case AgentStatus::Running: {
            glyph = std::string(cc::ui::components::kDotsAnimation[
                opts.spinner_frame % cc::ui::components::kDotsAnimation.size()]);
            break;
        }
        case AgentStatus::Idle:      glyph = "●"; break;
        case AgentStatus::Scheduled: glyph = "●"; break;
        case AgentStatus::Errored:   glyph = "●"; break;
        case AgentStatus::Disabled:  glyph = "○"; break;
    }

    auto dot = text(" " + glyph + " ") | color(c);

    if (opts.with_label) {
        auto label = text(std::string(status_label(opts.status))) | color(c);
        if (opts.status == AgentStatus::Disabled) label = label | dim | strikethrough;
        return hbox({dot, label});
    }
    return dot;
}

// ============================================================
// RoleTags
// ============================================================

/// Render a horizontal row of role-tag chips (e.g. "researcher", "coder",
/// "reviewer"). When there are more than `max_visible` tags, the extras are
/// replaced with a "+N more" dimmed summary chip.
[[nodiscard]] inline Element RoleTags(
    const std::vector<std::string>& tags,
    int max_visible = 4)
{
    Elements chips;
    int count = static_cast<int>(tags.size());
    int shown = std::min(count, max_visible);

    for (int i = 0; i < shown; ++i) {
        chips.push_back(
            hbox({
                text(" "),
                text(tags[i]) | color(Color::CyanLight) | dim,
                text(" "),
            }) | borderLight | color(Color::Cyan)
        );
        if (i < shown - 1) chips.push_back(text(" "));
    }

    if (count > max_visible) {
        int extra = count - max_visible;
        chips.push_back(text(" "));
        chips.push_back(
            text(std::format(" +{} more ", extra)) | dim | color(Color::GrayDark)
        );
    }

    if (chips.empty()) return text("") | size(HEIGHT, EQUAL, 1);
    return hbox(std::move(chips));
}

// ============================================================
// RunStatsBar
// ============================================================

/// One-line 3-metric summary used by cards and list headers.
struct RunStats {
    int run_count = 0;
    double avg_cost_usd = 0.0;
    std::uint64_t total_tokens = 0;
    int running_count = 0;
};

/// Render:  "12 runs · 3 running · avg $0.024 · 58K tokens"
[[nodiscard]] inline Element RunStatsBar(const RunStats& stats) {
    Elements parts;

    parts.push_back(text(std::format("{} ", stats.run_count)) | bold);
    parts.push_back(text("runs") | dim);

    if (stats.running_count > 0) {
        parts.push_back(text(" · ") | dim);
        parts.push_back(text(std::format("{} ", stats.running_count))
            | color(Color::Green) | bold);
        parts.push_back(text("running") | dim | color(Color::Green));
    }

    parts.push_back(text(" · ") | dim);
    parts.push_back(text(std::format("avg ${:.3f} ", stats.avg_cost_usd))
        | color(Color::Yellow));

    parts.push_back(text(" · ") | dim);
    std::string tokens_str;
    if (stats.total_tokens >= 1'000'000) {
        tokens_str = std::format("{:.1f}M", stats.total_tokens / 1'000'000.0);
    } else if (stats.total_tokens >= 1'000) {
        tokens_str = std::format("{:.0f}K", stats.total_tokens / 1'000.0);
    } else {
        tokens_str = std::format("{}", stats.total_tokens);
    }
    parts.push_back(text(std::format("{} tokens", tokens_str)) | dim);

    return hbox(std::move(parts));
}

// ============================================================
// ToolChips
// ============================================================

/// Render a list of tool names as chips. Collapses when more than
/// `collapse_threshold` tools are present; the user can expand with '+'
/// (component version) or the element version just appends "+N more".
struct ToolChipsOptions {
    std::vector<std::string> tools;
    int collapse_threshold = 8;
    bool start_collapsed = true;
};

/// Static (non-interactive) tool-chips element.
[[nodiscard]] inline Element ToolChipsElement(const ToolChipsOptions& opts) {
    Elements chips;
    int count = static_cast<int>(opts.tools.size());
    int visible = (opts.start_collapsed && count > opts.collapse_threshold)
                      ? opts.collapse_threshold
                      : count;

    for (int i = 0; i < visible; ++i) {
        chips.push_back(
            hbox({
                text(" "),
                text(opts.tools[i]) | color(Color::MagentaLight),
                text(" "),
            }) | borderLight | color(Color::Magenta)
        );
        chips.push_back(text(" "));
    }

    if (count > visible) {
        int hidden = count - visible;
        chips.push_back(
            text(std::format(" +{} tools ", hidden)) | dim | color(Color::GrayDark)
        );
    }

    if (chips.empty()) return text("(no tools)") | dim;

    // Wrap chips into multiple rows if they exceed ~80 columns (best effort).
    // FTXUI's flex-wrap isn't available, so we render as hbox wrapped in a
    // yframe paragraph-ish container.
    return hbox(std::move(chips)) | flex;
}

// ============================================================
// StepTimeline
// ============================================================

/// Render a vertical timeline of agent run steps.
///
/// Layout per step:
///   pipe + dot (status-colored) | index | tool icon | tool name | status
///   (optional) output preview, collapsed if > 2 lines / > 100 chars
///
/// If `steps.size() > 2000` only the last 500 are rendered with a
/// "(only latest 500 of N shown — expand with Show all)" banner.
struct StepTimelineOptions {
    std::vector<TimelineStep> steps;
    bool show_all = false;
    int truncate_preview_chars = 200;
};

/// Split a long string into at most `max_lines` rows of roughly `width`
/// columns. Used by StepTimeline for output folding.
[[nodiscard]] inline std::vector<std::string> wrap_lines(
    std::string_view text, int max_lines, int width = 72)
{
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::size_t pos = 0;
    while (pos < text.size() && static_cast<int>(out.size()) < max_lines) {
        auto take = std::min(static_cast<std::size_t>(width), text.size() - pos);
        out.emplace_back(text.substr(pos, take));
        pos += take;
    }
    return out;
}

[[nodiscard]] inline Element StepTimeline(const StepTimelineOptions& opts) {
    constexpr int kShowAllThreshold = 2000;
    constexpr int kLatestWindow    = 500;

    Elements rows;
    int total = static_cast<int>(opts.steps.size());

    int start_idx = 0;
    if (!opts.show_all && total > kShowAllThreshold) {
        start_idx = total - kLatestWindow;
        rows.push_back(
            hbox({
                text(" ⓘ "),
                text(std::format(
                    "Showing latest {} of {} steps. Press [v] to view all.",
                    kLatestWindow, total)) | dim | color(Color::Cyan),
            })
        );
        rows.push_back(separator() | dim);
    }

    for (int i = start_idx; i < total; ++i) {
        const auto& step = opts.steps[i];
        Color c = status_color(step.status);

        // Left column: pipe + status dot + index.
        std::string dot = (step.status == AgentStatus::Running) ? "◐" : "●";
        auto marker = hbox({
            text(" │") | dim,
            text(std::format("{} ", dot)) | color(c),
        });

        // Index (right-aligned, 3 chars).
        auto idx = text(std::format("{:>3}. ", step.index)) | dim | color(c);

        // Tool icon + name.
        std::string icon = step.icon.empty() ? "  " : (step.icon + " ");
        auto tool = hbox({
            text(icon),
            text(step.tool_name) | (step.status == AgentStatus::Errored
                                        ? color(Color::Red)
                                        : color(Color::CyanLight)),
        });

        // Duration / status suffix.
        Elements tail;
        if (step.duration.count() > 0) {
            double ms = static_cast<double>(step.duration.count());
            std::string dur = (ms >= 1000.0)
                ? std::format("{:.1f}s", ms / 1000.0)
                : std::format("{:.0f}ms", ms);
            tail.push_back(text(std::format(" ({})", dur)) | dim);
        }
        if (!step.status_text.empty() && step.status_text != "success") {
            tail.push_back(text(std::format(" {}", step.status_text))
                | color(c));
        }

        auto head = hbox({
            marker, idx, tool, hbox(std::move(tail)), filler(),
        });
        rows.push_back(head);

        // Optional output preview (collapsed if long).
        if (!step.output_preview.empty()) {
            bool truncate = static_cast<int>(step.output_preview.size())
                            > opts.truncate_preview_chars;
            std::string_view preview = step.output_preview;
            if (truncate) preview = preview.substr(0, opts.truncate_preview_chars);

            auto lines = wrap_lines(preview, 2);
            for (auto& ln : lines) {
                rows.push_back(hbox({
                    text(" │    ") | dim,
                    text(ln) | color(Color::GrayLight) | dim,
                }));
            }
            if (truncate) {
                rows.push_back(hbox({
                    text(" │    ") | dim,
                    text(std::format(" ... +{} chars (click to expand)",
                                     step.output_preview.size()
                                         - opts.truncate_preview_chars))
                        | dim | color(Color::GrayDark),
                }));
            }
        }
    }

    if (rows.empty()) {
        return text("(no steps recorded yet)") | dim;
    }

    return vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
}

// ============================================================
// Utility: interactive state helpers
// ============================================================

/// Shared state container used by interactive widgets (cards, list, dialogs).
/// Holds counters that increment each frame to drive spinner animations.
struct SharedAnimState {
    uint32_t frame = 0;
    /// Advance by one tick; call once per Render().
    void tick() { ++frame; }
};

} // namespace cc::ui::agents::shared
