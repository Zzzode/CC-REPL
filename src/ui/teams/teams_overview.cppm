/// @file teams_overview.cppm
/// @brief Teams main overview view - team switcher header, member card grid
/// (avatar + status + role + last seen), and recent activity table.
/// Migrated from src/components/teams/ (TeamsDialog + TeamStatus) plus
/// teammate entries from src/components/agents/.
///
/// Reuses:
///   - UI13 Avatar/StatusDot/RoleTags (defined inline below, since they are
///     small lightweight display helpers - the real reusable variants live in
///     agent_view.cppm and team_status.cppm; see aliases at the top)
///   - Permissions StatusDot enum as inspiration (defined here with Team
///     member semantics: Online / Idle / Offline)
///
/// Engine logic is 100% delegated to utils/swarm + coordinator. This file
/// only mirrors state.  The actual "invite member", "change role", "remove
/// member" operations are hooks - they fire the supplied callbacks and let
/// the coordinator / team-discovery layer apply the mutation.
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
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.teams.teams_overview;

import cc.types.types;
import cc.ui.team_status;
import cc.ui.components.agent_view;  // Avatar/RoleTags helpers inspiration

export namespace cc::ui::teams::overview {
using namespace ftxui;

// Bring in reusable status primitives. Teams UI extends the TeammateStatus
// enum with extra members (Online/Idle/Offline) needed for the human-member
// grid.  The underlying coordinator-facing TeammateStatus values are
// convertible via the helpers below.
using CoordinationStatus = cc::ui::team_status::TeammateStatus;
using TeamRole = cc::ui::team_status::TeammateRole;

// ============================================================
// Shared Display Helpers (UI13 Avatar / StatusDot / RoleTags)
//
// These are defined locally as small pure functions, so this file can be
// consumed standalone.  They are functionally equivalent to the UI13
// versions (same glyphs, same color palette) and are kept in sync manually.
// ============================================================

/// Member presence status (extends TeammateStatus for human member display)
enum class MemberPresence : std::uint8_t {
    Online,   // 🟢
    Idle,     // 🟡
    Offline,  // 🔴
};

[[nodiscard]] inline Color PresenceColor(MemberPresence p) {
    switch (p) {
        case MemberPresence::Online:  return Color::Green;
        case MemberPresence::Idle:    return Color::Yellow;
        case MemberPresence::Offline: return Color::Red;
    }
    return Color::GrayDark;
}

/// Render a 3-cell status dot glyph.
[[nodiscard]] inline Element StatusDot(MemberPresence p) {
    return text("● ") | color(PresenceColor(p));
}

/// Team membership role (RBAC role, not the agent/worker role above)
enum class MembershipRole : std::uint8_t {
    Owner,
    Maintainer,
    Member,
    Guest,
};

[[nodiscard]] inline std::string_view RoleLabel(MembershipRole r) {
    switch (r) {
        case MembershipRole::Owner:      return "OWNER";
        case MembershipRole::Maintainer: return "MAINTAINER";
        case MembershipRole::Member:     return "MEMBER";
        case MembershipRole::Guest:      return "GUEST";
    }
    return "?";
}

[[nodiscard]] inline Color RoleColor(MembershipRole r) {
    switch (r) {
        case MembershipRole::Owner:      return Color::RedLight;
        case MembershipRole::Maintainer: return Color::Magenta;
        case MembershipRole::Member:     return Color::Cyan;
        case MembershipRole::Guest:      return Color::GrayLight;
    }
    return Color::GrayLight;
}

/// Render a 1-cell padded role tag badge.
[[nodiscard]] inline Element RoleTag(MembershipRole r) {
    return text(std::format("[{}]", RoleLabel(r))) | color(RoleColor(r)) | dim;
}

/// Viewer role (the logged-in user's view-level role in the team)
enum class ViewerRole : std::uint8_t {
    Owner,
    Member,
    Viewer,
};

[[nodiscard]] inline Element ViewerRoleBadge(ViewerRole r) {
    switch (r) {
        case ViewerRole::Owner:  return text("OWNER")  | bold | color(Color::RedLight) | bgcolor(Color::RGB(40,10,10));
        case ViewerRole::Member: return text("MEMBER") | bold | color(Color::Cyan)     | bgcolor(Color::RGB(10,30,40));
        case ViewerRole::Viewer: return text("VIEWER") | bold | color(Color::GrayLight)| bgcolor(Color::RGB(25,25,25));
    }
    return text("");
}

/// Render a 2x2 "avatar" block - first letter of name, with a colored
/// background depending on role.  Keeps a fixed footprint so grids align.
[[nodiscard]] inline Element AgentAvatar(std::string_view name, Color bg = Color::RGB(40, 50, 70)) {
    char first = name.empty() ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return hbox({
        text(std::string(1, first))
            | bold
            | color(Color::White)
            | bgcolor(bg)
            | center,
    }) | size(WIDTH, EQUAL, 3) | size(HEIGHT, EQUAL, 1);
}

/// Small shared helper: render "5m ago" style relative timestamps.
[[nodiscard]] inline std::string RelativeTime(std::chrono::seconds ago) {
    auto s = ago.count();
    if (s < 60)         return std::format("{}s ago", s);
    if (s < 3600)       return std::format("{}m ago", s / 60);
    if (s < 86400)      return std::format("{}h ago", s / 3600);
    return std::format("{}d ago", s / 86400);
}

// ============================================================
// Data Types
// ============================================================

/// A single team row (shown in the team switcher dropdown)
struct TeamSummary {
    std::string id;
    std::string name;
    int member_count = 0;
};

/// A member shown in the member grid
struct MemberCard {
    std::string id;
    std::string name;
    std::string handle;         // "@alice"
    MembershipRole role;
    MemberPresence presence;
    std::chrono::seconds last_seen{0}; // relative
    std::optional<std::string> avatar_color; // e.g. "cyan" → Color
};

/// Kind of activity item (used to pick icon / color)
enum class ActivityKind : std::uint8_t {
    Join,
    Leave,
    CreateAgent,
    Build,
    RoleChange,
    Invite,
    General,
};

/// A row in the recent-activity table
struct ActivityEntry {
    std::string id;
    std::chrono::seconds when{0};   // relative
    std::string actor_name;
    std::string actor_avatar_color;
    ActivityKind kind;
    std::string summary;            // "created agent foo"
    std::optional<std::string> detail; // extra one-line detail
};

[[nodiscard]] inline Color ActivityColor(ActivityKind k) {
    switch (k) {
        case ActivityKind::Join:        return Color::Green;
        case ActivityKind::Leave:       return Color::Red;
        case ActivityKind::CreateAgent: return Color::Cyan;
        case ActivityKind::Build:       return Color::Yellow;
        case ActivityKind::RoleChange:  return Color::Magenta;
        case ActivityKind::Invite:      return Color::BlueLight;
        case ActivityKind::General:     return Color::GrayLight;
    }
    return Color::GrayLight;
}

[[nodiscard]] inline std::string_view ActivityGlyph(ActivityKind k) {
    switch (k) {
        case ActivityKind::Join:        return "➕";
        case ActivityKind::Leave:       return "➖";
        case ActivityKind::CreateAgent: return "🤖";
        case ActivityKind::Build:       return "⚙";
        case ActivityKind::RoleChange:  return "🛡";
        case ActivityKind::Invite:      return "✉";
        case ActivityKind::General:     return "•";
    }
    return "•";
}

/// Full options for the TeamsOverview view
struct TeamsOverviewOptions {
    // --- Team context ---
    std::vector<TeamSummary> teams;
    int current_team_index = 0;
    ViewerRole viewer_role = ViewerRole::Member;

    // --- Members ---
    std::vector<MemberCard> members;
    int selected_member_index = 0;

    // --- Activity feed (last 10 rendered) ---
    std::vector<ActivityEntry> activity;

    // --- Search / filter state ---
    std::string member_search;

    // --- Callbacks (delegated to team-discovery / coordinator layer) ---
    std::function<void(int team_index)> on_switch_team;
    std::function<void(const MemberCard&)> on_invite;     // 'i' key triggers this callback
    std::function<void(const MemberCard&)> on_pm;
    std::function<void(const MemberCard&)> on_remove_member;
    std::function<void(const MemberCard&, MembershipRole)> on_change_role;
    std::function<void(const MemberCard&)> on_open_details; // Enter key
    std::function<void()> on_close;
    std::function<void(std::string_view pattern)> on_search_update;
};

// ============================================================
// Rendering
// ============================================================

/// Render the top bar: team switcher dropdown, role badge, member count,
/// [+ Invite] button.
[[nodiscard]] inline Element RenderHeaderBar(const TeamsOverviewOptions& opts) {
    const auto& team = opts.teams.empty()
        ? TeamSummary{"", "No team", 0}
        : opts.teams[opts.current_team_index];

    // Team selector (shown as "▼ <name>" hinting at dropdown)
    Element team_selector = hbox({
        text("▼ "),
        text(team.name) | bold | color(Color::Magenta),
    });
    if (opts.teams.size() > 1) {
        team_selector = hbox({
            team_selector,
            text(std::format(" (+{} more)", opts.teams.size() - 1)) | dim,
        });
    }

    // Members count
    auto count = text(std::format(" 👥 {} ", team.member_count))
        | color(Color::GrayLight);

    // Invite button (only shown for >= Member viewer role)
    Element invite_btn = text("");
    if (opts.viewer_role != ViewerRole::Viewer) {
        invite_btn = hbox({
            text(" [+Invite] ") | bold | color(Color::Green) | borderLight,
        });
    }

    return hbox({
        team_selector,
        text("  "),
        ViewerRoleBadge(opts.viewer_role),
        filler(),
        count,
        invite_btn,
    });
}

/// Render one member card (grid cell).
[[nodiscard]] inline Element RenderMemberCard(
    const MemberCard& m, bool selected, int card_width) {

    // Avatar
    Color bg = Color::RGB(40, 50, 70);
    if (m.avatar_color) {
        // crude mapping for well-known names - real implementation would use
        // a palette lookup; falls back to default
    }
    if (m.role == MembershipRole::Owner)       bg = Color::RGB(60, 20, 20);
    else if (m.role == MembershipRole::Maintainer) bg = Color::RGB(60, 20, 55);
    else if (m.role == MembershipRole::Guest)  bg = Color::RGB(35, 35, 40);

    Element avatar = AgentAvatar(m.name, bg);

    // Name + @handle
    Element name_line = hbox({
        text(m.name) | bold | color(selected ? Color::White : Color::GrayLight),
    });
    Element handle_line = hbox({
        text("@" + m.handle) | dim | color(Color::GrayDark),
    });

    // Status row
    Element status_row = hbox({
        StatusDot(m.presence),
        text(std::format("{} · {}",
            m.presence == MemberPresence::Online ? "Online" :
            m.presence == MemberPresence::Idle   ? "Idle"   : "Offline",
            RelativeTime(m.last_seen))) | dim,
    });

    // Role tag
    Element role = RoleTag(m.role);

    auto body = vbox({
        hbox({
            avatar,
            text(" "),
            vbox({name_line, handle_line}) | flex,
        }),
        status_row,
        hbox({role, filler()}),
    }) | borderLight;

    if (selected) {
        body = body | bgcolor(Color::RGB(25, 30, 48));
    }

    // Force each card to the same width so grid lines up.
    return body | size(WIDTH, EQUAL, card_width);
}

/// Render the member card grid.  4 columns when room permits.
[[nodiscard]] inline Element RenderMemberGrid(
    const TeamsOverviewOptions& opts, int avail_width) {

    // Apply search filter (case-insensitive substring match over name/handle)
    std::vector<int> visible;
    visible.reserve(opts.members.size());
    auto needle = opts.member_search;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (int i = 0; i < static_cast<int>(opts.members.size()); ++i) {
        if (needle.empty()) { visible.push_back(i); continue; }
        const auto& m = opts.members[i];
        std::string hay = m.name + " " + m.handle;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (hay.find(needle) != std::string::npos) visible.push_back(i);
    }

    if (visible.empty()) {
        return text(" No members match search") | dim | center | borderRounded;
    }

    // Pick column count (target 4; clamp to available width)
    constexpr int kMinCardWidth = 24;
    int cols = std::max(1, std::min(4, std::max(1, avail_width / kMinCardWidth)));
    // Recompute card width from actual column count
    int card_width = std::max(kMinCardWidth, (avail_width - cols) / cols);

    Elements rows;
    Elements current_row;

    // Map the caller-provided selected_member_index to the visible list
    int effective_selected = -1;
    for (int vi = 0; vi < static_cast<int>(visible.size()); ++vi) {
        if (visible[vi] == opts.selected_member_index) {
            effective_selected = vi;
            break;
        }
    }
    // If selected is outside the filter, fall back to first visible
    if (effective_selected == -1) effective_selected = 0;

    for (int vi = 0; vi < static_cast<int>(visible.size()); ++vi) {
        bool selected = (vi == effective_selected);
        auto cell = RenderMemberCard(
            opts.members[visible[vi]], selected, card_width);
        current_row.push_back(cell);
        if ((vi + 1) % cols == 0) {
            rows.push_back(hbox(current_row));
            current_row.clear();
        }
    }
    if (!current_row.empty()) {
        // pad with empty cells so the last row still respects grid width
        while (static_cast<int>(current_row.size()) < cols) {
            current_row.push_back(text("") | size(WIDTH, EQUAL, card_width));
        }
        rows.push_back(hbox(current_row));
    }

    // Header
    Element header = hbox({
        text(" Members ") | bold | color(Color::Cyan),
        text(std::format("({}/{})", visible.size(), opts.members.size())) | dim,
        opts.member_search.empty() ? text("") :
            text(std::format("  🔎 \"{}\"", opts.member_search)) | color(Color::Yellow),
        filler(),
        text("j/k select · Enter details · i invite · t team · / search") | dim,
    });

    return vbox({
        header,
        separator(),
        vbox(rows) | vscroll_indicator | yframe | flex,
    }) | borderRounded;
}

/// Render recent activity table (max 10 rows).
[[nodiscard]] inline Element RenderActivityTable(
    const TeamsOverviewOptions& opts) {

    Elements rows;
    int limit = std::min(10, static_cast<int>(opts.activity.size()));

    // Column layout: [Time | Avatar | Action | Detail]
    for (int i = 0; i < limit; ++i) {
        const auto& a = opts.activity[i];

        Element avatar = AgentAvatar(a.actor_name);
        Element time_cell = text(RelativeTime(a.when))
            | dim | color(Color::GrayDark)
            | size(WIDTH, EQUAL, 10);
        Element glyph = text(std::string{ActivityGlyph(a.kind)})
            | color(ActivityColor(a.kind));
        Element actor = hbox({
            text(a.actor_name) | bold,
            text(" "),
            text(a.summary) | color(ActivityColor(a.kind)),
        });
        Element detail = text("");
        if (a.detail) {
            detail = text("  " + *a.detail) | dim;
        }

        rows.push_back(hbox({
            time_cell,
            avatar,
            text(" "),
            glyph,
            text(" "),
            actor,
            detail,
            filler(),
        }));
    }

    if (rows.empty()) {
        rows.push_back(text(" No recent activity") | dim | center);
    }

    Element header = hbox({
        text(" Recent Activity ") | bold | color(Color::Cyan),
        text(std::format("(showing {} of {})", limit, opts.activity.size())) | dim,
        filler(),
    });

    return vbox({
        header,
        separator(),
        vbox(rows) | vscroll_indicator | yframe,
    }) | borderRounded;
}

/// Render the full teams overview view.
[[nodiscard]] inline Element RenderTeamsOverview(
    const TeamsOverviewOptions& opts, int avail_width = 120) {

    Element header = RenderHeaderBar(opts);
    Element members = RenderMemberGrid(opts, avail_width);
    Element activity = RenderActivityTable(opts);

    return vbox({
        header,
        separator(),
        members | yflex_shrink,
        activity | yflex_shrink,
    }) | borderHeavy;
}

// ============================================================
// Interactive Component
// ============================================================

/// Keybindings:
///   j/k          - move member selection
///   Enter        - open details for selected member
///   i            - trigger invite callback
///   t            - switch to next team (cycles)
///   /            - begin search (focus a search input; pattern forwarded)
///   Esc          - close
///
/// Search is modelled as a plain text field in the options struct.  We don't
/// open a modal here - we append/delete characters when the user types them
/// and forward the resulting string via `on_search_update`.
[[nodiscard]] inline Component MakeTeamsOverview(TeamsOverviewOptions options) {
    auto state = std::make_shared<TeamsOverviewOptions>(std::move(options));

    return Renderer([state] {
        return RenderTeamsOverview(*state);
    }) | CatchEvent([state](Event event) -> bool {

        // Escape always closes
        if (event == Event::Escape) {
            if (state->on_close) state->on_close();
            return true;
        }

        // Up / Down selection in the member grid
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_member_index =
                std::max(0, state->selected_member_index - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_member_index = std::min(
                static_cast<int>(state->members.size()) - 1,
                state->selected_member_index + 1);
            return true;
        }

        // Enter: open details for selected member
        if (event == Event::Return) {
            if (!state->members.empty() && state->on_open_details) {
                state->on_open_details(
                    state->members[state->selected_member_index]);
            }
            return true;
        }

        // 'i' - invite
        if (event == Event::Character('i')) {
            if (state->on_invite) {
                MemberCard blank;
                state->on_invite(blank);
            }
            return true;
        }

        // 't' - switch to next team (cycle)
        if (event == Event::Character('t')) {
            if (!state->teams.empty()) {
                int next = (state->current_team_index + 1) %
                           static_cast<int>(state->teams.size());
                state->current_team_index = next;
                if (state->on_switch_team) state->on_switch_team(next);
            }
            return true;
        }

        // '/' - search: for simplicity, we reset + switch to search mode.
        // We just capture alphanumeric / backspace directly until Esc.
        // (A full input widget is overkill here - the pattern is exposed via
        // on_search_update.)
        if (event == Event::Character('/')) {
            state->member_search.clear();
            if (state->on_search_update) state->on_search_update("");
            return true;
        }

        // Plain single-char input while search is "active" (non-empty or '/')
        // - append printable characters, backspace strips last char.
        if (!event.is_character()) {
            if (event == Event::Backspace) {
                if (!state->member_search.empty()) {
                    state->member_search.pop_back();
                    if (state->on_search_update)
                        state->on_search_update(state->member_search);
                }
                return true;
            }
            return false;
        }
        char c = event.character()[0];
        // Accept printable ASCII for search, except control chars
        if (c >= 0x20 && c < 0x7f && c != 'j' && c != 'k' && c != 'i'
            && c != 't' && c != '/' && c != 'q') {
            state->member_search.push_back(c);
            if (state->on_search_update)
                state->on_search_update(state->member_search);
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::teams::overview
