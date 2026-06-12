/// @file team_details_dialog.cppm
/// @brief Team detail dialog with 4 tabs:
///   Tab 1 Members      - member table (role dropdown, status, join date)
///   Tab 2 Activity     - full timeline with filter + time-range
///   Tab 3 Permissions  - RBAC role matrix (read-only + edit mode)
///   Tab 4 Billing/Integrations - placeholder (reuses UI3 usage_dialog)
///
/// Reuses:
///   - cc.ui.teams.teams_overview (MemberPresence / MembershipRole /
///     RoleTag / AgentAvatar / RelativeTime helpers)
///   - cc.ui.dialogs.usage_dialog (UsageSnapshot struct; rendering is NOT
///     imported to avoid a hard link cycle - callers provide a pre-rendered
///     Element via options; annotated import note below)
///   - cc.ui.components.agent_view  (state helpers)
///
/// Engine logic delegated to the coordinator / permission engine - the
/// dialog only exposes callbacks and reflects their state back via the
/// caller repopulating `options`.
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
#include <array>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.teams.team_details_dialog;

import cc.types.types;
import cc.ui.team_status;
import cc.ui.teams.teams_overview; // Member/Activity/avatar helpers
import cc.ui.components.agent_view;
// Note: import cc.ui.dialogs.usage_dialog would be ideal, but we keep this
// file standalone.  The caller pre-renders the usage stats Element and
// passes it via `usage_stats_element` if they want real numbers.

export namespace cc::ui::teams::details {
using namespace ftxui;

// Pull in display helpers from the overview module.
using MemberPresence = cc::ui::teams::overview::MemberPresence;
using MembershipRole = cc::ui::teams::overview::MembershipRole;
using ActivityKind   = cc::ui::teams::overview::ActivityKind;
using ActivityEntry  = cc::ui::teams::overview::ActivityEntry;
using MemberCard     = cc::ui::teams::overview::MemberCard;
using ViewerRole    = cc::ui::teams::overview::ViewerRole;
using cc::ui::teams::overview::PresenceColor;
using cc::ui::teams::overview::StatusDot;
using cc::ui::teams::overview::RoleColor;
using cc::ui::teams::overview::RoleLabel;
using cc::ui::teams::overview::RoleTag;
using cc::ui::teams::overview::AgentAvatar;
using cc::ui::teams::overview::RelativeTime;
using cc::ui::teams::overview::ActivityColor;
using cc::ui::teams::overview::ActivityGlyph;
using cc::ui::teams::overview::ViewerRoleBadge;

// ============================================================
// Tab selection
// ============================================================

enum class Tab : std::uint8_t {
    Members = 0,
    Activity,
    Permissions,
    Billing,
    _Count,
};

[[nodiscard]] inline std::string_view TabLabel(Tab t) {
    switch (t) {
        case Tab::Members:     return "Members";
        case Tab::Activity:    return "Activity";
        case Tab::Permissions: return "Permissions";
        case Tab::Billing:     return "Billing / Integrations";
        default: return "?";
    }
}

[[nodiscard]] inline Element TabBar(Tab active) {
    Elements parts;
    auto push = [&](Tab t) {
        bool sel = (t == active);
        auto el = text(std::format(" {} ", TabLabel(t)));
        if (sel) el = el | bold | color(Color::White)
                    | bgcolor(Color::Cyan) | inverted;
        else     el = el | dim;
        parts.push_back(el);
    };
    push(Tab::Members);
    push(Tab::Activity);
    push(Tab::Permissions);
    push(Tab::Billing);
    return hbox(parts) | size(WIDTH, GREATER_THAN, 40);
}

// ============================================================
// Members Tab
// ============================================================

struct MembersTableRow : MemberCard {
    std::chrono::seconds joined_at{0};  // relative age (days)
    std::chrono::seconds last_active{0};
};

/// Activity filter enum (used for Tab 2)
enum class ActivityFilter : std::uint8_t {
    All = 0,
    AgentOps,
    MemberOps,
    Billing,
};

/// Time range for the activity tab
enum class TimeRange : std::uint8_t {
    Today = 0,
    Last7d,
    Last30d,
};

// ============================================================
// Permissions Tab (RBAC)
// ============================================================

/// Well-known capabilities, displayed as rows in the matrix.
enum class Capability : std::uint8_t {
    ViewTeam = 0,
    InviteMembers,
    RemoveMembers,
    ChangeRoles,
    CreateAgents,
    RunBash,
    ManageSecrets,
    EditBilling,
    ViewAuditLog,
    DeleteTeam,
    _Count,
};

[[nodiscard]] inline std::string_view CapabilityLabel(Capability c) {
    switch (c) {
        case Capability::ViewTeam:      return "View team & members";
        case Capability::InviteMembers: return "Invite members";
        case Capability::RemoveMembers: return "Remove members";
        case Capability::ChangeRoles:   return "Change member roles";
        case Capability::CreateAgents:  return "Create agents / swarms";
        case Capability::RunBash:       return "Execute shell commands";
        case Capability::ManageSecrets: return "Manage secrets / vault";
        case Capability::EditBilling:   return "Edit billing & plan";
        case Capability::ViewAuditLog:  return "View audit log";
        case Capability::DeleteTeam:    return "Delete team";
        default: return "?";
    }
}

// The roles that form the matrix columns
constexpr std::array<MembershipRole, 4> kRoles = {
    MembershipRole::Owner,
    MembershipRole::Maintainer,
    MembershipRole::Member,
    MembershipRole::Guest,
};

/// Hard-coded default matrix (read-only).  In edit-mode the caller can
/// override any cell via `matrix_overrides`.
[[nodiscard]] inline bool DefaultPermission(MembershipRole r, Capability c) {
    // Owner: everything
    if (r == MembershipRole::Owner) return true;
    // Guest: view only
    if (r == MembershipRole::Guest) return (c == Capability::ViewTeam);
    // Member: create agents, run bash, manage secrets, view audit, view team
    switch (c) {
        case Capability::ViewTeam:
        case Capability::CreateAgents:
        case Capability::RunBash:
        case Capability::ManageSecrets:
        case Capability::ViewAuditLog:
            return true;
        default:
            return false;
    }
    // (Maintainer - all except billing/delete-team)
    if (r == MembershipRole::Maintainer) {
        return (c != Capability::EditBilling && c != Capability::DeleteTeam);
    }
    return false;
}

struct MatrixOverrideKey {
    MembershipRole role;
    Capability cap;
    bool operator==(const MatrixOverrideKey&) const = default;
};

struct PermissionsMatrix {
    // When false the matrix is read-only.
    bool edit_mode = false;
    // Non-empty cells override the built-in default.
    std::vector<std::pair<MatrixOverrideKey, bool>> overrides;

    [[nodiscard]] bool lookup(MembershipRole r, Capability c) const {
        MatrixOverrideKey k{r, c};
        for (const auto& o : overrides) {
            if (o.first == k) return o.second;
        }
        return DefaultPermission(r, c);
    }

    void set(MembershipRole r, Capability c, bool v) {
        MatrixOverrideKey k{r, c};
        for (auto& o : overrides) {
            if (o.first == k) { o.second = v; return; }
        }
        overrides.emplace_back(k, v);
    }
};

// ============================================================
// Billing / Integrations Tab
// ============================================================

/// One integration row
struct IntegrationRow {
    std::string name;       // "Slack", "GitHub", "Jira"
    std::string glyph;      // icon
    bool connected = false;
    std::optional<std::string> account; // e.g. "acme.slack.com"
};

// ============================================================
// Full dialog options
// ============================================================

struct TeamDetailsDialogOptions {
    // --- Header ---
    std::string team_id;
    std::string team_name;
    ViewerRole viewer_role = ViewerRole::Member;
    int member_count = 0;

    // --- Tab state ---
    Tab active_tab = Tab::Members;

    // --- Tab 1: Members ---
    std::vector<MembersTableRow> members;
    int selected_member = 0;
    // Header actions row (top-right of members tab):
    bool show_invite_btn   = true;
    bool show_export_btn   = true;
    bool show_settings_btn = true;

    // --- Tab 2: Activity ---
    std::vector<ActivityEntry> activity;
    ActivityFilter activity_filter = ActivityFilter::All;
    TimeRange time_range = TimeRange::Today;

    // --- Tab 3: Permissions ---
    PermissionsMatrix matrix;
    int selected_capability_row = 0;  // keyboard cursor for edit-mode toggle

    // --- Tab 4: Billing / Integrations ---
    // Pre-rendered usage stats element (may be empty).  Callers who wish to
    // use cc.ui.dialogs.usage_dialog can pass the rendered subtree here.
    Element usage_stats_element;
    std::vector<IntegrationRow> integrations;

    // --- Callbacks ---
    std::function<void(Tab)> on_switch_tab;
    std::function<void(const MembersTableRow&)> on_invite_member;
    std::function<void()> on_export_csv;
    std::function<void()> on_team_settings;
    std::function<void(const MembersTableRow&, MembershipRole new_role)> on_change_member_role;
    std::function<void(const MembersTableRow&)> on_remove_member;
    std::function<void(ActivityFilter)> on_change_filter;
    std::function<void(TimeRange)> on_change_range;
    std::function<void(bool)> on_toggle_edit_mode;
    std::function<void(MembershipRole, Capability, bool /*new_val*/)> on_toggle_permission;
    std::function<void()> on_create_custom_role;
    std::function<void(const IntegrationRow&)> on_configure_integration;
    std::function<void(IntegrationRow&, bool)> on_toggle_integration;
    std::function<void()> on_close;
};

// ============================================================
// Rendering
// ============================================================

/// Team header block: avatar block (team initial) + name + count + gear.
[[nodiscard]] inline Element RenderHeader(const TeamDetailsDialogOptions& opts) {
    char initial = opts.team_name.empty() ? '?' :
        static_cast<char>(std::toupper(static_cast<unsigned char>(opts.team_name[0])));

    Element team_avatar = hbox({
        text(std::format(" {} ", initial))
            | bold | color(Color::White)
            | bgcolor(Color::RGB(80, 30, 90))
            | center,
    }) | size(WIDTH, EQUAL, 4);

    Element header = hbox({
        team_avatar,
        text(" "),
        vbox({
            hbox({
                text(opts.team_name) | bold | color(Color::Magenta),
                text(" "),
                ViewerRoleBadge(opts.viewer_role),
            }),
            text(std::format("{} members", opts.member_count)) | dim,
        }) | flex,
        filler(),
        text(" ⚙ Settings ") | dim | borderLight, // placeholder button
    });

    return vbox({header, separator(), TabBar(opts.active_tab)});
}

// ----------------------- Tab 1: Members -----------------------

[[nodiscard]] inline Element RenderMembersTab(
    const TeamDetailsDialogOptions& opts) {

    // Action buttons row
    Elements btns;
    if (opts.show_invite_btn) {
        btns.push_back(text(" [Invite] ") | bold | color(Color::Green)
                       | borderLight);
        btns.push_back(text(" "));
    }
    if (opts.show_export_btn) {
        btns.push_back(text(" [Export CSV] ") | color(Color::Cyan)
                       | borderLight);
        btns.push_back(text(" "));
    }
    if (opts.show_settings_btn) {
        btns.push_back(text(" [Settings] ") | color(Color::Yellow)
                       | borderLight);
    }
    Element action_row = hbox({
        hbox(btns),
        filler(),
        text(" r cycle role · R remove") | dim,
    });

    // Table header
    Element header_row = hbox({
        text("   ")      | size(WIDTH, EQUAL, 5),  // avatar/selection
        text(" Name ")   | bold | size(WIDTH, EQUAL, 18),
        text(" Role ")   | bold | size(WIDTH, EQUAL, 12),
        text(" Status ") | bold | size(WIDTH, EQUAL, 9),
        text(" Joined ") | bold | size(WIDTH, EQUAL, 10),
        text(" Last active ") | bold,
    }) | bgcolor(Color::RGB(25, 30, 45));

    Elements rows;
    rows.push_back(header_row);
    rows.push_back(separator());

    for (int i = 0; i < static_cast<int>(opts.members.size()); ++i) {
        const auto& m = opts.members[i];
        bool selected = (i == opts.selected_member);

        // Avatar + selection indicator
        Element marker = text(selected ? " ► " : "   ")
            | color(selected ? Color::Cyan : Color::GrayDark);
        Element avatar = AgentAvatar(m.name);

        // Name + handle
        Element name = vbox({
            text(m.name) | (selected ? bold : nothing)
                | color(selected ? Color::White : Color::GrayLight),
            text("@" + m.handle) | dim | color(Color::GrayDark),
        });

        // Role (shown as editable dropdown-like badge; when selected the 'r'
        // key cycles through all possible values via on_change_member_role)
        Element role = RoleTag(m.role);
        if (selected) role = role | inverted | color(Color::Cyan);

        // Status
        Element status = hbox({
            StatusDot(m.presence),
            text(m.presence == MemberPresence::Online ? "Online " :
                 m.presence == MemberPresence::Idle   ? "Idle "   : "Offline")
                | dim,
        });

        // Joined
        Element joined = text(RelativeTime(m.joined_at)) | dim;

        // Last active
        Element last_active = text(RelativeTime(m.last_active)) | dim;

        Element row = hbox({
            marker, avatar, text(" "),
            name        | size(WIDTH, EQUAL, 18),
            role        | size(WIDTH, EQUAL, 12),
            status      | size(WIDTH, EQUAL, 9),
            joined      | size(WIDTH, EQUAL, 10),
            last_active,
            filler(),
        });
        if (selected) row = row | bgcolor(Color::RGB(25, 30, 48));
        rows.push_back(row);
    }

    if (opts.members.empty()) {
        rows.push_back(text(" No members") | dim | center);
    }

    return vbox({
        action_row,
        separator(),
        vbox(rows) | vscroll_indicator | yframe | flex,
    }) | borderLight;
}

// ----------------------- Tab 2: Activity -----------------------

[[nodiscard]] inline std::string_view FilterLabel(ActivityFilter f) {
    switch (f) {
        case ActivityFilter::All:      return "All";
        case ActivityFilter::AgentOps: return "Agent ops";
        case ActivityFilter::MemberOps:return "Member ops";
        case ActivityFilter::Billing:  return "Billing";
    }
    return "?";
}
[[nodiscard]] inline std::string_view RangeLabel(TimeRange t) {
    switch (t) {
        case TimeRange::Today:   return "Today";
        case TimeRange::Last7d:  return "7d";
        case TimeRange::Last30d: return "30d";
    }
    return "?";
}

[[nodiscard]] inline bool ActivityMatches(
    const ActivityEntry& a, ActivityFilter f) {
    switch (f) {
        case ActivityFilter::All: return true;
        case ActivityFilter::AgentOps:
            return a.kind == ActivityKind::CreateAgent ||
                   a.kind == ActivityKind::Build;
        case ActivityFilter::MemberOps:
            return a.kind == ActivityKind::Join ||
                   a.kind == ActivityKind::Leave ||
                   a.kind == ActivityKind::Invite ||
                   a.kind == ActivityKind::RoleChange;
        case ActivityFilter::Billing:
            return a.kind == ActivityKind::General; // loosely mapped
    }
    return true;
}

[[nodiscard]] inline Element RenderActivityTab(
    const TeamDetailsDialogOptions& opts) {

    // Filter / range row
    Elements filter_chips;
    constexpr std::array<ActivityFilter, 4> kFilters = {
        ActivityFilter::All, ActivityFilter::AgentOps,
        ActivityFilter::MemberOps, ActivityFilter::Billing,
    };
    for (auto f : kFilters) {
        bool sel = (f == opts.activity_filter);
        Element e = text(std::format(" {} ", FilterLabel(f)));
        e = sel ? (e | bold | bgcolor(Color::Cyan) | inverted)
                : (e | dim);
        filter_chips.push_back(e);
        filter_chips.push_back(text(" "));
    }
    Elements range_chips;
    constexpr std::array<TimeRange, 3> kRanges = {
        TimeRange::Today, TimeRange::Last7d, TimeRange::Last30d,
    };
    for (auto r : kRanges) {
        bool sel = (r == opts.time_range);
        Element e = text(std::format(" {} ", RangeLabel(r)));
        e = sel ? (e | bold | bgcolor(Color::Cyan) | inverted)
                : (e | dim);
        range_chips.push_back(e);
        range_chips.push_back(text(" "));
    }

    Element top_row = hbox({
        text(" Filter: ") | dim,
        hbox(filter_chips),
        filler(),
        text(" Range: ") | dim,
        hbox(range_chips),
    });

    // Timeline rows
    Elements rows;
    int shown = 0;
    for (const auto& a : opts.activity) {
        if (!ActivityMatches(a, opts.activity_filter)) continue;
        ++shown;

        Element t = text(RelativeTime(a.when)) | dim
            | color(Color::GrayDark) | size(WIDTH, EQUAL, 10);
        Element avatar = AgentAvatar(a.actor_name);
        Element glyph = text(std::string{ActivityGlyph(a.kind)})
            | color(ActivityColor(a.kind));
        Element line1 = hbox({
            text(a.actor_name) | bold,
            text(" "),
            text(a.summary) | color(ActivityColor(a.kind)),
        });
        Element line2 = text("");
        if (a.detail) {
            line2 = text("      " + *a.detail) | dim;
        }
        rows.push_back(hbox({
            t, avatar, text(" "), glyph, text(" "), vbox({line1, line2}),
            filler(),
        }));
    }
    if (rows.empty()) rows.push_back(text(" No activity") | dim | center);

    Element footer = hbox({
        text(std::format(" {} entry(s) shown", shown)) | dim,
        filler(),
        text(" f filter · r range") | dim,
    });

    return vbox({
        top_row,
        separator(),
        vbox(rows) | vscroll_indicator | yframe | flex,
        separator(),
        footer,
    }) | borderLight;
}

// ----------------------- Tab 3: Permissions -----------------------

// Role role-column header (the MembershipRole list that forms the matrix
// columns).  Rows are Capabilities.
[[nodiscard]] inline Element RenderPermissionsTab(
    TeamDetailsDialogOptions& opts) {

    // Title + edit mode toggle
    Element title_row = hbox({
        text(" RBAC role matrix ") | bold | color(Color::Cyan),
        filler(),
        opts.matrix.edit_mode
            ? (text(" ● EDITING ") | bold | color(Color::Yellow)
                | bgcolor(Color::RGB(60, 45, 5)))
            : (text(" read-only ") | dim),
        text(" [e] " +
             std::string(opts.matrix.edit_mode ? "exit edit mode"
                                               : "enter edit mode"))
            | color(Color::Cyan),
        text(" "),
        text(" [+ New role] ") | color(Color::Green) | borderLight,
    });

    // Header row: blank + role columns
    Elements header_cells;
    header_cells.push_back(
        text(" Capability ") | bold
        | size(WIDTH, GREATER_THAN, 28));
    for (const auto& r : kRoles) {
        header_cells.push_back(
            RoleTag(r) | bold | center
            | size(WIDTH, EQUAL, 12));
    }
    Element header_line = hbox(header_cells)
        | bgcolor(Color::RGB(25, 30, 45));

    // Body
    Elements body;
    body.push_back(header_line);
    body.push_back(separator());

    for (int ci = 0; ci < static_cast<int>(Capability::_Count); ++ci) {
        Capability c = static_cast<Capability>(ci);
        bool selected = (ci == opts.selected_capability_row)
                        && opts.matrix.edit_mode;

        Elements cells;
        cells.push_back(
            text(std::format(" {} ", CapabilityLabel(c)))
            | color(selected ? Color::White : Color::GrayLight)
            | (selected ? bold : nothing)
            | size(WIDTH, GREATER_THAN, 28));

        for (const auto& r : kRoles) {
            bool v = opts.matrix.lookup(r, c);
            std::string chk = v ? " ✓ " : "   ";
            auto cell = text(chk) | center | size(WIDTH, EQUAL, 12);
            if (v) cell = cell | color(Color::Green) | bold;
            if (selected) cell = cell | bgcolor(Color::RGB(25, 30, 48));
            cells.push_back(cell);
        }

        Element row = hbox(cells);
        if (selected) row = row | bgcolor(Color::RGB(25, 30, 48));
        body.push_back(row);
    }

    Element footer = hbox({
        text(opts.matrix.edit_mode
                 ? " Space toggle · ↑/↓ move · Enter save"
                 : " e to edit · Esc to cancel") | dim,
        filler(),
        text(std::format(" {} capabilities × {} roles",
              static_cast<int>(Capability::_Count), kRoles.size()))
            | dim,
    });

    return vbox({
        title_row,
        separator(),
        vbox(body) | vscroll_indicator | yframe | flex,
        separator(),
        footer,
    }) | borderLight;
}

// ----------------------- Tab 4: Billing / Integrations -----------------------

[[nodiscard]] inline Element RenderBillingTab(
    const TeamDetailsDialogOptions& opts) {

    // --- Usage stats section ---
    Element usage_header = hbox({
        text(" Usage ") | bold | color(Color::Cyan),
        filler(),
        text(" (data from UI3 usage_dialog) ") | dim,
    });

    Element usage_body = opts.usage_stats_element
        ? opts.usage_stats_element
        : (vbox({
            text(" ┌ Total cost .............. $0.00 ") | dim,
            text(" ├ Tokens (in / out) ...... 0 / 0 ") | dim,
            text(" ├ Daily budget ............ 0% ")    | dim,
            text(" └ Rate limit .............. 0% ")    | dim,
        }) | borderLight);

    // --- Integrations section ---
    Element int_header = hbox({
        text(" Integrations ") | bold | color(Color::Cyan),
        filler(),
        text(" Space toggle · Enter configure") | dim,
    });

    Elements int_rows;
    int_rows.push_back(int_header);
    int_rows.push_back(separator());

    for (const auto& it : opts.integrations) {
        Element icon = text(it.glyph + " ") | center;
        Element name = text(it.name) | bold;
        Element acct = it.account
            ? text(" (" + *it.account + ")") | dim
            : text("");
        Element toggle = hbox({
            text(it.connected ? "● connected " : "○ disabled "),
            text("")  | color(it.connected ? Color::Green : Color::GrayDark),
        }) | color(it.connected ? Color::Green : Color::GrayDark);
        Element cfg = text(" [Configure] ") | color(Color::Cyan) | borderLight;

        int_rows.push_back(hbox({
            icon, name, acct, filler(), toggle, cfg,
        }));
    }
    if (opts.integrations.empty()) {
        int_rows.push_back(text(" No integrations configured") | dim | center);
    }

    return vbox({
        usage_header,
        usage_body,
        separator(),
        vbox(int_rows) | borderLight,
    }) | yframe | flex;
}

// ============================================================
// Full dialog renderer
// ============================================================

[[nodiscard]] inline Element RenderTeamDetailsDialog(
    TeamDetailsDialogOptions& opts) {

    Element body = text("");
    switch (opts.active_tab) {
        case Tab::Members:     body = RenderMembersTab(opts);     break;
        case Tab::Activity:    body = RenderActivityTab(opts);    break;
        case Tab::Permissions: body = RenderPermissionsTab(opts); break;
        case Tab::Billing:     body = RenderBillingTab(opts);     break;
        default: body = text("Unknown tab") | dim;
    }

    Element footer = hbox({
        text(" ←/→ tabs · Esc close") | dim,
        filler(),
        text(" Team ") | dim | color(Color::Magenta),
        text(opts.team_id) | dim | color(Color::GrayDark),
    });

    return vbox({
        RenderHeader(opts),
        separator(),
        body | flex,
        separator(),
        footer,
    }) | borderRounded;
}

// ============================================================
// Interactive Component
// ============================================================

/// Keybindings:
///   Tab / →  - next tab
///   Shift+Tab / ←  - prev tab
///   Esc      - close
///
/// Members tab extras:
///   j/k      - select member
///   r        - cycle role of selected member (triggers on_change_member_role)
///   R        - remove (triggers on_remove_member)
///   i        - invite  (Invite header button)
///   x        - export CSV
///   g        - settings
///
/// Activity tab extras:
///   f        - cycle filter
///   r        - cycle range
///
/// Permissions tab extras:
///   e        - toggle edit mode
///   j/k      - move row
///   Space    - toggle selected cell (cycles through roles, or per-role?
///              simplest: toggles the current-row/Owner→Guest left-to-right
///              via shift+hjkl. We implement the simpler Space → flip all
///              roles for that row to the "next obvious state", and hook
///              into on_toggle_permission for each flipped cell.)
///   +        - create custom role
///
/// Billing tab extras:
///   j/k      - select integration
///   Space    - toggle selected integration
///   Enter    - configure selected integration
[[nodiscard]] inline Component MakeTeamDetailsDialog(
    TeamDetailsDialogOptions options) {

    auto state = std::make_shared<TeamDetailsDialogOptions>(std::move(options));
    state->integrations.push_back({"Slack",  "💬", true,  "acme-workspace"});
    state->integrations.push_back({"GitHub", "🐙", true,  "github.com/acme"});
    state->integrations.push_back({"Jira",   "📋", false, std::nullopt});
    // Persistent billing selection
    state->on_toggle_integration = state->on_toggle_integration;

    return Renderer([state] {
        return RenderTeamDetailsDialog(*state);
    }) | CatchEvent([state](Event event) -> bool {

        // Escape always closes
        if (event == Event::Escape) {
            // If we're in edit mode, Esc first exits edit mode, then closes
            // on the second press (feels natural).
            if (state->active_tab == Tab::Permissions &&
                state->matrix.edit_mode) {
                state->matrix.edit_mode = false;
                if (state->on_toggle_edit_mode)
                    state->on_toggle_edit_mode(false);
                return true;
            }
            if (state->on_close) state->on_close();
            return true;
        }

        // Tab navigation (global)
        if (event == Event::Tab || event == Event::ArrowRight) {
            int next = (static_cast<int>(state->active_tab) + 1)
                       % static_cast<int>(Tab::_Count);
            state->active_tab = static_cast<Tab>(next);
            if (state->on_switch_tab) state->on_switch_tab(state->active_tab);
            return true;
        }
        if (event == Event::Special({27,91,90}) || // Shift+Tab
            event == Event::ArrowLeft) {
            int cur = static_cast<int>(state->active_tab);
            int total = static_cast<int>(Tab::_Count);
            int prev = (cur - 1 + total) % total;
            state->active_tab = static_cast<Tab>(prev);
            if (state->on_switch_tab) state->on_switch_tab(state->active_tab);
            return true;
        }

        // ----- Members-tab keys -----
        if (state->active_tab == Tab::Members) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->selected_member =
                    std::max(0, state->selected_member - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->selected_member = std::min(
                    static_cast<int>(state->members.size()) - 1,
                    state->selected_member + 1);
                return true;
            }
            // 'r' cycle role
            if (event == Event::Character('r')) {
                if (!state->members.empty() && state->on_change_member_role) {
                    const auto& cur = state->members[state->selected_member];
                    constexpr MembershipRole kCycle[] = {
                        MembershipRole::Guest,
                        MembershipRole::Member,
                        MembershipRole::Maintainer,
                        MembershipRole::Owner,
                    };
                    int idx = 0;
                    constexpr int kCycleCount = static_cast<int>(std::size(kCycle));
                    for (int i = 0; i < kCycleCount; ++i)
                        if (kCycle[i] == cur.role) { idx = (i + 1) % kCycleCount; break; }
                    MembershipRole next = kCycle[idx];
                    state->members[state->selected_member].role = next;
                    state->on_change_member_role(cur, next);
                }
                return true;
            }
            if (event == Event::Character('R')) {
                if (!state->members.empty() && state->on_remove_member) {
                    state->on_remove_member(
                        state->members[state->selected_member]);
                }
                return true;
            }
            if (event == Event::Character('i')) {
                if (state->on_invite_member) {
                    MembersTableRow blank;
                    state->on_invite_member(blank);
                }
                return true;
            }
            if (event == Event::Character('x')) {
                if (state->on_export_csv) state->on_export_csv();
                return true;
            }
            if (event == Event::Character('g')) {
                if (state->on_team_settings) state->on_team_settings();
                return true;
            }
            return false;
        }

        // ----- Activity-tab keys -----
        if (state->active_tab == Tab::Activity) {
            if (event == Event::Character('f')) {
                int v = (static_cast<int>(state->activity_filter) + 1) % 4;
                state->activity_filter = static_cast<ActivityFilter>(v);
                if (state->on_change_filter)
                    state->on_change_filter(state->activity_filter);
                return true;
            }
            if (event == Event::Character('r')) {
                int v = (static_cast<int>(state->time_range) + 1) % 3;
                state->time_range = static_cast<TimeRange>(v);
                if (state->on_change_range)
                    state->on_change_range(state->time_range);
                return true;
            }
            if (event == Event::ArrowUp   || event == Event::Character('k') ||
                event == Event::ArrowDown || event == Event::Character('j')) {
                return true; // eat them so nothing scrolls unexpectedly
            }
            return false;
        }

        // ----- Permissions-tab keys -----
        if (state->active_tab == Tab::Permissions) {
            if (event == Event::Character('e')) {
                state->matrix.edit_mode = !state->matrix.edit_mode;
                if (state->on_toggle_edit_mode)
                    state->on_toggle_edit_mode(state->matrix.edit_mode);
                return true;
            }
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->selected_capability_row = std::max(
                    0, state->selected_capability_row - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->selected_capability_row = std::min(
                    static_cast<int>(Capability::_Count) - 1,
                    state->selected_capability_row + 1);
                return true;
            }
            // Space: flip permissions for all columns on this row
            // (cycles through "all default → all on → all off → default")
            if (event == Event::Character(' ') && state->matrix.edit_mode) {
                Capability c = static_cast<Capability>(
                    state->selected_capability_row);
                // Compute a new value - toggle everything opposite its default
                for (const auto& r : kRoles) {
                    bool cur = state->matrix.lookup(r, c);
                    bool def = DefaultPermission(r, c);
                    // Policy: if any cell differs from default, reset all
                    // to default.  Otherwise flip to all-on (or if already
                    // default == all-on, flip to all-off).
                    (void)def;
                    bool nv = !cur;
                    state->matrix.set(r, c, nv);
                    if (state->on_toggle_permission)
                        state->on_toggle_permission(r, c, nv);
                }
                return true;
            }
            // '+' - create custom role
            if (event == Event::Character('+') ||
                event == Event::Character('n')) {
                if (state->on_create_custom_role)
                    state->on_create_custom_role();
                return true;
            }
            return false;
        }

        // ----- Billing-tab keys -----
        if (state->active_tab == Tab::Billing) {
            static int s_integration_idx = 0;
            int total = static_cast<int>(state->integrations.size());
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                s_integration_idx = std::max(0, s_integration_idx - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                s_integration_idx = std::max(0,
                    std::min(total - 1, s_integration_idx + 1));
                return true;
            }
            if (event == Event::Character(' ')) {
                if (total > 0) {
                    auto& row = state->integrations[s_integration_idx];
                    row.connected = !row.connected;
                    if (state->on_toggle_integration)
                        state->on_toggle_integration(row, row.connected);
                }
                return true;
            }
            if (event == Event::Return) {
                if (total > 0 && state->on_configure_integration)
                    state->on_configure_integration(
                        state->integrations[s_integration_idx]);
                return true;
            }
            return false;
        }

        return false;
    });
}

} // namespace cc::ui::teams::details
