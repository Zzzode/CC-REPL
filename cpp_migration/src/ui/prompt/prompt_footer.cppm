/// @file prompt_footer.cppm
/// @brief FTXUI Component footer bar below the Prompt text input.
///
/// Renders:
///   LEFT:  permission mode pill → effort pill → active agent pill
///          → swarm banner → task/team pill presence → PR badge hook
///          → shift+tab cycle hint → mode-cyle hotkey
///   RIGHT: input token estimate → session verbose token count
///          → "N chars × M lines" → ctrl+enter multi-line hint
///          → sandbox / IDE hint → loading spinner hint
///
/// Migrated from src/components/PromptInput/PromptInputFooterLeftSide.tsx
///              + PromptInputFooterRightSide.tsx + Notifications.tsx
/// (combined ~900 TS lines → focused FTXUI Element render, ~750 C++ lines).
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.prompt.prompt_footer;

import ui.components.text_input;

export namespace cc::ui::prompt::footer {
using namespace ftxui;
using ui::components::PermissionMode;
using ui::components::PromptContext;
using ui::components::PromptMode;

// ============================================================
// Types
// ============================================================

/// External pill presence: rendered inside the footer as a small tag
/// that the user can click / focus to navigate to another screen.
struct TaskPill {
    int count = 0;
    bool selected = false;
};
struct TeamPill {
    int count = 0;
    bool selected = false;
};
struct PrBadge {
    std::string title;          // e.g. "PR #123 ready"
    Color color = Color::Green;
};

/// Configuration for PromptFooter component.
/// Most of the render data comes from `PromptContext`; extras are
/// interaction callbacks and the optional pill/badge payloads.
struct PromptFooterOptions {
    /// Live context from TextInputImpl (caller refreshes each frame)
    std::function<const PromptContext&()> get_context;

    // --- optional extras ---
    std::optional<TaskPill> tasks;
    std::optional<TeamPill> teams;
    std::optional<PrBadge> pr_badge;

    // --- interaction callbacks ---
    /// Called when user presses Shift+Tab (cycle permission mode)
    std::function<void()> on_cycle_mode;
    /// Called when user presses Ctrl+T (jump to tasks view)
    std::function<void()> on_open_tasks;
    /// Called when user presses Ctrl+G (jump to teams view)
    std::function<void()> on_open_teams;
    /// Called when user presses Ctrl+P (jump to PR view)
    std::function<void()> on_open_pr;

    // --- misc tuning ---
    bool show_verbose_tokens = true;
    bool show_mode_cycle_hint = true;
};

// ============================================================
// Rendering Helpers — left side
// ============================================================

/// Symbol + title for the permission/auto mode pill
[[nodiscard]] inline std::pair<std::string, std::string>
mode_pill_info(PermissionMode m) {
    switch (m) {
        case PermissionMode::Default:     return {"○",  "AUTO: off"};
        case PermissionMode::AutoApprove: return {"●",  "AUTO: safe"};
        case PermissionMode::Unlimited:   return {"◉",  "AUTO: on"};
        case PermissionMode::PlanOnly:    return {"▢",  "PLAN only"};
        case PermissionMode::UltraPlan:   return {"▣",  "ULTRA plan"};
        case PermissionMode::UltraReview: return {"◫",  "ULTRA review"};
    }
    return {"○", "AUTO: off"};
}
[[nodiscard]] inline Color mode_pill_color(PermissionMode m) {
    switch (m) {
        case PermissionMode::Default:     return Color::GrayLight;
        case PermissionMode::AutoApprove: return Color::Cyan;
        case PermissionMode::Unlimited:   return Color::Green;
        case PermissionMode::PlanOnly:    return Color::Yellow;
        case PermissionMode::UltraPlan:   return Color::Magenta;
        case PermissionMode::UltraReview: return Color::BlueLight;
    }
    return Color::White;
}

/// Human-readable effort label
[[nodiscard]] inline std::string effort_display(std::string_view level) {
    if (level.empty()) return {};
    // Accept raw TS strings or lowercase keys
    if (level == "low"    || level == "Low")    return "Effort: low";
    if (level == "medium" || level == "Medium") return "Effort: medium";
    if (level == "high"   || level == "High")   return "Effort: high";
    if (level == "ultra"  || level == "Ultra")  return "Effort: ultra";
    return std::string("Effort: ") + std::string(level);
}
[[nodiscard]] inline Color effort_color(std::string_view level) {
    if (level == "low")    return Color::Cyan;
    if (level == "medium") return Color::Green;
    if (level == "high")   return Color::Yellow;
    if (level == "ultra")  return Color::Magenta;
    return Color::GrayLight;
}

[[nodiscard]] inline Element render_mode_pill(const PromptContext& ctx) {
    auto [sym, title] = mode_pill_info(ctx.permission);
    Color clr = mode_pill_color(ctx.permission);
    return hbox({
        text(" ") | dim,
        text(sym) | bold | color(clr),
        text(" " + title) | color(clr),
        text(" ") | dim,
    });
}

[[nodiscard]] inline Element render_effort_pill(const PromptContext& ctx) {
    auto label = effort_display(ctx.effort_level);
    if (label.empty()) return text("");
    return hbox({
        text(" │ ") | dim,
        text(label) | color(effort_color(ctx.effort_level)) | dim,
    });
}

[[nodiscard]] inline Element render_agent_pill(const PromptContext& ctx) {
    if (!ctx.active_agent || ctx.active_agent->empty()) return text("");
    return hbox({
        text(" │ ") | dim,
        text("[●] ") | bold | color(Color::Blue),
        text(*ctx.active_agent) | color(Color::BlueLight),
    });
}

[[nodiscard]] inline Element render_swarm_banner(const PromptContext& ctx) {
    if (!ctx.swarm_banner || ctx.swarm_banner->empty()) return text("");
    return hbox({
        text(" │ ") | dim,
        text("⊟ " + *ctx.swarm_banner) | bold | color(Color::Magenta),
    });
}

[[nodiscard]] inline Element render_task_pill(const std::optional<TaskPill>& t) {
    if (!t || t->count <= 0) return text("");
    auto base = text(std::format(" {}⏳ ", t->count)) | dim | color(Color::Cyan);
    if (t->selected) base = base | inverted | bold;
    return hbox({ text(" ") | dim, base });
}
[[nodiscard]] inline Element render_team_pill(const std::optional<TeamPill>& t) {
    if (!t || t->count <= 0) return text("");
    auto base = text(std::format(" {}👥 ", t->count)) | dim | color(Color::Magenta);
    if (t->selected) base = base | inverted | bold;
    return hbox({ text(" ") | dim, base });
}
[[nodiscard]] inline Element render_pr_badge(const std::optional<PrBadge>& p) {
    if (!p || p->title.empty()) return text("");
    return hbox({
        text(" │ ") | dim,
        text("⇪ " + p->title) | color(p->color) | dim,
    });
}

[[nodiscard]] inline Element render_mode_cycle_hint(const PromptContext& ctx) {
    if (!ctx.show_mode_cycle_hint) return text("");
    return hbox({
        text("  ") | dim,
        text("[shift+tab cycle]") | dim,
    });
}

// ============================================================
// Rendering Helpers — right side
// ============================================================

/// Format verbose tokens + input estimate. Aligned on the right end.
[[nodiscard]] inline Element render_token_stats(
    const PromptContext& ctx, bool show_verbose) {
    Elements parts;
    // input estimate is often the most useful one
    if (ctx.input_tokens_estimate > 0) {
        parts.push_back(text(std::format("≈{}in",
            ctx.input_tokens_estimate)) | dim | color(Color::Cyan));
    }
    if (show_verbose && ctx.token_count > 0) {
        if (!parts.empty()) parts.push_back(text(" │ ") | dim);
        parts.push_back(text(std::format("{}Σ",
            ctx.token_count)) | dim);
    }
    if (parts.empty()) return text("");
    return hbox(std::move(parts));
}

/// N chars × M lines  (matches TS char counter in footer)
[[nodiscard]] inline Element render_char_line_counter(const PromptContext& ctx) {
    if (ctx.char_count == 0 && ctx.line_count <= 1) return text("");
    std::string s;
    if (ctx.line_count > 1) {
        s = std::format("{}×{}", ctx.char_count, ctx.line_count);
    } else {
        s = std::format("{}", ctx.char_count);
    }
    return text(s) | dim;
}

[[nodiscard]] inline Element render_ctrl_enter_hint(const PromptContext& ctx) {
    if (ctx.line_count <= 1) return text("");
    // Only show when the buffer actually spans multiple lines
    return text(" [ctrl+enter to send]") | dim | color(Color::Yellow);
}

[[nodiscard]] inline Element render_sandbox_hint(const PromptContext& ctx) {
    if (!ctx.show_sandbox_hint) return text("");
    return hbox({
        text(" ") | dim,
        text("[sandboxed]") | dim | color(Color::Yellow),
    });
}

[[nodiscard]] inline Element render_loading_spinner_hint(const PromptContext& ctx) {
    if (!ctx.is_loading) return text("");
    // A subtle spinner glyph; the heavy spinner lives above the input area
    return hbox({
        text(" ") | dim,
        text("◐") | color(Color::Cyan) | bold,
    });
}

[[nodiscard]] inline Element render_token_warning(const PromptContext& ctx) {
    if (!ctx.is_above_warning_threshold) return text("");
    return hbox({
        text(" ⚠ ") | color(Color::Red) | bold,
        text("tokens high") | dim | color(Color::Red),
    });
}

// ============================================================
// Full footer render
// ============================================================

/// Render the prompt footer as a single ftxui Element.
///
/// Layout (horizontal, single row):
///   [mode-pill] [effort] [agent] [swarm] [tasks] [teams] [pr]
///   ── filler ──
///   [token-warning] [loading] [sandbox] [ctrl+enter] [chars×lines] [tokens]
[[nodiscard]] inline Element RenderPromptFooter(
    const PromptContext& ctx,
    const PromptFooterOptions& opts) {

    Elements left_parts;
    left_parts.push_back(render_mode_pill(ctx));
    left_parts.push_back(render_effort_pill(ctx));
    left_parts.push_back(render_agent_pill(ctx));
    left_parts.push_back(render_swarm_banner(ctx));
    left_parts.push_back(render_task_pill(opts.tasks));
    left_parts.push_back(render_team_pill(opts.teams));
    left_parts.push_back(render_pr_badge(opts.pr_badge));
    if (opts.show_mode_cycle_hint) {
        left_parts.push_back(render_mode_cycle_hint(ctx));
    }

    Elements right_parts;
    right_parts.push_back(render_token_warning(ctx));
    right_parts.push_back(render_loading_spinner_hint(ctx));
    right_parts.push_back(render_sandbox_hint(ctx));
    right_parts.push_back(render_ctrl_enter_hint(ctx));
    right_parts.push_back(render_char_line_counter(ctx));
    if (!right_parts.empty() &&
        (opts.show_verbose_tokens || ctx.input_tokens_estimate > 0)) {
        right_parts.insert(right_parts.begin(), text("  ") | dim);
    }
    right_parts.push_back(render_token_stats(ctx, opts.show_verbose_tokens));

    return hbox({
        hbox(std::move(left_parts)),
        filler(),
        hbox(std::move(right_parts)),
    }) | size(HEIGHT, EQUAL, 1);
}

// ============================================================
// Interactive Component
// ============================================================

/// Wrap RenderPromptFooter() into a Component that catches hotkeys for
/// the pills it exposes (Shift+Tab cycle mode, Ctrl+T tasks, Ctrl+G teams,
/// Ctrl+P PR).
///
/// NOTE: The actual state mutations (e.g. advancing the permission mode)
/// live OUTSIDE this component — the caller owns PromptContext and the
/// TextInputImpl. We just invoke the callbacks for deterministic tests.
[[nodiscard]] inline Component PromptFooter(PromptFooterOptions options) {
    auto opts = std::make_shared<PromptFooterOptions>(std::move(options));

    return Renderer([opts] {
        if (!opts->get_context) return text("");
        const auto& ctx = opts->get_context();
        return RenderPromptFooter(ctx, *opts);
    }) | CatchEvent([opts](Event event) -> bool {
        // Shift+Tab → cycle permission mode (matches TS footer)
        if (event == Event::TabReverse) {
            if (opts->on_cycle_mode) opts->on_cycle_mode();
            return true;
        }
        // Ctrl+T → tasks
        if (event.is_character() && event.character().size() == 1 &&
            event.character()[0] == '\x14') {
            if (opts->on_open_tasks) opts->on_open_tasks();
            return true;
        }
        // Ctrl+G → teams
        if (event.is_character() && event.character().size() == 1 &&
            event.character()[0] == '\x07') {
            if (opts->on_open_teams) opts->on_open_teams();
            return true;
        }
        // Ctrl+P → PR
        if (event.is_character() && event.character().size() == 1 &&
            event.character()[0] == '\x10') {
            if (opts->on_open_pr) opts->on_open_pr();
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::prompt::footer
