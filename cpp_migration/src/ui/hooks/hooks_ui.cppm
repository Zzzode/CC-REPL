/// @file hooks_ui.cppm
/// @brief Hook execution display, hook config editor, hook status indicators.
/// Migrated from src/components/hooks/ (all files)
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <variant>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.hooks_ui;

export namespace cc::ui::hooks_ui {
using namespace ftxui;

// ============================================================
// Types (migrated from hooks settings and config types)
// ============================================================

/// Hook event types that can trigger hooks
enum class HookEvent : std::uint8_t {
    PreToolUse,
    PostToolUse,
    Notification,
    Stop,
    SubagentStop,
};

/// Hook type discriminator
enum class HookType : std::uint8_t {
    Command,    // Shell command to execute
    Prompt,     // Prompt to inject into conversation
    Agent,      // Agent-based hook
    Http,       // HTTP webhook
};

/// Matcher for determining when a hook fires
struct HookMatcher {
    std::string tool_name;              // Tool name pattern (glob supported)
    std::optional<std::string> project; // Project filter
    std::optional<std::string> path;    // File path pattern
};

/// A single hook configuration entry
struct IndividualHookConfig {
    std::string id;
    std::string name;
    HookType type;
    HookEvent event;
    HookMatcher matcher;
    std::string command_or_content;     // Command string or prompt content
    std::optional<int> timeout_ms;
    bool enabled = true;
};

/// Hook execution status
enum class HookExecStatus : std::uint8_t {
    Pending,
    Running,
    Succeeded,
    Failed,
    TimedOut,
    Skipped,
};

/// Result of a single hook execution
struct HookExecResult {
    std::string hook_id;
    std::string hook_name;
    HookExecStatus status;
    std::optional<std::string> output;
    std::optional<std::string> error;
    std::chrono::milliseconds duration{0};
};

// ============================================================
// Mode State (migrated from HooksConfigMenu.tsx ModeState)
// ============================================================

/// Navigation mode for the hooks config menu
struct ModeSelectEvent {};
struct ModeSelectMatcher { HookEvent event; };
struct ModeSelectHook { HookEvent event; std::string matcher; };
struct ModeViewHook { HookEvent event; IndividualHookConfig hook; };

using HooksMenuMode = std::variant<
    ModeSelectEvent,
    ModeSelectMatcher,
    ModeSelectHook,
    ModeViewHook
>;

// ============================================================
// Status Indicator Helpers
// ============================================================

/// Get icon/color for hook event
[[nodiscard]] inline std::pair<std::string, Color> hook_event_display(HookEvent ev) {
    switch (ev) {
        case HookEvent::PreToolUse:   return {"⟨", Color::Cyan};
        case HookEvent::PostToolUse:  return {"⟩", Color::Blue};
        case HookEvent::Notification: return {"🔔", Color::Yellow};
        case HookEvent::Stop:         return {"⏹", Color::Red};
        case HookEvent::SubagentStop: return {"⏹", Color::Magenta};
    }
    return {"?", Color::White};
}

/// Get hook event display name
[[nodiscard]] inline std::string hook_event_name(HookEvent ev) {
    switch (ev) {
        case HookEvent::PreToolUse:   return "PreToolUse";
        case HookEvent::PostToolUse:  return "PostToolUse";
        case HookEvent::Notification: return "Notification";
        case HookEvent::Stop:         return "Stop";
        case HookEvent::SubagentStop: return "SubagentStop";
    }
    return "Unknown";
}

/// Get icon/color for hook exec status
[[nodiscard]] inline std::pair<std::string, Color> hook_exec_status_display(HookExecStatus s) {
    switch (s) {
        case HookExecStatus::Pending:   return {"○", Color::GrayLight};
        case HookExecStatus::Running:   return {"◐", Color::Cyan};
        case HookExecStatus::Succeeded: return {"✓", Color::Green};
        case HookExecStatus::Failed:    return {"✗", Color::Red};
        case HookExecStatus::TimedOut:  return {"⏱", Color::Yellow};
        case HookExecStatus::Skipped:   return {"⊘", Color::GrayDark};
    }
    return {"?", Color::White};
}

/// Get hook type badge
[[nodiscard]] inline std::string hook_type_badge(HookType t) {
    switch (t) {
        case HookType::Command: return "cmd";
        case HookType::Prompt:  return "prompt";
        case HookType::Agent:   return "agent";
        case HookType::Http:    return "http";
    }
    return "?";
}

// ============================================================
// Rendering: Hook Execution Display
// ============================================================

/// Render a hook execution result row
[[nodiscard]] inline Element RenderHookExecRow(
    const HookExecResult& result, bool selected) {

    auto [icon, icon_color] = hook_exec_status_display(result.status);

    Elements parts = {
        text(" " + icon + " ") | color(icon_color),
        text(result.hook_name) | (selected ? bold : nothing) | color(Color::White),
        filler(),
        text(std::format(" {}ms ", result.duration.count())) | dim,
    };

    auto line = hbox(parts);

    // Show error if failed
    Elements result_parts = {line};
    if (result.error && result.status == HookExecStatus::Failed) {
        result_parts.push_back(hbox({
            text("   ") | dim,
            text("⚠ " + *result.error) | color(Color::Red) | dim,
        }));
    }

    // Show output preview if succeeded
    if (result.output && result.status == HookExecStatus::Succeeded
        && !result.output->empty()) {
        auto preview = result.output->substr(0, 80);
        result_parts.push_back(hbox({
            text("   ") | dim,
            text(preview) | dim | color(Color::GrayLight),
        }));
    }

    auto el = vbox(result_parts);
    if (selected) {
        el = el | bgcolor(Color::RGB(25, 30, 45));
    }
    return el;
}

// ============================================================
// Rendering: Hook Config Browser (SelectEventMode)
// ============================================================

/// Event summary for the event selection screen
struct HookEventSummary {
    HookEvent event;
    int matcher_count = 0;
    int hook_count = 0;
};

/// Render the event selection list
[[nodiscard]] inline Element RenderSelectEventMode(
    const std::vector<HookEventSummary>& events, int selected_index) {

    Elements items;
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        const auto& ev = events[i];
        auto [icon, ic] = hook_event_display(ev.event);
        bool sel = (i == selected_index);

        auto row = hbox({
            text(" " + icon + " ") | color(ic),
            text(hook_event_name(ev.event))
                | (sel ? bold : nothing) | color(Color::White),
            filler(),
            text(std::format(" {} matchers, {} hooks ",
                ev.matcher_count, ev.hook_count)) | dim,
        });

        if (sel) row = row | bgcolor(Color::RGB(25, 30, 45));
        items.push_back(row);
    }

    if (items.empty()) {
        items.push_back(text(" No hooks configured") | dim | center);
    }

    return vbox({
        text(" Hook Events ") | bold | color(Color::Blue),
        separator(),
        vbox(items) | vscroll_indicator | yframe | flex,
        separator(),
        hbox({
            text(" [Enter]") | color(Color::Cyan), text(" drill in "),
            text("[Esc]") | color(Color::Cyan), text(" close "),
        }) | dim,
    }) | borderRounded;
}

// ============================================================
// Rendering: Hook View Detail
// ============================================================

/// Render individual hook detail view
[[nodiscard]] inline Element RenderHookDetail(const IndividualHookConfig& hook) {
    Elements rows;

    rows.push_back(hbox({
        text(" Name: ") | bold,
        text(hook.name) | color(Color::White),
    }));
    rows.push_back(hbox({
        text(" Type: ") | bold,
        text(hook_type_badge(hook.type)) | color(Color::Cyan),
    }));
    rows.push_back(hbox({
        text(" Event: ") | bold,
        text(hook_event_name(hook.event)) | color(Color::Blue),
    }));
    rows.push_back(hbox({
        text(" Matcher: ") | bold,
        text(hook.matcher.tool_name.empty() ? "*" : hook.matcher.tool_name)
            | color(Color::Yellow),
    }));
    rows.push_back(hbox({
        text(" Enabled: ") | bold,
        text(hook.enabled ? "yes" : "no")
            | color(hook.enabled ? Color::Green : Color::Red),
    }));

    rows.push_back(separator());
    rows.push_back(text(" Content:") | bold);
    rows.push_back(paragraph(hook.command_or_content) | dim
        | size(WIDTH, LESS_THAN, 70));

    if (hook.timeout_ms) {
        rows.push_back(separator());
        rows.push_back(hbox({
            text(" Timeout: ") | bold,
            text(std::format("{}ms", *hook.timeout_ms)) | dim,
        }));
    }

    return vbox(rows) | borderRounded;
}

// ============================================================
// Full Hooks UI Options and Component
// ============================================================

/// Options for the hooks config menu
struct HooksUIOptions {
    std::vector<IndividualHookConfig> all_hooks;
    std::vector<std::string> tool_names; // Available tool names for matching
    HooksMenuMode mode;
    int selected_index = 0;

    std::function<void()> on_exit;
};

/// Compute event summaries from all hooks
[[nodiscard]] inline std::vector<HookEventSummary> compute_event_summaries(
    const std::vector<IndividualHookConfig>& hooks) {

    // Collect unique events
    std::vector<HookEvent> seen_events;
    for (const auto& h : hooks) {
        bool found = false;
        for (auto ev : seen_events) {
            if (ev == h.event) { found = true; break; }
        }
        if (!found) seen_events.push_back(h.event);
    }

    std::vector<HookEventSummary> result;
    for (auto ev : seen_events) {
        HookEventSummary summary;
        summary.event = ev;
        std::vector<std::string> matchers_seen;
        for (const auto& h : hooks) {
            if (h.event != ev) continue;
            ++summary.hook_count;
            bool matcher_seen = false;
            for (const auto& m : matchers_seen) {
                if (m == h.matcher.tool_name) { matcher_seen = true; break; }
            }
            if (!matcher_seen) {
                matchers_seen.push_back(h.matcher.tool_name);
                ++summary.matcher_count;
            }
        }
        result.push_back(summary);
    }
    return result;
}

/// Create the hooks config menu component
[[nodiscard]] inline Component HooksConfigMenu(HooksUIOptions options) {
    auto state = std::make_shared<HooksUIOptions>(std::move(options));

    return Renderer([state] {
        if (std::holds_alternative<ModeSelectEvent>(state->mode)) {
            auto summaries = compute_event_summaries(state->all_hooks);
            return RenderSelectEventMode(summaries, state->selected_index);
        }
        if (std::holds_alternative<ModeViewHook>(state->mode)) {
            auto& vh = std::get<ModeViewHook>(state->mode);
            return RenderHookDetail(vh.hook);
        }
        // Default: event selection
        auto summaries = compute_event_summaries(state->all_hooks);
        return RenderSelectEventMode(summaries, state->selected_index);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::Escape) {
            // Navigate back or exit
            if (std::holds_alternative<ModeSelectEvent>(state->mode)) {
                if (state->on_exit) state->on_exit();
            } else {
                state->mode = ModeSelectEvent{};
                state->selected_index = 0;
            }
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index = std::max(0, state->selected_index - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index++;
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::hooks_ui
