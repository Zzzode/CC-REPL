/// @file log_selector.cppm
/// @brief Multi-source log panel (LogSelector).
/// Migrated from src/components/LogSelector.tsx (~1574 lines).
///
/// Covers 6 tabs in a 3-split layout:
///   1. Recent Sessions   – active / pinned sessions, card grid.
///   2. Projects          – sessions grouped by project tag, collapsible.
///   3. Drafts            – unsent prompts, card grid.
///   4. Archive           – read-only sessions grouped by YYYY-MM timeline,
///                          restore available.
///   5. Remote / Cloud    – paginated cloud list, load-more + spinner.
///   6. Trash             – soft-deleted sessions (restore / purge).
///
/// Layout:
///   ┌─────── 25% sidebar ───────┬────── 60% main ──────┬── 15% preview ──┐
///   │ 🔍 Search                 │ Tab content: grid /   │ 10-msg preview  │
///   │  1 Recent       (124)     │   list / timeline    │ metadata card   │
///   │  2 Projects      (18)     │                      │                 │
///   │  3 Drafts         (3)     │  filter chips row    │                 │
///   │  4 Archive       (77)     │                      │                 │
///   │  5 Remote         (0)     │                      │                 │
///   │  6 Trash         (12)     │                      │                 │
///   │                           │                      │                 │
///   │  ✨ [N] New session       │                      │                 │
///   └───────────────────────────┴──────────────────────┴─────────────────┘
///   footer toolbar:  [a]rchive [d]elete [r]estore [e]xport [i]mport
///                    [p]in [t]ag [/]search [Enter] open [Esc/q] close
///
/// Batch ops: Shift-range select, v-visual mode, Ctrl+A select all,
///            u clear.  Shift+↑/↓ reorder pinned rows.
/// Delete confirm (>10) → Medium TrustDialog; purge → High TrustDialog
/// with mandatory 5 s countdown (UI8, reused — not reimplemented).
module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.screens.log_selector;

import cc.ui.trust_dialog;
import cc.ui.trust_utils;
import cc.ui.design.tokens;

export namespace cc::ui::screens::log_selector {

using namespace ftxui;

// ─── Cross-module imports (tokens / UI8 trust primitives) ────────────────────
namespace dt = cc::ui::design::tokens;
namespace tu = cc::ui::trust_utils;
using tu::RiskLevel;
using tu::TrustChoice;
using cc::ui::trust_dialog::TrustDialogProps;
using cc::ui::trust_dialog::MakeTrustDialogComponent;

// =========================================================================
// 1. Enums
// =========================================================================

/// The six sidebar tabs — matches the navigation order of TS LogSelector's
/// extended view (Recent / Projects / Drafts / Archive / Remote / Trash).
enum class Tab : std::uint8_t {
    Recent = 0,
    Projects,
    Drafts,
    Archive,
    Remote,
    Trash,
};

[[nodiscard]] constexpr std::string_view to_string(Tab t) {
    switch (t) {
        case Tab::Recent:   return "Recent";
        case Tab::Projects: return "Projects";
        case Tab::Drafts:   return "Drafts";
        case Tab::Archive:  return "Archive";
        case Tab::Remote:   return "Remote";
        case Tab::Trash:    return "Trash";
    }
    return "Recent";
}

[[nodiscard]] constexpr std::string_view tab_icon(Tab t) {
    switch (t) {
        case Tab::Recent:   return "🕘";
        case Tab::Projects: return "📁";
        case Tab::Drafts:   return "📝";
        case Tab::Archive:  return "🗄";
        case Tab::Remote:   return "☁";
        case Tab::Trash:    return "🗑";
    }
    return "•";
}

/// Lifecycle status of a single log entry.  Each tab primarily exposes one
/// status, but filtering allows overlays (e.g. Archived pinned entries).
enum class Status : std::uint8_t {
    Active = 0,   // Recent tab
    Draft,        // Drafts tab
    Archived,     // Archive tab
    Trashed,      // Trash tab
    Remote,       // Remote / cloud
};

[[nodiscard]] constexpr std::string_view to_string(Status s) {
    switch (s) {
        case Status::Active:   return "Active";
        case Status::Draft:    return "Draft";
        case Status::Archived: return "Archived";
        case Status::Trashed:  return "Trashed";
        case Status::Remote:   return "Remote";
    }
    return "Active";
}

[[nodiscard]] constexpr Color status_color(Status s) {
    switch (s) {
        case Status::Active:   return Color::Green;
        case Status::Draft:    return Color::Yellow;
        case Status::Archived: return Color::GrayLight;
        case Status::Trashed:  return Color::Red;
        case Status::Remote:   return Color::Cyan;
    }
    return Color::White;
}

/// Batch toolbar operations triggered by pill keys and the footer.
enum class BatchOp : std::uint8_t {
    Archive,
    Restore,
    Delete,
    Purge,
    PinToggle,
    TagAssign,
    Export,
};

/// Date-range filter preset — matches TS filter chips row.
enum class DateRange : std::uint8_t {
    Any = 0,
    Today,
    Last7d,
    Last30d,
    Custom,
};

// =========================================================================
// 2. Data types (mirror TS SessionLogEntry / ProjectEntry)
// =========================================================================

/// One row of session metadata.  Mirrors the fields of LogOption +
/// the richer fields exposed by the TS `SessionLogEntry` interface used
/// internally inside LogSelector.tsx (title, project tag, model, counts,
/// cost, tokens, timestamps, status, pin flag).
struct SessionLogEntry {
    std::string id;
    std::string title;
    std::optional<std::string> project;
    std::optional<std::string> model;
    int msg_count = 0;
    int turns = 0;
    double cost_usd = 0.0;
    std::vector<std::string> tags;
    std::size_t tokens_in = 0;
    std::size_t tokens_out = 0;
    std::chrono::system_clock::time_point created{};
    std::chrono::system_clock::time_point updated{};
    Status status = Status::Active;
    bool pinned = false;
    /// First 1-2 line summary used for title + search + preview.
    std::string preview_summary;
    /// First 10 collapsed messages — populated lazily by the provider when
    /// the right-side preview panel needs them.
    mutable std::optional<std::vector<std::pair<std::string, std::string>>>
        preview_messages;  // .first = role label, .second = collapsed text
};

/// A project group used by the Projects tab.
struct ProjectEntry {
    std::string name;
    std::vector<SessionLogEntry> sessions;
    std::chrono::system_clock::time_point last_activity{};
    int total_sessions = 0;
    double total_cost_usd = 0.0;
    bool collapsed = false;
};

/// Top-level options struct — every data access goes through a provider
/// lambda so this module stays pure-UI (no business-module imports).
struct LogSelectorOptions {
    Tab initial_tab = Tab::Recent;

    /// Produce the raw list of sessions visible to Recent / Drafts / Archive /
    /// Trash tabs.  The component itself filters by status + search.
    std::function<std::vector<SessionLogEntry>()> sessions_provider;

    /// Produce project-grouped sessions (used for the Projects tab).
    std::function<std::vector<ProjectEntry>()> projects_provider;

    /// Callback when user opens (Enter) a single session.
    std::function<void(const std::string& id)> on_open;

    /// Callback for batch operations (archive / restore / delete / purge /
    /// pin / tag).  The vector of IDs is the multi-selection.
    std::function<void(BatchOp op, std::vector<std::string> ids)> on_batch;

    /// Export the selected IDs to JSONL.
    std::function<void(std::vector<std::string> ids)> on_export;

    /// Import from a filesystem path.  The path has already been validated
    /// for existence by the component.
    std::function<void(const std::string& path)> on_import;

    /// Remote-tab: fetch page `page` (0-based, 50 items per page).  When the
    /// fetched batch arrives the caller should re-invoke the sessions
    /// provider with the concatenated remote list.
    std::function<void(int page, std::string_view cursor)> on_fetch_remote;

    /// Triggered lazily when the right preview panel is shown for a session
    /// that doesn't yet have preview_messages filled in.
    std::function<std::vector<std::pair<std::string, std::string>>(
        const std::string& id, std::size_t limit)>
        load_preview;

    /// Dismiss / close the panel.  Fires on Esc / q.
    std::function<void()> on_cancel;

    /// New session button (bottom-left of sidebar).
    std::function<void()> on_new_session;

    /// Called when the component needs a High-risk TrustDialog overlay to
    /// confirm a purge.  Caller should invoke MakeTrustDialogComponent()
    /// with the supplied props and swap it in front of the main screen.
    std::function<void(TrustDialogProps)> request_trust_dialog;
};

// =========================================================================
// 3. Small helpers — time / text / search
// =========================================================================

// CSS-style padding decorator.
inline ftxui::Decorator padding(int top, int right, int bottom, int left) {
    using namespace ftxui;
    return [=](Element e) -> Element {
        Elements rows;
        for (int i = 0; i < top; ++i) rows.push_back(text(""));
        {
            Elements lp, rp;
            for (int i = 0; i < left; ++i) lp.push_back(text(" "));
            for (int i = 0; i < right; ++i) rp.push_back(text(" "));
            rows.push_back(hbox({hbox(std::move(lp)), std::move(e), hbox(std::move(rp))}));
        }
        for (int i = 0; i < bottom; ++i) rows.push_back(text(""));
        return vbox(std::move(rows));
    };
}
inline ftxui::Decorator padding(int all) { return padding(all, all, all, all); }
inline ftxui::Decorator padding(int horizontal, int vertical) {
    return padding(vertical, horizontal, vertical, horizontal);
}

[[nodiscard]] inline std::string truncate(std::string_view s, std::size_t max) {
    if (s.size() <= max) return std::string{s};
    if (max <= 3) return std::string{s.substr(0, max)};
    return std::string{s.substr(0, max - 3)} + "...";
}

[[nodiscard]] inline std::string format_relative(
    std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto diff = duration_cast<minutes>(now - tp);
    if (diff.count() < 1)       return "just now";
    if (diff.count() < 60)      return std::format("{}m ago", diff.count());
    const auto h = duration_cast<hours>(diff);
    if (h.count() < 24)         return std::format("{}h ago", h.count());
    const long days = h.count() / 24;
    if (days < 7)               return std::format("{}d ago", days);
    if (days < 30)              return std::format("{}w ago", days / 7);
    if (days < 365)             return std::format("{}mo ago", days / 30);
    return std::format("{}y ago", days / 365);
}

[[nodiscard]] inline std::string format_absolute(
    std::chrono::system_clock::time_point tp)
{
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return std::string{buf};
}

[[nodiscard]] inline std::string format_month_key(
    std::chrono::system_clock::time_point tp)
{
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m", &tm_buf);
    return std::string{buf};
}

[[nodiscard]] inline bool icontains(std::string_view hay,
                                    std::string_view needle)
{
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    const auto lc = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lc(hay[i + j]) != lc(needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

[[nodiscard]] inline bool in_date_range(
    std::chrono::system_clock::time_point tp, DateRange r)
{
    if (r == DateRange::Any) return true;
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto days = duration_cast<hours>(now - tp).count() / 24;
    switch (r) {
        case DateRange::Today:   return days < 1;
        case DateRange::Last7d:  return days <= 7;
        case DateRange::Last30d: return days <= 30;
        default:                 return true;  // Custom handled outside
    }
}

// =========================================================================
// 4. UI19-style session card  (copied verbatim from resume_screen.cppm
//    RenderSessionCard shape — same icon/colour tokens, same split layout,
//    same selected/hovered treatment.  Comment retained so style stays in
//    sync with UI19.  Inlined here (not a cross-module import) so
//    log_selector remains a self-contained P0 module.
// =========================================================================
namespace ui19_style {
    const Color kGreen  = Color::Green;
    const Color kBlue   = Color::Blue;
    const Color kCyan   = Color::Cyan;
    const Color kYellow = Color::Yellow;
    const Color kRed    = Color::Red;
    const Color kDim    = Color::GrayLight;
    const Color kFg     = Color::White;

    /// Identical Element layout to resume_screen::RenderSessionCard.
    /// Uses SessionLogEntry instead of SessionMetaRow — fields are 1:1.
    [[nodiscard]] inline Element RenderSessionCard(
        const SessionLogEntry& r,
        bool selected, bool hovered, bool pinned)
    {
        Elements left_col;
        const std::string title = truncate(r.title, 45);
        left_col.push_back(hbox({
            text(std::string(pinned ? "📌" : "📝") + " ") | dim,
            text(title) | (selected ? (bold | color(kGreen))
                                    : (bold | color(kFg))),
        }));
        left_col.push_back(hbox({
            text("    ") | dim,
            text(truncate(r.preview_summary, 60)) | dim | color(kDim),
        }));
        const std::string model = r.model ? *r.model : std::string{"(model)"};
        left_col.push_back(hbox({
            text("    ") | dim,
            text("🧠 ") | dim,
            text(truncate(model, 28)) | dim | color(kCyan),
            text("  ") | dim,
            text("⏱ ") | dim,
            text(format_relative(r.updated)) | dim | color(kDim),
        }));

        Elements right_col;
        right_col.push_back(hbox({
            text(format_absolute(r.updated)) | dim | color(kDim),
            text("  ") | dim,
            text(std::to_string(r.msg_count)) | bold | color(kCyan),
            text(" msg") | dim | color(kDim),
        }));
        {
            Elements row;
            if (r.cost_usd > 0) {
                row.push_back(text(std::format("${:.2f}", r.cost_usd))
                    | color(kGreen) | dim);
                row.push_back(text("  ") | dim);
            }
            row.push_back(text(std::to_string(r.turns)) | dim | color(kDim));
            row.push_back(text(" turns") | dim | color(kDim));
            right_col.push_back(hbox(std::move(row)));
        }
        if (r.project) {
            right_col.push_back(hbox({
                text("🏷 ") | dim,
                text(truncate(*r.project, 20)) | dim | color(kYellow),
            }));
        }
        if (hovered) {
            right_col.push_back(text(""));
            right_col.push_back(hbox({
                text("[a]rchive  ") | dim | color(kYellow),
                text("[d]el  ") | dim | color(kRed),
                text("[p]in  ") | dim | color(kCyan),
            }));
        } else {
            right_col.push_back(hbox({ text("") }));
            right_col.push_back(hbox({ text("···") | dim | color(kDim) }));
        }

        Element card = hbox({
            vbox(std::move(left_col)) | flex,
            text("   "),
            vbox(std::move(right_col)) | align_right,
        }) | padding(1);

        if (selected) {
            card = card | bgcolor(Color::RGB(25, 30, 45))
                        | borderStyled(kGreen);
        } else {
            card = card | borderLight;
        }

        const std::string chev = selected ? " ❯ " : "   ";
        return hbox({
            text(chev) | color(selected ? kGreen : Color::Default),
            card | flex,
        });
    }
} // namespace ui19_style

// =========================================================================
// 5. Internal state struct
// =========================================================================

struct SelectorState {
    LogSelectorOptions opts;

    // --- data ---
    std::vector<SessionLogEntry> all_sessions;
    std::vector<ProjectEntry>    projects;

    // --- navigation ---
    Tab tab = Tab::Recent;
    std::vector<std::size_t> view;  // indices into all_sessions for the
                                    // currently-filtered main list
    std::size_t cursor = 0;         // index into view
    std::size_t visual_anchor = 0;  // for v-mode range selects
    bool visual_mode = false;

    // --- selection ---
    std::unordered_set<std::string> selected_ids;

    // --- filters ---
    std::string search;
    bool search_focused = false;
    std::unordered_set<Status>    status_filter;   // empty = all
    std::string                   model_filter;
    std::string                   tag_filter;
    std::string                   project_filter;
    DateRange                     date_range = DateRange::Any;
    int                           filter_chip_cursor = 0; // 0..n_chips-1

    // --- Projects tab state ---
    std::vector<std::string> collapsed_projects;
    std::size_t project_cursor = 0;   // flat row cursor into Projects list

    // --- Archive tab: grouped by YYYY-MM ---
    std::vector<std::pair<std::string, std::vector<std::size_t>>> archive_groups;
    std::size_t archive_cursor = 0;

    // --- Remote tab ---
    int remote_page = 0;
    bool remote_loading = false;
    std::string remote_cursor;
    std::size_t remote_fetched = 0;

    // --- Import dialog ---
    bool import_open = false;
    std::string import_path;
    bool import_error = false;

    // --- Lazy preview ---
    std::string preview_for_id;
};

// =========================================================================
// 6. Refresh / filter functions
// =========================================================================

inline Status primary_status_for_tab(Tab t) {
    switch (t) {
        case Tab::Recent:   return Status::Active;
        case Tab::Drafts:   return Status::Draft;
        case Tab::Archive:  return Status::Archived;
        case Tab::Remote:   return Status::Remote;
        case Tab::Trash:    return Status::Trashed;
        default:            return Status::Active; // Projects: mixed
    }
}

/// Recompute `view` for tab views that render a flat list.
// Forward declarations for helpers called inside refresh_view.
inline void refresh_archive_groups(SelectorState& s);
inline void rebuild_project_cursor(SelectorState& s);
inline void refresh_view(SelectorState& s) {
    s.view.clear();
    if (s.tab == Tab::Projects) { refresh_archive_groups(s); rebuild_project_cursor(s); return; }
    if (s.tab == Tab::Archive)  { refresh_archive_groups(s); return; }

    const Status primary = primary_status_for_tab(s.tab);

    for (std::size_t i = 0; i < s.all_sessions.size(); ++i) {
        const auto& e = s.all_sessions[i];
        if (s.tab != Tab::Remote && s.tab != Tab::Recent) {
            if (e.status != primary) continue;
        } else if (s.tab == Tab::Recent) {
            if (e.status != Status::Active) continue;
        } else if (s.tab == Tab::Remote) {
            if (e.status != Status::Remote) continue;
        }

        if (!s.status_filter.empty() && !s.status_filter.count(e.status))
            continue;
        if (!s.model_filter.empty()) {
            if (!e.model || !icontains(*e.model, s.model_filter)) continue;
        }
        if (!s.tag_filter.empty()) {
            bool hit = false;
            for (const auto& t : e.tags) if (icontains(t, s.tag_filter)) { hit = true; break; }
            if (!hit) continue;
        }
        if (!s.project_filter.empty()) {
            if (!e.project || !icontains(*e.project, s.project_filter)) continue;
        }
        if (!in_date_range(e.updated, s.date_range)) continue;
        if (!s.search.empty()) {
            const bool hit = icontains(e.title, s.search)
                          || icontains(e.preview_summary, s.search)
                          || (e.project && icontains(*e.project, s.search))
                          || (e.model && icontains(*e.model, s.search))
                          || [&]{ for (auto& t:e.tags) if (icontains(t,s.search)) return true; return false; }();
            if (!hit) continue;
        }
        s.view.push_back(i);
    }

    // Sort: pinned first, then newest updated.
    std::stable_sort(s.view.begin(), s.view.end(), [&](auto a, auto b) {
        const auto& A = s.all_sessions[a];
        const auto& B = s.all_sessions[b];
        if (A.pinned != B.pinned) return A.pinned;
        return A.updated > B.updated;
    });

    if (s.cursor >= s.view.size() && !s.view.empty()) s.cursor = s.view.size() - 1;
}

// ─── Archive / Projects helpers (declared out-of-line to keep refresh_view
//     readable; called from refresh_view above).  Forward decls keep the
//     compiler happy in a single-module world.
inline void SelectorState_refresh_archive_groups(SelectorState& s);
inline void SelectorState_rebuild_project_cursor(SelectorState& s);
inline void refresh_archive_groups(SelectorState& s) {
    SelectorState_refresh_archive_groups(s);
}
inline void rebuild_project_cursor(SelectorState& s) {
    SelectorState_rebuild_project_cursor(s);
}

inline void SelectorState_refresh_archive_groups(SelectorState& s) {
    s.archive_groups.clear();
    std::unordered_map<std::string, std::vector<std::size_t>> by_month;
    for (std::size_t i = 0; i < s.all_sessions.size(); ++i) {
        const auto& e = s.all_sessions[i];
        if (e.status != Status::Archived) continue;
        if (!s.search.empty()) {
            const bool hit = icontains(e.title, s.search)
                          || icontains(e.preview_summary, s.search);
            if (!hit) continue;
        }
        by_month[format_month_key(e.updated)].push_back(i);
    }
    s.archive_groups.assign(by_month.begin(), by_month.end());
    std::sort(s.archive_groups.begin(), s.archive_groups.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [k, v] : s.archive_groups) {
        std::sort(v.begin(), v.end(), [&](auto x, auto y) {
            return s.all_sessions[x].updated > s.all_sessions[y].updated;
        });
    }
    if (s.archive_cursor > 0 && s.archive_groups.empty()) s.archive_cursor = 0;
}

inline void SelectorState_rebuild_project_cursor(SelectorState& /*s*/) {
    // Flat cursor is recomputed inside RenderProjects from collapsed state —
    // the actual row traversal happens at render time.
}

// =========================================================================
// 7. Sidebar rendering
// =========================================================================

[[nodiscard]] inline Element TabBadge(std::size_t count) {
    if (count == 0) return text("");
    return text(std::format(" ({})", count)) | dim | color(Color::GrayLight);
}

[[nodiscard]] inline Element RenderSidebar(SelectorState& s) {
    const auto count_status = [&](Status st) -> std::size_t {
        return std::count_if(s.all_sessions.begin(), s.all_sessions.end(),
            [=](const auto& e) { return e.status == st; });
    };
    const auto cnt_projects = s.projects.size();

    struct TabRowDef { Tab t; std::string_view hotkey; std::size_t count; };
    const std::array<TabRowDef, 6> rows{{
        { Tab::Recent,   "1", count_status(Status::Active)   },
        { Tab::Projects, "2", cnt_projects                   },
        { Tab::Drafts,   "3", count_status(Status::Draft)    },
        { Tab::Archive,  "4", count_status(Status::Archived) },
        { Tab::Remote,   "5", count_status(Status::Remote)   },
        { Tab::Trash,    "6", count_status(Status::Trashed)  },
    }};

    Elements items;

    // Search box
    const bool sf = s.search_focused;
    items.push_back(hbox({
        text(sf ? " / " : " 🔍 ")
            | color(sf ? Color::Cyan : Color::GrayLight),
        s.search.empty()
            ? text("Search…") | dim
            : text(s.search) | color(Color::White),
        text(sf ? "│" : " ") | (sf ? blink : nothing),
    }) | borderLight | size(WIDTH, EQUAL, 26));

    items.push_back(text(""));

    // Tabs
    for (const auto& r : rows) {
        const bool sel = (s.tab == r.t);
        Color chev_col = sel ? Color(Color::Green) : Color(Color::Default);
        Element line = hbox({
            text(sel ? "▶ " : "  ") | color(chev_col),
            text(std::string{tab_icon(r.t)}) | dim,
            text(" ") | dim,
            text(std::string{to_string(r.t)})
                | (sel ? (bold | color(Color::Green)) : color(Color::White)),
            filler(),
            TabBadge(r.count),
            text(" [") | dim,
            text(std::string{r.hotkey}) | bold | color(Color::Cyan),
            text("]") | dim,
        });
        if (sel) line = line | bgcolor(Color::RGB(25, 30, 45));
        items.push_back(line | size(WIDTH, EQUAL, 28));
    }

    items.push_back(text(""));
    items.push_back(separatorLight());

    // New session button
    items.push_back(hbox({
        text("✨ ") | color(Color::Yellow),
        text("[N] New session") | bold | color(Color::Yellow),
    }) | size(WIDTH, EQUAL, 28));

    return vbox(std::move(items))
         | borderRounded
         | size(WIDTH, EQUAL, 32)
         | padding(1);
}

// =========================================================================
// 8. Filter chips row
// =========================================================================

struct ChipDef {
    std::string label;
    std::string value;   // current value text (empty = inactive)
    std::string hotkey;
};

[[nodiscard]] inline std::vector<ChipDef> MakeChips(const SelectorState& s) {
    std::vector<ChipDef> out;
    // Status: 5-tuple multi-select pill
    {
        std::string val;
        if (s.status_filter.empty()) val = "all";
        else {
            for (auto st : { Status::Active, Status::Draft, Status::Archived,
                             Status::Trashed, Status::Remote })
            {
                if (s.status_filter.count(st)) {
                    if (!val.empty()) val += ",";
                    val += to_string(st).substr(0, 3);
                }
            }
        }
        out.push_back({ "Status", val, "s" });
    }
    out.push_back({ "Model",
                    s.model_filter.empty() ? "any" : s.model_filter, "m" });
    out.push_back({ "Tag",
                    s.tag_filter.empty() ? "any" : s.tag_filter, "#" });
    out.push_back({ "Project",
                    s.project_filter.empty() ? "any" : s.project_filter, "P" });
    std::string dr;
    switch (s.date_range) {
        case DateRange::Any:     dr = "any"; break;
        case DateRange::Today:   dr = "today"; break;
        case DateRange::Last7d:  dr = "7d"; break;
        case DateRange::Last30d: dr = "30d"; break;
        case DateRange::Custom:  dr = "custom"; break;
    }
    out.push_back({ "Date", dr, "d" });
    return out;
}

[[nodiscard]] inline Element RenderFilterChips(SelectorState& s) {
    const auto chips = MakeChips(s);
    Elements row;
    row.push_back(text(" [f] filter ") | bold | color(Color::Cyan));
    for (std::size_t i = 0; i < chips.size(); ++i) {
        const bool focus = (i == static_cast<std::size_t>(s.filter_chip_cursor));
        const auto& c = chips[i];
        Element chip = hbox({
            text(c.label) | dim,
            text("=") | dim,
            text(c.value) | color(c.value == "any" || c.value == "all"
                                      ? Color::GrayLight
                                      : Color::Cyan),
        });
        if (focus) chip = chip | bold | bgcolor(Color::RGB(25, 30, 45))
                                | borderStyled(Color::Green);
        else        chip = chip | borderLight;
        chip = chip | padding(0, 1);
        row.push_back(text(" "));
        row.push_back(chip);
    }
    return hbox(std::move(row));
}

// =========================================================================
// 9. Per-tab content rendering
// =========================================================================

/// Shared helper: fetch the SessionLogEntry* under `cursor` for flat-tab
/// views.  Returns nullptr when the view is empty.
[[nodiscard]] inline const SessionLogEntry* current_entry(
    const SelectorState& s)
{
    if (s.view.empty()) return nullptr;
    const auto c = std::min(s.cursor, s.view.size() - 1);
    return &s.all_sessions[s.view[c]];
}

[[nodiscard]] inline Element RenderCardGrid(SelectorState& s) {
    Elements cards;
    // Header
    cards.push_back(hbox({
        text(" " + std::string{to_string(s.tab)} + " ") | bold | color(Color::Green),
        text(std::format("({})", s.view.size())) | dim,
        filler(),
        text(s.selected_ids.empty()
                 ? std::string{}
                 : std::format("{} selected ", s.selected_ids.size()))
            | color(Color::Cyan),
        text(s.visual_mode ? "VISUAL" : "") | color(Color::Yellow) | bold,
    }));
    cards.push_back(separator());
    cards.push_back(RenderFilterChips(s));
    cards.push_back(separator());

    if (s.view.empty()) {
        cards.push_back(text(""));
        cards.push_back(text(" No entries.") | dim | center);
        cards.push_back(text(""));
        return vbox(std::move(cards));
    }

    // Lazy: trigger fetch-more for Remote tab if cursor falls past
    // (size - 10).  This matches the TS spec "scroll to N-10 triggers
    // on_fetch_more()".
    if (s.tab == Tab::Remote && !s.remote_loading && !s.view.empty()) {
        const auto threshold = (s.view.size() >= 10) ? s.view.size() - 10 : 0;
        if (s.cursor >= threshold) {
            s.remote_loading = true;
            s.remote_page += 1;
            s.remote_fetched = s.view.size();
            if (s.opts.on_fetch_remote) {
                s.opts.on_fetch_remote(s.remote_page, s.remote_cursor);
            }
        }
    }

    // 2-column card grid.  (In terminals we use a simple 1-wide vertical
    // stream if columns < 100; callers with wider terminals get 2-col via
    // FTXUI flex.  Keeping the simple layout avoids complex row math.)
    for (std::size_t i = 0; i < s.view.size(); ++i) {
        const auto& e = s.all_sessions[s.view[i]];
        const bool sel = (i == s.cursor);
        const bool in_batch = s.selected_ids.count(e.id) > 0;
        cards.push_back(
            ui19_style::RenderSessionCard(e, sel || in_batch, sel, e.pinned)
        );
        if (i + 1 < s.view.size()) cards.push_back(text(" "));
    }

    // Remote-tab: loading pill
    if (s.tab == Tab::Remote) {
        cards.push_back(separatorLight());
        if (s.remote_loading) {
            cards.push_back(hbox({
                text(" ⠋ ") | blink | color(Color::Cyan),
                text(std::format(" Loading more… ({} fetched so far) ",
                                 s.remote_fetched))
                    | color(Color::Cyan),
            }));
        } else {
            cards.push_back(hbox({
                text(" ∞ ") | dim,
                text(std::format(" Load more ({} fetched) — press j to reach bottom ",
                                 s.view.size()))
                    | dim,
            }));
        }
    }

    return vbox(std::move(cards))
         | yframe | vscroll_indicator | flex;
}

// ─── Projects tab: collapsible groups, each group exposes its cards.
//     We track `project_cursor` as a flat index across (header + rows).
[[nodiscard]] inline Element RenderProjects(SelectorState& s) {
    Elements cards;
    cards.push_back(hbox({
        text(" Projects ") | bold | color(Color::Blue),
        text(std::format("({})", s.projects.size())) | dim,
        filler(),
    }));
    cards.push_back(separator());
    cards.push_back(RenderFilterChips(s));
    cards.push_back(separator());

    std::size_t flat_index = 0;
    std::size_t total_rows = 0;

    for (auto& p : s.projects) {
        const bool is_collapsed = std::any_of(
            s.collapsed_projects.begin(), s.collapsed_projects.end(),
            [&](const auto& n) { return n == p.name; });
        total_rows += 1;                         // header
        if (!is_collapsed) total_rows += p.sessions.size();
    }

    // Clamp cursor
    if (s.project_cursor >= total_rows && total_rows > 0)
        s.project_cursor = total_rows - 1;

    for (auto& p : s.projects) {
        const bool is_collapsed = std::any_of(
            s.collapsed_projects.begin(), s.collapsed_projects.end(),
            [&](const auto& n) { return n == p.name; });
        const bool header_focus = (flat_index == s.project_cursor);
        Color hdr_fg = header_focus ? Color(Color::Green) : Color(Color::Default);

        // Group header
        {
            Element hdr = hbox({
                text(header_focus ? "▶ " : "  ")
                    | color(hdr_fg),
                text(is_collapsed ? "▸ " : "▾ ")
                    | color(Color::Cyan) | bold,
                text("📁 ") | dim,
                text(p.name) | bold | color(Color::Blue),
                text(std::format(" ({} sessions)", p.sessions.size())) | dim,
                filler(),
                text(p.total_cost_usd > 0
                         ? std::format("${:.2f}", p.total_cost_usd)
                         : "") | color(Color::Green) | dim,
                text("  "),
                text(format_relative(p.last_activity)) | dim,
            });
            if (header_focus) hdr = hdr | bgcolor(Color::RGB(25, 30, 45));
            cards.push_back(hdr | padding(0, 1));
        }
        ++flat_index;

        if (is_collapsed) continue;

        for (std::size_t si = 0; si < p.sessions.size(); ++si) {
            const bool row_focus = (flat_index == s.project_cursor);
            const auto& e = p.sessions[si];
            const bool in_batch = s.selected_ids.count(e.id) > 0;
            cards.push_back(ui19_style::RenderSessionCard(
                e, row_focus || in_batch, row_focus, e.pinned));
            cards.push_back(text(" "));
            ++flat_index;
        }
    }

    return vbox(std::move(cards))
         | yframe | vscroll_indicator | flex;
}

/// Archive tab: YYYY-MM timeline groups → session rows.
[[nodiscard]] inline Element RenderArchive(SelectorState& s) {
    Elements cards;
    cards.push_back(hbox({
        text(" Archive ") | bold | color(Color::GrayLight),
        text("(read-only — use [r] to restore)") | dim,
        filler(),
    }));
    cards.push_back(separator());
    cards.push_back(RenderFilterChips(s));
    cards.push_back(separator());

    // Compute total rows across groups for cursor clamp.
    std::size_t total = 0;
    for (const auto& [_, v] : s.archive_groups) total += 1 + v.size();
    if (s.archive_cursor >= total && total > 0)
        s.archive_cursor = total - 1;

    std::size_t flat = 0;
    for (const auto& [month, rows] : s.archive_groups) {
        const bool hdr_sel = (flat == s.archive_cursor);
        Color hdr_col = hdr_sel ? Color(Color::Green) : Color(Color::Default);
        cards.push_back(hbox({
            text(hdr_sel ? "▶ " : "  ")
                | color(hdr_col),
            text(" 📅 ") | dim,
            text(month) | bold | color(Color::Yellow),
            text(std::format(" ({} entries)", rows.size())) | dim,
            filler(),
        }) | padding(0, 1));
        ++flat;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const bool sel = (flat == s.archive_cursor);
            const auto& e = s.all_sessions[rows[i]];
            const bool in_batch = s.selected_ids.count(e.id) > 0;
            cards.push_back(ui19_style::RenderSessionCard(
                e, sel || in_batch, sel, e.pinned));
            if (i + 1 < rows.size()) cards.push_back(text(" "));
            ++flat;
        }
    }
    return vbox(std::move(cards))
         | yframe | vscroll_indicator | flex;
}

// =========================================================================
// 10. Right-side preview panel (10-msg compressed view + metadata)
// =========================================================================

[[nodiscard]] inline Element RenderPreviewPanel(SelectorState& s) {
    const SessionLogEntry* entry = nullptr;
    if (s.tab == Tab::Projects) {
        // Walk projects to find the cursor.
        std::size_t flat = 0;
        for (auto& p : s.projects) {
            const bool collapsed = std::any_of(
                s.collapsed_projects.begin(), s.collapsed_projects.end(),
                [&](const auto& n) { return n == p.name; });
            ++flat;
            if (collapsed) continue;
            for (auto& e : p.sessions) {
                if (flat == s.project_cursor) entry = &e;
                ++flat;
            }
        }
    } else if (s.tab == Tab::Archive) {
        std::size_t flat = 0;
        for (const auto& [_, rows] : s.archive_groups) {
            ++flat;
            for (const auto i : rows) {
                if (flat == s.archive_cursor) entry = &s.all_sessions[i];
                ++flat;
            }
        }
    } else {
        entry = current_entry(s);
    }

    Elements body;
    body.push_back(hbox({
        text("🔎 Preview") | bold | color(Color::Blue),
        filler(),
    }));
    body.push_back(separator());

    if (!entry) {
        body.push_back(text(""));
        body.push_back(text(" Nothing selected.") | dim | center);
        body.push_back(text(""));
        return vbox(std::move(body)) | borderRounded | padding(1);
    }

    // Lazy load preview messages
    if ((!entry->preview_messages
         || entry->preview_messages->empty())
        && s.opts.load_preview)
    {
        entry->preview_messages = s.opts.load_preview(entry->id, 10);
    }

    // Metadata
    body.push_back(hbox({
        text(truncate(entry->title, 30)) | bold | color(Color::Green),
    }));
    body.push_back(hbox({
        text(std::string{to_string(entry->status)})
            | color(status_color(entry->status)) | bold,
        entry->pinned ? text(" 📌") : text(""),
        filler(),
        text("#" + entry->id.substr(0, 8)) | dim,
    }));
    body.push_back(separatorLight());

    body.push_back(hbox({
        text("Model:   ") | dim,
        text(entry->model ? *entry->model : "-") | color(Color::Cyan),
    }));
    body.push_back(hbox({
        text("Msgs:    ") | dim,
        text(std::to_string(entry->msg_count)) | color(Color::White),
        text("  Turns: ") | dim,
        text(std::to_string(entry->turns)),
    }));
    body.push_back(hbox({
        text("Cost:    ") | dim,
        text(std::format("${:.3f}", entry->cost_usd)) | color(Color::Green),
        text("  Tokens: ") | dim,
        text(std::format("{}k/{}k",
            (entry->tokens_in + 500) / 1000,
            (entry->tokens_out + 500) / 1000)) | dim,
    }));
    body.push_back(hbox({
        text("Updated: ") | dim,
        text(format_relative(entry->updated)),
    }));
    if (entry->project) {
        body.push_back(hbox({
            text("Project: ") | dim,
            text(*entry->project) | color(Color::Yellow),
        }));
    }
    if (!entry->tags.empty()) {
        Elements tags_els;
        for (const auto& t : entry->tags) {
            tags_els.push_back(text(" " + t + " ") | borderLight | color(Color::Cyan));
        }
        body.push_back(hbox({
            text("Tags:    ") | dim,
            hbox(std::move(tags_els)),
        }));
    }
    body.push_back(separatorLight());

    // Messages (compressed)
    body.push_back(hbox({
        text("Last 10 messages") | bold | color(Color::Blue),
    }));
    body.push_back(text(""));

    if (!entry->preview_messages || entry->preview_messages->empty()) {
        body.push_back(text(" (no preview)") | dim);
    } else {
        for (const auto& [role, text_] : *entry->preview_messages) {
            Color col = Color::White;
            std::string icon = "  ";
            if (role == "user")      { col = Color::Cyan;   icon = "👤 "; }
            else if (role == "assistant") { col = Color::Green;  icon = "🤖 "; }
            else if (role == "tool")      { col = Color::Orange1;icon = "🔧 "; }
            else if (role == "system")    { col = Color::Yellow; icon = "ℹ "; }
            body.push_back(hbox({
                text(icon) | color(col),
                text(truncate(text_, 60)) | color(col) | dim,
            }));
        }
    }

    return vbox(std::move(body))
         | borderRounded | padding(1)
         | size(WIDTH, EQUAL, 42);
}

// =========================================================================
// 11. Footer toolbar
// =========================================================================

[[nodiscard]] inline Element RenderFooter(const SelectorState& s) {
    auto pill = [](std::string_view k, std::string_view label, Color c) {
        return hbox({
            text(" [") | dim,
            text(std::string{k}) | bold | color(c),
            text("] ") | dim,
            text(std::string{label}) | dim,
        });
    };
    Element row = hbox({
        pill("a", "archive", Color::Yellow),
        pill("d", "delete",  Color::Red),
        pill("r", "restore", Color::Green),
        pill("e", "export",  Color::Cyan),
        pill("i", "import",  Color::Cyan),
        pill("p", "pin",     Color::Blue),
        pill("t", "tag",     Color::Magenta),
        filler(),
        pill("/", "search",  Color::Cyan),
        pill("Enter", "open", Color::Green),
        pill("Esc/q", "close", Color::Red),
    });
    std::string extra;
    if (s.visual_mode)            extra += "  VISUAL-MODE (v to exit) ";
    if (!s.selected_ids.empty())  extra += std::format("  {} selected",
                                                       s.selected_ids.size());
    return vbox({
        separatorLight(),
        row,
        extra.empty() ? text("") : text(extra) | color(Color::Yellow) | dim,
    });
}

// =========================================================================
// 12. Import dialog
// =========================================================================

[[nodiscard]] inline Element RenderImportDialog(SelectorState& s) {
    Elements body;
    body.push_back(hbox({
        text("⬇ Import JSONL session file") | bold | color(Color::Cyan),
    }));
    body.push_back(separator());
    body.push_back(text(" Path to .jsonl / .json export:") | dim);
    body.push_back(text(""));
    Element field = hbox({
        text(" 📄 ") | dim,
        text(s.import_path.empty() ? std::string{"(enter path)"} : s.import_path)
            | color(s.import_error ? Color::Red : Color::White),
        text("│") | blink | color(Color::Cyan),
    });
    if (s.import_error) field = field | borderStyled(Color::Red)
                                       | bgcolor(Color::RGB(40, 15, 15));
    else                field = field | borderLight;
    body.push_back(field | padding(1));
    if (s.import_error) {
        body.push_back(hbox({
            text(" ⚠ ") | color(Color::Red),
            text("File not found — enter a valid path.") | color(Color::Red),
        }));
    }
    body.push_back(text(""));
    body.push_back(separatorLight());
    body.push_back(hbox({
        text(" Enter") | bold | color(Color::Green),
        text(" import   "),
        text("Esc") | bold | color(Color::Red),
        text(" cancel   "),
        text("Tab") | bold | dim,
        text(" paste (clipboard path via manual input)") | dim,
    }));
    return vbox(std::move(body)) | borderRounded | padding(1)
         | size(WIDTH, EQUAL, 60) | center;
}

// =========================================================================
// 13. Top-level render dispatch
// =========================================================================

[[nodiscard]] inline Element RenderLogSelector(SelectorState& s) {
    Element main;
    switch (s.tab) {
        case Tab::Recent:
        case Tab::Drafts:
        case Tab::Remote:
        case Tab::Trash:
            main = RenderCardGrid(s);
            break;
        case Tab::Projects:
            main = RenderProjects(s);
            break;
        case Tab::Archive:
            main = RenderArchive(s);
            break;
    }

    Element layout = hbox({
        RenderSidebar(s),
        main | flex,
        RenderPreviewPanel(s),
    });

    Element full = vbox({
        hbox({
            text(" 📚 Log Selector ") | bold | color(Color::Green),
            filler(),
            text(s.search_focused ? " SEARCH " : " LIST ")
                | color(Color::Cyan) | bold,
            s.import_open ? text(" · IMPORT") | color(Color::Yellow) | bold
                          : text(""),
        }),
        separator(),
        layout | flex,
        RenderFooter(s),
    });

    if (s.import_open) {
        return dbox({
            full | dim,
            RenderImportDialog(s) | clear_under | center,
        });
    }
    return full;
}

// =========================================================================
// 14. Batch helpers: collect selection IDs, TrustDialog builders,
//     toggle in-batch helper.
// =========================================================================

/// Collect currently-selected IDs.  If nothing explicitly selected, returns
/// the single cursor row (acts like a default selection for ops).
[[nodiscard]] inline std::vector<std::string> collect_selection(
    SelectorState& s)
{
    if (!s.selected_ids.empty()) {
        return { s.selected_ids.begin(), s.selected_ids.end() };
    }
    // Fallback: use cursor row
    std::vector<std::string> out;
    auto add = [&](const SessionLogEntry* e) { if (e) out.push_back(e->id); };

    if (s.tab == Tab::Projects) {
        std::size_t flat = 0;
        for (auto& p : s.projects) {
            const bool collapsed = std::any_of(
                s.collapsed_projects.begin(), s.collapsed_projects.end(),
                [&](const auto& n) { return n == p.name; });
            ++flat;
            if (collapsed) continue;
            for (auto& e : p.sessions) {
                if (flat == s.project_cursor) out.push_back(e.id);
                ++flat;
            }
        }
    } else if (s.tab == Tab::Archive) {
        std::size_t flat = 0;
        for (auto& [_, rows] : s.archive_groups) {
            ++flat;
            for (auto i : rows) {
                if (flat == s.archive_cursor)
                    out.push_back(s.all_sessions[i].id);
                ++flat;
            }
        }
    } else {
        add(current_entry(s));
    }
    return out;
}

inline void toggle_selected(SelectorState& s, const std::string& id) {
    if (s.selected_ids.count(id)) s.selected_ids.erase(id);
    else s.selected_ids.insert(id);
}

inline void toggle_selected_range(SelectorState& s,
                                  std::size_t a, std::size_t b)
{
    if (a > b) std::swap(a, b);
    for (std::size_t i = a; i <= b && i < s.view.size(); ++i) {
        toggle_selected(s, s.all_sessions[s.view[i]].id);
    }
}

/// Build a TrustDialogProps for delete / purge.  The caller is expected to
/// overlay UI8's TrustDialog; we just pre-format the summary + risk level.
[[nodiscard]] inline TrustDialogProps MakeDeleteTrustProps(
    SelectorState& s, bool purge)
{
    const auto ids = collect_selection(s);
    TrustDialogProps p;
    p.action = tu::ActionType::PathWrite;
    p.action_label = purge ? "Purge permanently" : "Delete sessions";
    p.forced_level = purge ? RiskLevel::High :
                    (ids.size() > 10 ? RiskLevel::Medium : RiskLevel::Low);
    tu::RiskSummary summary;
    summary.level = p.forced_level;
    std::string fmt_template = purge
        ? "You are about to PERMANENTLY purge {} session(s)."
        : "You are about to soft-delete {} session(s).";
    const auto n_ids = static_cast<unsigned long>(ids.size());
    summary.action_summary = std::vformat(
        fmt_template, std::make_format_args(n_ids));
    summary.risk_factors.push_back(
        purge ? "Action is irreversible.  5-second countdown enforced."
              : "Sessions move to Trash and will be auto-purged in 30 days.");
    if (ids.size() > 10) {
        summary.risk_factors.push_back(
            std::format("Bulk operation affecting {} items", ids.size()));
    }
    p.summary = std::move(summary);
    for (const auto& i : ids) p.paths.push_back(std::format("[session:{}]", i));
    p.on_done = [&s, purge, ids](TrustChoice c) mutable {
        if (c == TrustChoice::AllowOnce || c == TrustChoice::AlwaysAllow
            || c == TrustChoice::EnableAnyway)
        {
            if (s.opts.on_batch) {
                s.opts.on_batch(purge ? BatchOp::Purge : BatchOp::Delete,
                                std::move(ids));
            }
        }
        // Dismiss import-open overlay (defensive, shouldn't be up).
        s.import_open = false;
    };
    return p;
}

// =========================================================================
// 15. Event handler  (navigation + batch ops + filter + import)
// =========================================================================

inline bool HandleEvents(SelectorState& s, Event event) {
    // ──────────────────────────────────────────────────────────────────
    // Import dialog modal — intercepts most keystrokes
    // ──────────────────────────────────────────────────────────────────
    if (s.import_open) {
        if (event == Event::Escape) {
            s.import_open = false;
            s.import_path.clear();
            s.import_error = false;
            return true;
        }
        if (event == Event::Return) {
            // Validate existence.
            bool ok = false;
            std::error_code ec;
            if (!s.import_path.empty()) {
                ok = std::filesystem::exists(s.import_path, ec);
            }
            if (!ok) { s.import_error = true; return true; }
            if (s.opts.on_import) s.opts.on_import(s.import_path);
            s.import_open = false;
            s.import_path.clear();
            s.import_error = false;
            return true;
        }
        if (event == Event::Backspace) {
            if (!s.import_path.empty()) s.import_path.pop_back();
            s.import_error = false;
            return true;
        }
        if (event.is_character() && event.character().size() == 1) {
            const char c = event.character()[0];
            if (c >= 0x20 && c < 0x7f) {
                s.import_path.push_back(c);
                s.import_error = false;
                return true;
            }
        }
        return false;
    }

    // ──────────────────────────────────────────────────────────────────
    // Search focus — captures printable + backspace + return
    // ──────────────────────────────────────────────────────────────────
    if (s.search_focused) {
        if (event == Event::Escape || event == Event::Return) {
            s.search_focused = false;
            refresh_view(s);
            return true;
        }
        if (event == Event::Backspace) {
            if (!s.search.empty()) s.search.pop_back();
            refresh_view(s);
            return true;
        }
        if (event.is_character() && event.character().size() == 1) {
            const char c = event.character()[0];
            if (c >= 0x20 && c < 0x7f) {
                s.search.push_back(c);
                refresh_view(s);
                return true;
            }
        }
        return false;
    }

    // ──────────────────────────────────────────────────────────────────
    // Global: close
    // ──────────────────────────────────────────────────────────────────
    if (event == Event::Escape || event == Event::Character('q')
        || event == Event::Character('Q'))
    {
        if (s.opts.on_cancel) s.opts.on_cancel();
        return true;
    }

    // ──────────────────────────────────────────────────────────────────
    // Global: search toggle / filter focus
    // ──────────────────────────────────────────────────────────────────
    if (event == Event::Character('/')) {
        s.search_focused = true;
        s.search.clear();
        return true;
    }
    if (event == Event::Character('f') || event == Event::Character('F')) {
        const auto chips = MakeChips(s);
        s.filter_chip_cursor =
            (s.filter_chip_cursor + 1) %
            std::max<std::size_t>(1, chips.size());
        return true;
    }

    // ──────────────────────────────────────────────────────────────────
    // Tab switching: 1…6
    // ──────────────────────────────────────────────────────────────────
    if (event.is_character()) {
        const char c = event.character()[0];
        if (c >= '1' && c <= '6') {
            s.tab = static_cast<Tab>(c - '1');
            s.cursor = 0;
            s.project_cursor = 0;
            s.archive_cursor = 0;
            refresh_view(s);
            return true;
        }
    }

    // ──────────────────────────────────────────────────────────────────
    // New session
    // ──────────────────────────────────────────────────────────────────
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (s.opts.on_new_session) s.opts.on_new_session();
        return true;
    }

    // ──────────────────────────────────────────────────────────────────
    // Select-all + clear selection + visual mode
    // ──────────────────────────────────────────────────────────────────
    if (event == Event::Special({1})) {
        s.selected_ids.clear();
        for (const auto& e : s.all_sessions) {
            // Only include entries that match the current tab semantics.
            Status p = primary_status_for_tab(s.tab);
            if (s.tab == Tab::Recent && e.status != Status::Active) continue;
            if (s.tab != Tab::Recent && s.tab != Tab::Projects
                && s.tab != Tab::Remote && e.status != p) continue;
            s.selected_ids.insert(e.id);
        }
        return true;
    }
    if (event == Event::Character('u') || event == Event::Character('U')) {
        s.selected_ids.clear();
        s.visual_mode = false;
        return true;
    }
    if (event == Event::Character('v') || event == Event::Character('V')) {
        s.visual_mode = !s.visual_mode;
        if (s.tab == Tab::Recent || s.tab == Tab::Drafts
            || s.tab == Tab::Remote || s.tab == Tab::Trash)
        {
            s.visual_anchor = s.cursor;
        }
        return true;
    }

    // ──────────────────────────────────────────────────────────────────
    // Navigation: arrow + j/k + Shift+j/k (range) + Shift+↑/↓ pin reorder
    // ──────────────────────────────────────────────────────────────────
    const bool flat_tab = (s.tab == Tab::Recent || s.tab == Tab::Drafts
                        || s.tab == Tab::Remote || s.tab == Tab::Trash);

    auto shift_move = [&](bool down) {
        // FTXUI reports Shift+Arrow via `Shift+ArrowUp` / `Shift+ArrowDown`
        // strings, but simpler bindings are: 'J' / 'K' for range select
        // and '{' / '}' for pin reorder (mapped per spec "Shift+↑/↓").
        if (flat_tab) {
            if (down) { if (s.cursor + 1 < s.view.size()) ++s.cursor; }
            else      { if (s.cursor > 0) --s.cursor; }
            if (s.visual_mode) {
                // Re-select the whole range each keypress (simple).
                s.selected_ids.clear();
                auto [lo, hi] = std::minmax(s.visual_anchor, s.cursor);
                for (std::size_t i = lo; i <= hi && i < s.view.size(); ++i) {
                    s.selected_ids.insert(s.all_sessions[s.view[i]].id);
                }
            }
        } else if (s.tab == Tab::Projects) {
            if (down) s.project_cursor++; else s.project_cursor--;
        } else if (s.tab == Tab::Archive) {
            if (down) s.archive_cursor++; else s.archive_cursor--;
        }
    };

    if (event == Event::ArrowDown || event == Event::Character('j'))
    { shift_move(true); return true; }
    if (event == Event::ArrowUp || event == Event::Character('k'))
    { shift_move(false); return true; }

    // Shift+arrow / J / K range extend
    if (event == Event::Character('J')
        || event.input() == "\x1b[1;2B")  // Shift+Down xterm
    {
        s.visual_mode = true;
        if (s.view.size() > s.cursor + 1) {
            s.visual_anchor = s.cursor;
            ++s.cursor;
            if (flat_tab) toggle_selected(
                s, s.all_sessions[s.view[s.cursor]].id);
        }
        return true;
    }
    if (event == Event::Character('K')
        || event.input() == "\x1b[1;2A")  // Shift+Up xterm
    {
        s.visual_mode = true;
        if (s.cursor > 0) {
            s.visual_anchor = s.cursor;
            --s.cursor;
            if (flat_tab) toggle_selected(
                s, s.all_sessions[s.view[s.cursor]].id);
        }
        return true;
    }

    // Pin reorder: Shift+↑/↓ (handled via { / } as well since many terminals
    // don't emit distinct Shift+Arrow sequences).
    if (flat_tab && (event == Event::Character('{')
                     || event.input() == "\x1b[1;2A_pin"))
    {
        if (s.view.size() > 1 && s.cursor > 0) {
            // Swap only pinned with pinned to preserve semantics.
            auto& cur = s.all_sessions[s.view[s.cursor]];
            auto& prev = s.all_sessions[s.view[s.cursor - 1]];
            if (cur.pinned && prev.pinned) std::swap(cur, prev);
            --s.cursor;
        }
        return true;
    }
    if (flat_tab && (event == Event::Character('}')
                     || event.input() == "\x1b[1;2B_pin"))
    {
        if (s.cursor + 1 < s.view.size()) {
            auto& cur = s.all_sessions[s.view[s.cursor]];
            auto& nxt = s.all_sessions[s.view[s.cursor + 1]];
            if (cur.pinned && nxt.pinned) std::swap(cur, nxt);
            ++s.cursor;
        }
        return true;
    }

    // PgUp / PgDn (≈ 10 rows)
    if (flat_tab && event == Event::PageUp) {
        s.cursor = (s.cursor >= 10) ? s.cursor - 10 : 0;
        return true;
    }
    if (flat_tab && event == Event::PageDown) {
        s.cursor = std::min(
            s.view.empty() ? 0 : s.view.size() - 1, s.cursor + 10);
        return true;
    }

    // ──────────────────────────────────────────────────────────────────
    // Enter = open current
    // ──────────────────────────────────────────────────────────────────
    if (event == Event::Return) {
        const auto ids = collect_selection(s);
        if (!ids.empty() && s.opts.on_open) s.opts.on_open(ids.front());
        return true;
    }

    // ──────────────────────────────────────────────────────────────────
    // Batch toolbar keys: a/d/r/p/t/e/i
    // ──────────────────────────────────────────────────────────────────
    const auto emit_batch = [&](BatchOp op) {
        const auto ids = collect_selection(s);
        if (s.opts.on_batch && !ids.empty()) s.opts.on_batch(op, ids);
    };

    if (event == Event::Character('a') || event == Event::Character('A')) {
        emit_batch(BatchOp::Archive); return true;
    }
    if (event == Event::Character('d') || event == Event::Character('D')) {
        const auto ids = collect_selection(s);
        if (ids.empty()) return true;
        if (s.opts.request_trust_dialog) {
            // >10 → Medium; else Low.
            s.opts.request_trust_dialog(MakeDeleteTrustProps(s, false));
        } else if (s.opts.on_batch) {
            s.opts.on_batch(BatchOp::Delete, ids);
        }
        return true;
    }
    if (event == Event::Character('r') || event == Event::Character('R')) {
        emit_batch(BatchOp::Restore); return true;
    }
    if (event == Event::Character('p') || event == Event::Character('P')) {
        emit_batch(BatchOp::PinToggle); return true;
    }
    if (event == Event::Character('t') || event == Event::Character('T')) {
        emit_batch(BatchOp::TagAssign); return true;
    }
    if (event == Event::Character('e') || event == Event::Character('E')) {
        const auto ids = collect_selection(s);
        if (s.opts.on_export && !ids.empty()) s.opts.on_export(ids);
        return true;
    }
    if (event == Event::Character('i') || event == Event::Character('I')) {
        s.import_open = true;
        s.import_path.clear();
        s.import_error = false;
        return true;
    }

    // Purge (Shift+X) — only meaningful on Trash tab but exposed globally
    // so the UI8 path is exercised consistently.
    if (event == Event::Character('X')) {
        const auto ids = collect_selection(s);
        if (ids.empty()) return true;
        if (s.opts.request_trust_dialog) {
            s.opts.request_trust_dialog(MakeDeleteTrustProps(s, true));
        } else if (s.opts.on_batch) {
            s.opts.on_batch(BatchOp::Purge, ids);
        }
        return true;
    }

    // ──────────────────────────────────────────────────────────────────
    // Project-tab: toggle collapse on current (Space / Enter on header)
    // Archive-tab: open / restore same semantics
    // ──────────────────────────────────────────────────────────────────
    if (event == Event::Character(' ')) {
        if (s.tab == Tab::Projects) {
            std::size_t flat = 0;
            for (auto& p : s.projects) {
                const bool collapsed = std::any_of(
                    s.collapsed_projects.begin(), s.collapsed_projects.end(),
                    [&](const auto& n) { return n == p.name; });
                if (flat == s.project_cursor) {
                    // Toggle collapse.
                    if (collapsed) {
                        s.collapsed_projects.erase(
                            std::remove(s.collapsed_projects.begin(),
                                        s.collapsed_projects.end(), p.name),
                            s.collapsed_projects.end());
                    } else {
                        s.collapsed_projects.push_back(p.name);
                    }
                    return true;
                }
                ++flat;
                if (collapsed) continue;
                for (std::size_t si = 0; si < p.sessions.size(); ++si) {
                    if (flat == s.project_cursor) {
                        toggle_selected(s, p.sessions[si].id);
                        return true;
                    }
                    ++flat;
                }
            }
        } else if (flat_tab) {
            if (const auto* e = current_entry(s)) {
                toggle_selected(s, e->id);
            }
            return true;
        } else if (s.tab == Tab::Archive) {
            std::size_t flat = 0;
            for (auto& [_, rows] : s.archive_groups) {
                ++flat;
                for (auto i : rows) {
                    if (flat == s.archive_cursor) {
                        toggle_selected(s, s.all_sessions[i].id);
                        return true;
                    }
                    ++flat;
                }
            }
        }
    }

    return false;
}

// =========================================================================
// 16. Public factory
// =========================================================================

/// Build the full LogSelector screen as a self-contained FTXUI component.
/// All data access flows through the lambdas in `options`; no state is read
/// from any business module directly (keeps the module UI-only as required
/// by the P0 migration spec).
[[nodiscard]] inline Component MakeLogSelector(LogSelectorOptions options)
{
    auto state = std::make_shared<SelectorState>();
    state->opts = std::move(options);
    state->tab  = state->opts.initial_tab;

    // Prime data sources once.  (Callers are expected to re-mount the
    // component if they want to react to provider churn; alternatively the
    // Remote tab can call `refresh_view` after `on_fetch_remote` completes
    // because the provider will return the extended list next time.)
    if (state->opts.sessions_provider) {
        state->all_sessions = state->opts.sessions_provider();
    }
    if (state->opts.projects_provider) {
        state->projects = state->opts.projects_provider();
    }
    refresh_view(*state);

    auto base = Renderer([state] { return RenderLogSelector(*state); });
    return base | CatchEvent([state](Event ev) {
        return HandleEvents(*state, std::move(ev));
    });
}

} // namespace cc::ui::screens::log_selector
