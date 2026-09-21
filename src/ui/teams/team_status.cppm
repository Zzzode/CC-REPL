/// @file team_status.cppm
/// @brief Team member status display, coordinator agent status, teammate view header.
/// Migrated from src/components/teams/, TeammateViewHeader.tsx, CoordinatorAgentStatus.tsx
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.team_status;

export namespace cc::ui::team_status {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Teammate connection status
enum class TeammateStatus : std::uint8_t {
    Active,         // Working normally
    Idle,           // Connected but idle
    AwaitingApproval, // Needs plan approval
    Stopping,       // Shutdown requested
    Disconnected,   // Not connected
    Error,          // In error state
};

/// Role of a teammate
enum class TeammateRole : std::uint8_t {
    Worker,
    Coordinator,
    TeamLead,
};

/// A single teammate entry
struct TeammateEntry {
    std::string id;
    std::string name;
    TeammateRole role;
    TeammateStatus status;
    std::optional<std::string> current_activity;
    std::optional<double> progress;         // 0.0 - 1.0
    std::chrono::steady_clock::time_point joined_at;
    std::optional<std::string> agent_color; // Color name for badge
};

/// Coordinator agent state
struct CoordinatorState {
    std::string agent_name;
    TeammateStatus status;
    int active_workers = 0;
    int total_workers = 0;
    std::optional<std::string> current_plan;
    std::vector<std::string> pending_approvals;
};

/// Team context overview
struct TeamContext {
    std::optional<CoordinatorState> coordinator;
    std::vector<TeammateEntry> teammates;
    std::string team_name;
};

// ============================================================
// Status Display Helpers
// ============================================================

/// Get icon for teammate status
[[nodiscard]] inline std::pair<std::string, Color> teammate_status_display(TeammateStatus status) {
    switch (status) {
        case TeammateStatus::Active:           return {"▶", Color::Green};
        case TeammateStatus::Idle:             return {"…", Color::GrayLight};
        case TeammateStatus::AwaitingApproval: return {"?", Color::Yellow};
        case TeammateStatus::Stopping:         return {"⚠", Color::Yellow};
        case TeammateStatus::Disconnected:     return {"○", Color::GrayDark};
        case TeammateStatus::Error:            return {"✗", Color::Red};
    }
    return {"?", Color::White};
}

/// Get role badge text
[[nodiscard]] inline std::string role_badge(TeammateRole role) {
    switch (role) {
        case TeammateRole::Worker:      return "worker";
        case TeammateRole::Coordinator: return "coord";
        case TeammateRole::TeamLead:    return "lead";
    }
    return "?";
}

// ============================================================
// Teammate View Header (migrated from TeammateViewHeader.tsx)
// ============================================================

/// Options for the teammate view header
struct TeammateViewHeaderOptions {
    std::string team_name;
    int total_teammates = 0;
    int active_count = 0;
    bool is_selected = false;
    bool show_hint = false;
};

/// Render the teammate view header (footer status indicator)
[[nodiscard]] inline Element RenderTeammateViewHeader(
    const TeammateViewHeaderOptions& opts) {

    if (opts.total_teammates == 0) {
        return text("");
    }

    std::string status_text = std::format("{} {}",
        opts.total_teammates,
        opts.total_teammates == 1 ? "teammate" : "teammates");

    Elements parts = {
        text(status_text)
            | color(Color::GrayLight)
            | (opts.is_selected ? inverted : nothing),
    };

    if (opts.show_hint && opts.is_selected) {
        parts.push_back(text(" · ") | dim);
        parts.push_back(text("Enter to view") | dim);
    }

    return hbox(parts);
}

// ============================================================
// Coordinator Agent Status (migrated from CoordinatorAgentStatus.tsx)
// ============================================================

/// Render the coordinator agent status panel
[[nodiscard]] inline Element RenderCoordinatorStatus(const CoordinatorState& coord) {
    auto [icon, icon_color] = teammate_status_display(coord.status);

    Elements header_parts = {
        text(" " + icon + " ") | color(icon_color),
        text(coord.agent_name) | bold | color(Color::Magenta),
        text(" (coordinator)") | dim,
        filler(),
        text(std::format(" {}/{} workers ",
            coord.active_workers, coord.total_workers)) | dim,
    };

    Elements body_parts;

    // Current plan
    if (coord.current_plan) {
        body_parts.push_back(hbox({
            text("  Plan: ") | dim,
            text(*coord.current_plan) | color(Color::Cyan),
        }));
    }

    // Pending approvals
    if (!coord.pending_approvals.empty()) {
        body_parts.push_back(hbox({
            text(std::format("  ⚠ {} pending approval(s)",
                coord.pending_approvals.size())) | color(Color::Yellow),
        }));
    }

    Elements all = {hbox(header_parts)};
    for (auto& bp : body_parts) {
        all.push_back(std::move(bp));
    }

    return vbox(all) | borderLight;
}

// ============================================================
// Team Status Display
// ============================================================

/// Options for the full team status panel
struct TeamStatusOptions {
    TeamContext context;
    int selected_index = 0;
    bool expanded = false;

    std::function<void(const std::string& teammate_id)> on_select;
    std::function<void(const std::string& teammate_id)> on_kill;
    std::function<void()> on_close;
};

/// Render a single teammate row
[[nodiscard]] inline Element RenderTeammateRow(
    const TeammateEntry& tm, bool selected) {

    auto [icon, icon_color] = teammate_status_display(tm.status);

    Elements parts = {
        text(" " + icon + " ") | color(icon_color),
        text(tm.name) | (selected ? bold : nothing) | color(Color::White),
        text(" ") | dim,
        text("[" + role_badge(tm.role) + "]") | dim | color(Color::Blue),
    };

    if (tm.current_activity) {
        parts.push_back(text(" · ") | dim);
        parts.push_back(text(*tm.current_activity) | dim | color(Color::Cyan));
    }

    parts.push_back(filler());

    auto line = hbox(parts);

    // Progress bar if available
    Elements result_parts = {line};
    if (tm.progress && tm.status == TeammateStatus::Active) {
        result_parts.push_back(hbox({
            text("   ") | dim,
            gauge(*tm.progress) | color(Color::Green) | size(WIDTH, LESS_THAN, 25),
            text(std::format(" {:.0f}%", *tm.progress * 100)) | dim,
        }));
    }

    auto result = vbox(result_parts);
    if (selected) {
        result = result | bgcolor(Color::RGB(25, 30, 45));
    }
    return result;
}

/// Render the full team status panel
[[nodiscard]] inline Element RenderTeamStatus(const TeamStatusOptions& opts) {
    Elements sections;

    // Team title
    sections.push_back(hbox({
        text(" 👥 ") | dim,
        text(opts.context.team_name) | bold | color(Color::Magenta),
        text(std::format(" ({} members)",
            opts.context.teammates.size())) | dim,
        filler(),
    }));

    sections.push_back(separator());

    // Coordinator status
    if (opts.context.coordinator) {
        sections.push_back(RenderCoordinatorStatus(*opts.context.coordinator));
        sections.push_back(separator());
    }

    // Teammate list
    Elements teammate_els;
    for (int i = 0; i < static_cast<int>(opts.context.teammates.size()); ++i) {
        const auto& tm = opts.context.teammates[i];
        // Skip team-lead in display (shown as coordinator)
        if (tm.role == TeammateRole::TeamLead) continue;
        teammate_els.push_back(RenderTeammateRow(tm, i == opts.selected_index));
    }

    if (teammate_els.empty()) {
        teammate_els.push_back(text(" No active teammates") | dim | center);
    }

    sections.push_back(
        vbox(teammate_els) | vscroll_indicator | yframe | flex);

    // Action bar
    sections.push_back(separator());
    sections.push_back(hbox({
        text(" [Enter]") | color(Color::Cyan), text(" view "),
        text("[k]") | color(Color::Cyan), text("ill "),
        text("[Esc]") | color(Color::Cyan), text(" close"),
    }) | dim);

    return vbox(sections) | borderRounded;
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a team status component
[[nodiscard]] inline Component TeamStatusPanel(TeamStatusOptions options) {
    auto state = std::make_shared<TeamStatusOptions>(std::move(options));

    return Renderer([state] {
        return RenderTeamStatus(*state);
    }) | CatchEvent([state](Event event) -> bool {
        int count = static_cast<int>(state->context.teammates.size());

        if (event == Event::Escape) {
            if (state->on_close) state->on_close();
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index = std::max(0, state->selected_index - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index = std::min(count - 1, state->selected_index + 1);
            return true;
        }
        if (event == Event::Return) {
            if (count > 0 && state->on_select) {
                state->on_select(
                    state->context.teammates[state->selected_index].id);
            }
            return true;
        }
        if (event == Event::Character('K')) {
            if (count > 0 && state->on_kill) {
                state->on_kill(
                    state->context.teammates[state->selected_index].id);
            }
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::team_status
