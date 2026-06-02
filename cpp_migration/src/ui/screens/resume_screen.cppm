/// @file resume_screen.cppm
/// @brief Conversation resume screen with session picker and preview panel.
/// Migrated from src/screens/ResumeConversation.tsx
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.resume_screen;

export namespace cc::ui::resume_screen {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// A single session log entry for the picker
struct SessionLogEntry {
    std::string session_id;
    std::string title;
    std::string project_path;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_active_at;
    int message_count = 0;
    int turn_count = 0;
    std::optional<double> total_cost_usd;
    bool is_current_project = true;
    bool has_custom_title = false;

    // Preview data (loaded on selection)
    std::vector<std::string> message_previews;
};

/// Search/filter configuration for sessions
struct SessionSearchConfig {
    std::string query;
    bool current_project_only = true;
    bool sort_by_recency = true;
};

/// Resume mode for how to handle the resumed session
enum class ResumeMode : std::uint8_t {
    Continue,       // Continue from where left off
    Branch,         // Branch from a specific message
    ViewOnly,       // Just view the conversation
};

/// Result of selecting a session to resume
struct ResumeSelection {
    std::string session_id;
    ResumeMode mode;
    std::optional<int> branch_from_turn; // For Branch mode
};

// ============================================================
// Rendering: Session Entry
// ============================================================

/// Format a time_point as relative time string
[[nodiscard]] inline std::string format_relative_time(
    std::chrono::system_clock::time_point tp) {
    auto now = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::minutes>(now - tp);

    if (diff.count() < 1) return "just now";
    if (diff.count() < 60) return std::format("{}m ago", diff.count());

    auto hours = std::chrono::duration_cast<std::chrono::hours>(diff);
    if (hours.count() < 24) return std::format("{}h ago", hours.count());

    auto days = hours.count() / 24;
    if (days < 7) return std::format("{}d ago", days);
    if (days < 30) return std::format("{}w ago", days / 7);

    return std::format("{}mo ago", days / 30);
}

/// Render a session entry row
[[nodiscard]] inline Element RenderSessionRow(
    const SessionLogEntry& session, bool selected, bool show_project) {

    // Title (or generate from session_id if no custom title)
    std::string display_title = session.title.empty()
        ? ("Session " + session.session_id.substr(0, 8))
        : session.title;

    Elements parts = {
        text(selected ? " ❯ " : "   ") | color(selected ? Color{Color::Green} : Color{Color::Default}),
        text(display_title)
            | (selected ? bold : nothing)
            | color(Color::White)
            | size(WIDTH, LESS_THAN, 50),
    };

    parts.push_back(filler());

    // Message count
    parts.push_back(
        text(std::format(" {}msg ", session.message_count)) | dim);

    // Cost
    if (session.total_cost_usd) {
        parts.push_back(
            text(std::format("${:.2f} ", *session.total_cost_usd))
                | dim | color(Color::Green));
    }

    // Relative time
    parts.push_back(
        text(format_relative_time(session.last_active_at))
            | dim | color(Color::GrayLight));

    // Project indicator (if showing all projects)
    if (show_project && !session.is_current_project) {
        parts.push_back(text(" ") | dim);
        parts.push_back(text("↗") | dim | color(Color::Yellow));
    }

    parts.push_back(text(" ") | dim);

    auto row = hbox(parts);
    if (selected) {
        row = row | bgcolor(Color::RGB(25, 30, 45));
    }
    return row;
}

// ============================================================
// Rendering: Preview Panel
// ============================================================

/// Render the session preview panel (right side)
[[nodiscard]] inline Element RenderPreviewPanel(const SessionLogEntry& session) {
    Elements parts;

    // Header
    parts.push_back(hbox({
        text(" Preview ") | bold | color(Color::Blue),
        text("(" + session.session_id.substr(0, 8) + ")") | dim,
    }));
    parts.push_back(separator());

    // Metadata
    parts.push_back(hbox({
        text(" Project: ") | dim,
        text(session.project_path) | color(Color::Cyan),
    }));
    parts.push_back(hbox({
        text(" Messages: ") | dim,
        text(std::to_string(session.message_count)) | color(Color::White),
        text("  Turns: ") | dim,
        text(std::to_string(session.turn_count)) | color(Color::White),
    }));

    parts.push_back(separator());

    // Message previews
    if (session.message_previews.empty()) {
        parts.push_back(text(" (no preview available)") | dim | center);
    } else {
        for (const auto& preview : session.message_previews) {
            parts.push_back(paragraph(" " + preview) | dim
                | size(WIDTH, LESS_THAN, 60));
        }
    }

    return vbox(parts) | borderLight | size(WIDTH, EQUAL, 50);
}

// ============================================================
// Rendering: Search Bar
// ============================================================

/// Render the search/filter bar
[[nodiscard]] inline Element RenderSearchBar(
    const SessionSearchConfig& config, bool is_focused) {

    Elements parts = {
        text(is_focused ? " / " : " 🔍 ") | color(Color::Cyan),
    };

    if (config.query.empty()) {
        parts.push_back(text("Search sessions...") | dim);
    } else {
        parts.push_back(text(config.query) | color(Color::White));
    }

    parts.push_back(filler());

    // Filter indicators
    if (config.current_project_only) {
        parts.push_back(text(" [project] ") | dim | color(Color::Yellow));
    } else {
        parts.push_back(text(" [all] ") | dim | color(Color::GrayLight));
    }

    return hbox(parts);
}

// ============================================================
// Full Resume Screen
// ============================================================

/// Options for the resume screen
struct ResumeScreenOptions {
    std::vector<SessionLogEntry> sessions;
    SessionSearchConfig search;
    int selected_index = 0;
    bool show_preview = true;
    bool loading = false;

    std::function<void(ResumeSelection)> on_select;
    std::function<void()> on_cancel;
    std::function<void(SessionSearchConfig)> on_search_change;
};

/// Apply search filter to sessions
[[nodiscard]] inline std::vector<const SessionLogEntry*> filter_sessions(
    const std::vector<SessionLogEntry>& sessions,
    const SessionSearchConfig& search) {

    std::vector<const SessionLogEntry*> result;
    for (const auto& s : sessions) {
        if (search.current_project_only && !s.is_current_project) continue;
        if (!search.query.empty()) {
            // Simple case-insensitive substring match
            bool found = s.title.find(search.query) != std::string::npos
                      || s.session_id.find(search.query) != std::string::npos
                      || s.project_path.find(search.query) != std::string::npos;
            if (!found) continue;
        }
        result.push_back(&s);
    }
    return result;
}

/// Render the full resume screen
[[nodiscard]] inline Element RenderResumeScreen(const ResumeScreenOptions& opts) {
    if (opts.loading) {
        return vbox({
            text("") | dim,
            text(" Loading sessions...") | color(Color::Cyan),
            text("") | dim,
        }) | borderRounded;
    }

    auto filtered = filter_sessions(opts.sessions, opts.search);

    // Left panel: session list
    Elements list_items;
    for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
        list_items.push_back(
            RenderSessionRow(*filtered[i], i == opts.selected_index,
                            !opts.search.current_project_only));
    }

    if (list_items.empty()) {
        list_items.push_back(
            text(" No sessions found") | dim | center);
    }

    auto left_panel = vbox({
        hbox({
            text(" Resume Conversation ") | bold | color(Color::Green),
            text(std::format("({} sessions)", filtered.size())) | dim,
            filler(),
        }),
        separator(),
        RenderSearchBar(opts.search, false),
        separator(),
        vbox(list_items) | vscroll_indicator | yframe | flex,
        separator(),
        hbox({
            text(" [Enter]") | color(Color::Cyan), text(" resume "),
            text("[/]") | color(Color::Cyan), text(" search "),
            text("[p]") | color(Color::Cyan), text("roject toggle "),
            text("[Tab]") | color(Color::Cyan), text(" preview "),
            text("[Esc]") | color(Color::Cyan), text(" cancel"),
        }) | dim,
    }) | borderRounded | flex;

    // Right panel: preview (if visible and a session is selected)
    if (opts.show_preview && !filtered.empty()
        && opts.selected_index < static_cast<int>(filtered.size())) {
        auto preview = RenderPreviewPanel(*filtered[opts.selected_index]);
        return hbox({left_panel, preview});
    }

    return left_panel;
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the resume screen component
[[nodiscard]] inline Component ResumeScreen(ResumeScreenOptions options) {
    auto state = std::make_shared<ResumeScreenOptions>(std::move(options));

    return Renderer([state] {
        return RenderResumeScreen(*state);
    }) | CatchEvent([state](Event event) -> bool {
        auto filtered = filter_sessions(state->sessions, state->search);
        int count = static_cast<int>(filtered.size());

        if (event == Event::Escape) {
            if (state->on_cancel) state->on_cancel();
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
                state->on_select(ResumeSelection{
                    .session_id = filtered[state->selected_index]->session_id,
                    .mode = ResumeMode::Continue,
                    .branch_from_turn = std::nullopt,
                });
            }
            return true;
        }
        if (event == Event::Character('p')) {
            // Toggle project filter
            state->search.current_project_only = !state->search.current_project_only;
            state->selected_index = 0;
            if (state->on_search_change)
                state->on_search_change(state->search);
            return true;
        }
        if (event == Event::Tab) {
            state->show_preview = !state->show_preview;
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::resume_screen
