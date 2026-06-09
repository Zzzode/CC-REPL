/// @file task_list_view.cppm
/// @brief Full task list view with Table + Kanban modes, filter/search
///   toolbar, sortable columns, summary footer with progress, keyboard
///   navigation, and virtual scroll for 500+ tasks.
///   Supersedes the skeleton in cc.ui.components.task_view.
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
#include <ranges>
#include <set>
#include <unordered_set>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/input.hpp>
#include <ftxui/component/menu.hpp>
#include <ftxui/component/toggle.hpp>
#include <ftxui/component/screen_interactive.hpp>

export module cc.ui.tasks.task_list_view;

import cc.ui.tasks.task_components;

export namespace cc::ui::tasks::list_view {
using namespace ftxui;
namespace comp = cc::ui::tasks::components;

// Re-export commonly-needed types from task_components so downstream users
// only need to import one module for the list subsystem.
using Priority  = comp::Priority;
using Status    = comp::Status;
using Tag       = comp::Tag;
using DueDate   = comp::DueDate;
using SubTask   = comp::SubTask;

// ============================================================
// Data model (UI-facing facade — the engine types live in cc.tasks.*).
// We intentionally keep our own POD for display so the view is decoupled
// from engine lifecycle, and callers can produce these rows from
// cc::tasks::* variants.
// ============================================================

/// Task view-mode discriminator.
enum class ViewMode : std::uint8_t {
    Table,   // Default: spreadsheet-style rows.
    Kanban,  // 4 vertical columns: Todo / InProgress / Review / Done.
};

/// Column identifiers for sort / width-config.
enum class Column : std::uint8_t {
    Checkbox, Priority, Title, Tags, Assignee, DueDate, Progress, Status,
    _Count,
};

/// Sort direction.
enum class SortDir : std::uint8_t { Asc, Desc };

/// A sort key: which column + direction.  Primary/secondary for multi-key.
struct SortSpec {
    Column col = Column::Status;
    SortDir dir = SortDir::Desc;  // "Done" sorts to bottom → Desc means
                                  // status is ordered with most active first.
};

/// A single task row.  All fields optional because callers typically
/// project them from whichever engine state variant they have.
struct TaskRow {
    std::string id;
    std::string title;
    std::string description;
    Status status     = Status::Todo;
    Priority priority = Priority::None;
    std::vector<Tag> tags;
    std::vector<std::string> assignees;
    std::optional<DueDate> due_date;
    double progress    = 0.0;     // 0.0 - 1.0
    bool is_done       = false;
    std::vector<SubTask> subtasks;
    // Index into the "source" vector — used to map selected row back.
    int source_index  = -1;
};

/// List of columns in their display order (Table mode).
inline constexpr Column kColumnOrder[] = {
    Column::Checkbox, Column::Priority, Column::Title, Column::Tags,
    Column::Assignee, Column::DueDate, Column::Progress, Column::Status,
};

/// Default column widths (character counts).  TODO(UI12): allow dragging
/// of column dividers when FTXUI exposes per-character mouse-drag.
inline constexpr int kDefaultWidths[] = {
    5,  // Checkbox
    8,  // Priority
    30, // Title
    18, // Tags
    12, // Assignee
    14, // DueDate
    16, // Progress
    14, // Status
};

[[nodiscard]] constexpr int col_width(Column c) {
    return kDefaultWidths[static_cast<std::uint8_t>(c)];
}

[[nodiscard]] constexpr std::string_view col_label(Column c) {
    switch (c) {
        case Column::Checkbox: return "☑";
        case Column::Priority: return "Prio";
        case Column::Title:    return "Title";
        case Column::Tags:     return "Tags";
        case Column::Assignee: return "Assignee";
        case Column::DueDate:  return "Due";
        case Column::Progress: return "Progress";
        case Column::Status:   return "Status";
        case Column::_Count:   return "";
    }
    return "";
}

// ============================================================
// Callbacks
// ============================================================

struct ListCallbacks {
    /// Fired when the user toggles a row's "done" checkbox.
    std::function<void(const std::string& task_id, bool done)> on_done_toggle;
    /// Fired when a row is opened (Enter / double click).
    std::function<void(const std::string& task_id)> on_open_details;
    /// Fired when user presses `a` or clicks "+ Add".
    std::function<void()> on_new_task;
    /// Fired when user presses `d` on a row.
    std::function<void(const std::string& task_id)> on_delete;
    /// Fired when user moves a task between Kanban columns (dragging is
    /// emulated via keyboard: `m` followed by 1-4).
    std::function<void(const std::string& task_id, Status new_status)> on_move_status;
};

// ============================================================
// Filter state
// ============================================================

struct FilterState {
    std::string search_text;                      // fuzzy title/desc match
    std::optional<Status> status_filter;          // std::nullopt = All
    std::set<Priority> priority_toggles;          // empty = all priorities
    std::optional<std::string> assignee_filter;   // std::nullopt = All
    std::optional<std::string> tag_filter;        // std::nullopt = All
};

/// Returns true if a task row passes the active filters.
[[nodiscard]] inline bool passes_filter(const TaskRow& row,
                                        const FilterState& f) {
    if (f.status_filter && row.status != *f.status_filter) return false;
    if (!f.priority_toggles.empty() &&
        !f.priority_toggles.contains(row.priority)) return false;
    if (f.assignee_filter) {
        bool match = false;
        for (auto& a : row.assignees) {
            if (a.find(*f.assignee_filter) != std::string::npos) { match = true; break; }
        }
        if (!match) return false;
    }
    if (f.tag_filter) {
        bool match = false;
        for (auto& t : row.tags) {
            if (t.text.find(*f.tag_filter) != std::string::npos) { match = true; break; }
        }
        if (!match) return false;
    }
    if (!f.search_text.empty()) {
        auto q = f.search_text;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        auto hay = row.title + " " + row.description;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (hay.find(q) == std::string::npos) return false;
    }
    return true;
}

// ============================================================
// Sorting
// ============================================================

/// Comparator that implements "Status > Priority > Due > Title" hierarchy
/// and honours the secondary sort dir.
struct RowCompare {
    const std::vector<SortSpec>& sort;
    explicit RowCompare(const std::vector<SortSpec>& s) : sort(s) {}

    [[nodiscard]] int cmp_col(Column col, const TaskRow& a, const TaskRow& b) const {
        switch (col) {
            case Column::Status:
                return static_cast<int>(a.status) - static_cast<int>(b.status);
            case Column::Priority:
                return static_cast<int>(a.priority) - static_cast<int>(b.priority);
            case Column::Title:
                return a.title.compare(b.title);
            case Column::DueDate: {
                if (!a.due_date && !b.due_date) return 0;
                if (!a.due_date) return 1;
                if (!b.due_date) return -1;
                return (a.due_date->date < b.due_date->date) ? -1
                     : (a.due_date->date > b.due_date->date) ?  1 : 0;
            }
            case Column::Progress:
                return a.progress < b.progress ? -1 : a.progress > b.progress ? 1 : 0;
            case Column::Checkbox:
                return (a.is_done ? 1 : 0) - (b.is_done ? 1 : 0);
            case Column::Assignee: {
                auto av = a.assignees.empty() ? std::string{} : a.assignees.front();
                auto bv = b.assignees.empty() ? std::string{} : b.assignees.front();
                return av.compare(bv);
            }
            case Column::Tags: {
                auto av = a.tags.empty() ? std::string{} : a.tags.front().text;
                auto bv = b.tags.empty() ? std::string{} : b.tags.front().text;
                return av.compare(bv);
            }
            case Column::_Count: return 0;
        }
        return 0;
    }

    bool operator()(const TaskRow* a, const TaskRow* b) const {
        for (auto& spec : sort) {
            int r = cmp_col(spec.col, *a, *b);
            if (r != 0) {
                return spec.dir == SortDir::Asc ? (r < 0) : (r > 0);
            }
        }
        return false;
    }
};

// ============================================================
// Filter / search toolbar
// ============================================================

struct ToolbarState {
    // Values kept inline with FilterState, but the toolbar owns the
    // interactive components and pushes updates out.
    std::string search_text;
    int status_selected = 0;  // index into kStatusOptions
    std::set<Priority> priority_toggles;
    int assignee_selected = 0;  // index into assignee dropdown
};

inline const std::vector<std::string> kStatusOptions = {
    "All", "Todo", "InProgress", "Review", "Done", "Cancelled"
};

[[nodiscard]] inline std::optional<Status> status_from_index(int idx) {
    switch (idx) {
        case 0: return std::nullopt;
        case 1: return Status::Todo;
        case 2: return Status::InProgress;
        case 3: return Status::Review;
        case 4: return Status::Done;
        case 5: return Status::Cancelled;
    }
    return std::nullopt;
}

/// Render the toolbar element: search + status dropdown + priority chips
/// + assignee dropdown + tag tabs.
[[nodiscard]] inline Element RenderToolbar(
    const ToolbarState& tb,
    const std::vector<std::string>& assignees,
    const std::vector<std::string>& tag_values,
    int active_tag_tab) {

    using comp::Priority;

    // Search input
    auto search = hbox({
        text("🔎 "),
        text(tb.search_text) | size(WIDTH, GREATER_THAN, 14)
            | borderEmpty,
        text(tb.search_text.empty() ? " (type / to search)" : ""),
    }) | size(WIDTH, EQUAL, 36);

    // Status dropdown
    auto status_label = kStatusOptions[std::min(tb.status_selected,
        static_cast<int>(kStatusOptions.size()) - 1)];
    auto status_box = hbox({
        text("Status: ") | dim,
        text(status_label) | bold | color(Color::Blue) | borderEmpty,
    }) | size(WIDTH, EQUAL, 16);

    // Priority chips (toggle-style)
    constexpr Priority kPrios[] = {
        Priority::Urgent, Priority::High, Priority::Medium,
        Priority::Low, Priority::None,
    };
    Elements prio_chips;
    for (auto p : kPrios) {
        auto emoji = comp::priority_emoji(p);
        auto on = tb.priority_toggles.contains(p);
        auto chip = text(" " + emoji + " ")
            | bgcolor(on ? comp::priority_color(p) : Color::GrayDark)
            | (on ? color(Color::Black) : color(Color::White));
        prio_chips.push_back(chip);
        prio_chips.push_back(text(" "));
    }
    auto prio_row = hbox(std::move(prio_chips));

    // Assignee dropdown (compact: show only name)
    std::string assignee_name =
        tb.assignee_selected == 0 || assignees.empty()
            ? "All"
            : assignees[std::min(tb.assignee_selected - 1,
                                 static_cast<int>(assignees.size()) - 1)];
    auto assignee_box = hbox({
        text("👤 ") | dim,
        text(assignee_name) | bold | borderEmpty,
    }) | size(WIDTH, EQUAL, 14);

    auto line1 = hbox({
        search, text("  "), status_box, text("  "),
        prio_row, text("  "), assignee_box,
        filler(),
    });

    // Tag tabs (second line)
    Elements tag_els;
    tag_els.push_back(text("Tags: ") | dim);
    // "All" tab
    tag_els.push_back(
        text(" All ") | (active_tag_tab == 0
            ? (bold | color(Color::White) | bgcolor(Color::Cyan) | inverted)
            : (dim | borderEmpty)));
    int shown = std::min(static_cast<int>(tag_values.size()), 8);
    for (int i = 0; i < shown; ++i) {
        int tab_idx = i + 1;
        auto& tv = tag_values[i];
        tag_els.push_back(text(" "));
        tag_els.push_back(
            text("#" + tv + " ") | (tab_idx == active_tag_tab
                ? (bold | color(Color::White) | bgcolor(Color::Cyan) | inverted)
                : (dim | borderEmpty)));
    }
    if (static_cast<int>(tag_values.size()) > shown) {
        tag_els.push_back(text(std::format(" +{}", tag_values.size() - shown)) | dim);
    }
    auto line2 = hbox(std::move(tag_els)) | borderEmpty;

    return vbox({line1, separatorEmpty(), line2});
}

// ============================================================
// Table view renderer
// ============================================================

/// Render one cell respecting a fixed width.
[[nodiscard]] inline Element cell(std::string text_str, int w,
                                  Decorator decorator = nothing) {
    // Truncate
    if (w < 1) return text("") | size(WIDTH, EQUAL, 1);
    if (static_cast<int>(text_str.size()) > w) {
        text_str = text_str.substr(0, std::max(0, w - 1)) + "…";
    }
    return text(text_str) | size(WIDTH, EQUAL, w) | decorator;
}

[[nodiscard]] inline Element RenderTableHeader(const std::vector<SortSpec>& sort) {
    Elements cells;
    for (auto c : kColumnOrder) {
        std::string label(col_label(c));
        // Append sort arrow if this column is primary.
        if (!sort.empty() && sort.front().col == c) {
            label += sort.front().dir == SortDir::Asc ? "▲" : "▼";
        }
        cells.push_back(cell(label, col_width(c), bold | color(Color::Cyan)));
    }
    return hbox(std::move(cells)) | bgcolor(Color::RGB(20, 25, 40));
}

[[nodiscard]] inline Element RenderTableRow(const TaskRow& row, bool selected,
                                            bool hovered = false) {
    using comp::Priority;
    Elements cells;

    // Checkbox
    cells.push_back(cell(row.is_done ? "[✓]" : "[ ]",
        col_width(Column::Checkbox),
        row.is_done ? color(Color::Green) : color(Color::GrayLight)));

    // Priority
    {
        auto c = comp::priority_color(row.priority);
        auto bar = std::string{"│"};
        cells.push_back(
            hbox({separatorCharacter(bar) | color(c),
                  text(" " + comp::priority_emoji(row.priority) + " ")})
            | size(WIDTH, EQUAL, col_width(Column::Priority)));
    }

    // Title (strikethrough if done)
    auto title_decor = row.is_done
        ? (strikethrough | color(Color::GrayDark))
        : color(Color::White);
    if (selected) title_decor = title_decor | bold;
    cells.push_back(cell(row.title, col_width(Column::Title), title_decor));

    // Tags (first 2)
    {
        std::string tag_str;
        for (std::size_t i = 0; i < row.tags.size() && i < 2; ++i) {
            if (i > 0) tag_str += ",";
            tag_str += row.tags[i].text;
        }
        if (row.tags.size() > 2) {
            tag_str += std::format("+{}", row.tags.size() - 2);
        }
        cells.push_back(cell(tag_str, col_width(Column::Tags),
            color(Color::CyanLight)));
    }

    // Assignees (first + N)
    {
        std::string as;
        if (!row.assignees.empty()) {
            as = row.assignees.front();
            if (row.assignees.size() > 1) {
                as += std::format("+{}", row.assignees.size() - 1);
            }
        }
        cells.push_back(cell(as, col_width(Column::Assignee),
            color(Color::MagentaLight)));
    }

    // Due date
    {
        std::string d;
        Color c = Color::GrayLight;
        if (row.due_date) {
            d = row.due_date->format_iso();
            auto [_, col] = comp::due_urgency_display(row.due_date->urgency());
            c = col;
        }
        cells.push_back(cell(d, col_width(Column::DueDate), color(c)));
    }

    // Progress (mini bar, no label for space)
    {
        int w = col_width(Column::Progress) - 2;
        w = std::max(w, 2);
        int filled = static_cast<int>(row.progress * w);
        int empty  = w - filled;
        std::string bar;
        for (int i = 0; i < filled; ++i) bar += '█';
        for (int i = 0; i < empty;  ++i) bar += '░';
        Color pc = Color::Green;
        if      (row.progress < 0.3) pc = Color::GrayLight;
        else if (row.progress < 0.7) pc = Color::Yellow;
        cells.push_back(hbox({
            text(bar.substr(0, filled)) | color(pc),
            text(bar.substr(filled))    | dim,
        }) | size(WIDTH, EQUAL, col_width(Column::Progress)));
    }

    // Status pill
    cells.push_back(cell(comp::status_label(row.status),
        col_width(Column::Status),
        color(comp::status_pill_fg(row.status)) |
        bgcolor(comp::status_pill_bg(row.status))));

    auto line = hbox(std::move(cells));
    if (selected) line = line | bgcolor(Color::RGB(25, 30, 45));
    else if (hovered) line = line | bgcolor(Color::RGB(15, 20, 35));
    return line;
}

// ============================================================
// Kanban view renderer
// ============================================================

struct KanbanColumn {
    Status status;
    std::string title;
};

inline constexpr KanbanColumn kKanbanColumns[] = {
    { Status::Todo,       "Todo"       },
    { Status::InProgress, "In Progress"},
    { Status::Review,     "Review"     },
    { Status::Done,       "Done"       },
};

[[nodiscard]] inline Element RenderKanbanCard(const TaskRow& row, bool selected) {
    auto card_body = vbox({
        hbox({
            text(" " + comp::priority_emoji(row.priority) + " "),
            text(row.title) | (selected ? bold : color(Color::White))
                | (row.status == Status::Done ? strikethrough : nothing)
                | flex,
        }),
        hbox({
            text(" ") | dim,
            comp::StatusPill(row.status) | flex,
            filler(),
            row.assignees.empty()
                ? text("")
                : comp::AssigneeAvatarStack(row.assignees, 1),
        }),
        (row.progress > 0.0 && row.progress < 1.0)
            ? comp::ProgressBar(row.progress, 12, false)
            : text(""),
        row.due_date
            ? comp::DueDateBadge(*row.due_date)
            : text(""),
    });
    return card_body | border | (selected ? color(Color::Cyan) : nothing);
}

[[nodiscard]] inline Element RenderKanbanColumn(
    const KanbanColumn& col,
    const std::vector<const TaskRow*>& rows,
    int selected_col,
    int this_col_idx,
    int selected_row_in_col) {

    Elements cards;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        bool sel = (this_col_idx == selected_col && i == selected_row_in_col);
        cards.push_back(RenderKanbanCard(*rows[i], sel));
        cards.push_back(separatorEmpty());
    }
    if (cards.empty()) {
        cards.push_back(text("   (empty)") | dim | center);
    }
    cards.push_back(
        hbox({text(" [+] Add ") | color(Color::Cyan) | dim})
        | size(WIDTH, EQUAL, 10) | center);

    auto head = hbox({
        text(" " + col.title + " ") | bold
            | (this_col_idx == selected_col ? color(Color::Cyan) : color(Color::White)),
        text(std::format(" ({}) ", rows.size())) | dim,
        filler(),
    }) | bgcolor(Color::RGB(20, 25, 40));

    auto body = vbox(std::move(cards)) | vscroll_indicator | yframe | flex;

    return vbox({head, separator(), body}) | borderRounded
        | (this_col_idx == selected_col ? color(Color::Cyan) : nothing)
        | flex;
}

// ============================================================
// Summary footer
// ============================================================

struct Summary {
    int total = 0;
    int todo = 0;
    int in_progress = 0;
    int review = 0;
    int done = 0;
    int cancelled = 0;
    double avg_progress = 0.0;  // weighted across non-done
};

[[nodiscard]] inline Summary compute_summary(const std::vector<TaskRow>& rows) {
    Summary s;
    s.total = static_cast<int>(rows.size());
    double prog_sum = 0;
    int prog_n = 0;
    for (auto& r : rows) {
        switch (r.status) {
            case Status::Todo:       ++s.todo; break;
            case Status::InProgress: ++s.in_progress; break;
            case Status::Review:     ++s.review; break;
            case Status::Done:       ++s.done; break;
            case Status::Cancelled:  ++s.cancelled; break;
        }
        if (r.status != Status::Done && r.status != Status::Cancelled) {
            prog_sum += r.progress;
            ++prog_n;
        }
    }
    if (prog_n > 0) s.avg_progress = prog_sum / prog_n;
    else if (s.done == s.total && s.total > 0) s.avg_progress = 1.0;
    return s;
}

[[nodiscard]] inline Element RenderSummaryFooter(const Summary& s) {
    double done_pct = s.total > 0
        ? static_cast<double>(s.done) / s.total : 0.0;
    return hbox({
        text(std::format(" {} total ", s.total)) | bold,
        text("· ") | dim,
        text(std::format(" {} Todo ", s.todo)) | color(Color::GrayLight),
        text("· ") | dim,
        text(std::format(" {} In Progress ", s.in_progress)) | color(Color::Blue),
        text("· ") | dim,
        text(std::format(" {} Review ", s.review)) | color(Color::Purple),
        text("· ") | dim,
        text(std::format(" {} Done ", s.done)) | color(Color::Green),
        text("· ") | dim,
        text(std::format(" {} Cancelled ", s.cancelled)) | color(Color::Red),
        filler(),
        comp::ProgressBar(done_pct, 24),
    });
}

// ============================================================
// Virtual scroll helpers
// ============================================================

/// Threshold at which we enable virtual scrolling (render only visible
/// window + a small buffer).  FTXUI does not have a native virtual list
/// widget, so we compute start/end from selected index + window_height.
inline constexpr std::size_t kVirtualScrollThreshold = 500;
inline constexpr int kVirtualBuffer = 8;

struct VirtualWindow {
    int visible_start = 0;
    int visible_count = 0;  // 0 means "render all"
    int window_height = 20; // fallback estimate
};

[[nodiscard]] inline VirtualWindow compute_virtual_window(
    int total_rows, int selected, int estimated_height) {

    if (total_rows < static_cast<int>(kVirtualScrollThreshold)) {
        return VirtualWindow{0, 0, estimated_height};
    }
    // 1 line/header + 1 summary line = 2 overhead
    int avail = std::max(4, estimated_height - 2);
    int start = std::max(0, selected - avail / 2);
    start = std::min(start, std::max(0, total_rows - avail));
    int end = std::min(total_rows, start + avail + kVirtualBuffer);
    return VirtualWindow{start, end - start, estimated_height};
}

// ============================================================
// Public options + component
// ============================================================

struct TaskListViewOptions {
    std::vector<TaskRow> tasks;
    ViewMode initial_mode = ViewMode::Table;
    ListCallbacks callbacks;
    std::vector<std::string> assignees_catalog;  // for dropdown
    std::vector<std::string> tags_catalog;       // for tag tabs
    // Display estimates — can be refined from Screen size at render time.
    int estimated_list_height = 25;
};

struct TaskListViewState {
    std::vector<TaskRow> tasks;
    ViewMode mode = ViewMode::Table;
    ToolbarState toolbar;
    FilterState filter;
    std::vector<SortSpec> sort;  // primary + optional secondary keys
    int selected = 0;            // index into *filtered & sorted* result
    // Kanban navigation: column + row-within-column
    int kb_column = 0;
    std::vector<int> kb_column_row;  // parallel to kKanbanColumns
    int kb_row_in_col = 0;
    ListCallbacks cb;
    std::vector<std::string> assignees_catalog;
    std::vector<std::string> tags_catalog;
    int active_tag_tab = 0;
    int estimated_height = 25;
    // Movement scratchpad: pending status move.
    bool pending_move = false;
    std::string pending_move_id;

    TaskListViewState() {
        // Default sort: Status > Priority > DueDate > Title
        sort.push_back(SortSpec{Column::Status,   SortDir::Asc});  // Todo first
        sort.push_back(SortSpec{Column::Priority, SortDir::Desc}); // Urgent first
        sort.push_back(SortSpec{Column::DueDate,  SortDir::Asc});  // Soonest first
        sort.push_back(SortSpec{Column::Title,    SortDir::Asc});  // Alpha
        kb_column_row.resize(4, 0);
    }
};

/// Compute filtered + sorted task list (shared logic used by both render
/// and event handlers so selection indices stay consistent).
[[nodiscard]] inline std::vector<const TaskRow*> build_visible(
    const TaskListViewState& s) {
    std::vector<const TaskRow*> out;
    out.reserve(s.tasks.size());
    for (auto& t : s.tasks) {
        if (passes_filter(t, s.filter)) out.push_back(&t);
    }
    std::stable_sort(out.begin(), out.end(), RowCompare(s.sort));
    return out;
}

/// For a given status, return rows belonging to that kanban column.
[[nodiscard]] inline std::vector<const TaskRow*> build_kanban_col(
    const std::vector<const TaskRow*>& visible, Status status) {
    std::vector<const TaskRow*> out;
    for (auto r : visible) if (r->status == status) out.push_back(r);
    return out;
}

/// Create the main component.
[[nodiscard]] inline Component TaskListView(TaskListViewOptions opts) {
    auto s = std::make_shared<TaskListViewState>();
    s->tasks              = std::move(opts.tasks);
    s->mode               = opts.initial_mode;
    s->cb                 = std::move(opts.callbacks);
    s->assignees_catalog  = std::move(opts.assignees_catalog);
    s->tags_catalog       = std::move(opts.tags_catalog);
    s->estimated_height   = opts.estimated_list_height;

    auto sync_filter_from_toolbar = [s]() {
        s->filter.search_text     = s->toolbar.search_text;
        s->filter.status_filter   = status_from_index(s->toolbar.status_selected);
        s->filter.priority_toggles = s->toolbar.priority_toggles;
        if (s->toolbar.assignee_selected == 0 || s->assignees_catalog.empty()) {
            s->filter.assignee_filter = std::nullopt;
        } else {
            auto idx = std::min(s->toolbar.assignee_selected - 1,
                static_cast<int>(s->assignees_catalog.size()) - 1);
            s->filter.assignee_filter = s->assignees_catalog[idx];
        }
        if (s->active_tag_tab == 0 || s->tags_catalog.empty()) {
            s->filter.tag_filter = std::nullopt;
        } else {
            auto idx = std::min(s->active_tag_tab - 1,
                static_cast<int>(s->tags_catalog.size()) - 1);
            s->filter.tag_filter = s->tags_catalog[idx];
        }
        // Clamp selection
        s->selected = 0;
    };

    auto renderer = Renderer([s] {
        auto toolbar = RenderToolbar(s->toolbar,
            s->assignees_catalog, s->tags_catalog, s->active_tag_tab);

        auto mode_switch = hbox({
            text(" Mode: "),
            text(s->mode == ViewMode::Table ? "● Table" : "○ Table") |
                (s->mode == ViewMode::Table ? (bold | color(Color::Cyan)) : dim),
            text("   "),
            text(s->mode == ViewMode::Kanban ? "● Kanban" : "○ Kanban") |
                (s->mode == ViewMode::Kanban ? (bold | color(Color::Cyan)) : dim),
            filler(),
            text(" [Tab] toggle  ") | dim,
        });

        Elements body;
        if (s->mode == ViewMode::Table) {
            auto visible = build_visible(*s);
            auto vw = compute_virtual_window(
                static_cast<int>(visible.size()), s->selected,
                s->estimated_height);
            s->selected = std::min(s->selected,
                std::max(0, static_cast<int>(visible.size()) - 1));

            body.push_back(RenderTableHeader(s->sort));
            body.push_back(separator());

            if (visible.empty()) {
                body.push_back(text(" No tasks match the current filters.")
                               | dim | center | yflex);
            } else {
                int start = vw.visible_start;
                int end   = vw.visible_count
                    ? start + vw.visible_count
                    : static_cast<int>(visible.size());
                Elements rows;
                rows.reserve(static_cast<std::size_t>(end - start));
                for (int i = start; i < end; ++i) {
                    rows.push_back(RenderTableRow(
                        *visible[i], i == s->selected));
                }
                if (vw.visible_count > 0) {
                    if (start > 0) {
                        rows.insert(rows.begin(),
                            text(std::format("  … {} above …", start))
                            | dim | center);
                    }
                    int below = static_cast<int>(visible.size()) - end;
                    if (below > 0) {
                        rows.push_back(
                            text(std::format("  … {} below …", below))
                            | dim | center);
                    }
                }
                body.push_back(vbox(std::move(rows)) | vscroll_indicator
                               | yframe | flex);
            }
        } else {
            // Kanban
            auto visible = build_visible(*s);
            Elements cols;
            for (std::size_t i = 0; i < 4; ++i) {
                auto col_rows = build_kanban_col(
                    visible, kKanbanColumns[i].status);
                // clamp per-col selection
                if (s->kb_column_row[i] >= static_cast<int>(col_rows.size())) {
                    s->kb_column_row[i] = std::max(0,
                        static_cast<int>(col_rows.size()) - 1);
                }
                cols.push_back(RenderKanbanColumn(
                    kKanbanColumns[i], col_rows,
                    s->kb_column, static_cast<int>(i),
                    s->kb_column_row[i]) | flex);
            }
            body.push_back(flexbox(std::move(cols),
                FlexboxConfig{
                    .direction = FlexboxConfig::Direction::Row,
                    .wrap = FlexboxConfig::Wrap::NoWrap,
                }) | flex);
        }

        auto summary = RenderSummaryFooter(compute_summary(s->tasks));

        // Key hints
        auto hints = hbox({
            text(" j/k") | color(Color::Cyan), text(" move "),
            text("[Space]") | color(Color::Cyan), text(" done "),
            text("[Enter]") | color(Color::Cyan), text(" open "),
            text("[a]") | color(Color::Cyan), text(" new "),
            text("[d]") | color(Color::Cyan), text(" del "),
            text("[/]") | color(Color::Cyan), text(" search "),
            text("[1-4]") | color(Color::Cyan), text(" kanban col "),
            text("[Esc]") | color(Color::Cyan), text(" close"),
            s->pending_move
                ? text("  ⚠ press 1-4 to move to column")
                    | color(Color::Yellow)
                : text(""),
        }) | dim;

        return vbox({
            toolbar,
            separator(),
            mode_switch,
            separator(),
            vbox(std::move(body)) | flex,
            separator(),
            summary,
            separatorEmpty(),
            hints,
        }) | borderRounded;
    });

    return renderer | CatchEvent([s, sync_filter_from_toolbar](Event event) {
        // Global shortcuts
        if (event == Event::Tab) {
            s->mode = s->mode == ViewMode::Table
                ? ViewMode::Kanban : ViewMode::Table;
            return true;
        }
        if (event == Event::Character('/')) {
            // Enter search mode — consumer will usually re-route to an
            // Input component.  For now we treat keypresses as append.
            // TODO(UI12): replace with a real Input focus ring.
            return true;
        }
        // Handle search text via printable characters when '/' was last
        // pressed.  Implementation: we simply record any single char event
        // into the search box if search mode is "active".  Since FTXUI
        // does not expose focus per-view, we adopt a convention: any
        // typed printable character when the selected task is a filter
        // textarea — here we simulate by using an event.is_character check
        // only if user typed '/'.  A full input is out of scope for the
        // FTXUI port; we provide a best-effort append.
        //
        // Search text toggling: Backspace deletes last char.
        if (event.is_character() && event.character().size() == 1) {
            char c = event.character()[0];
            if (c == 0x7f /* DEL */ || c == '\b') {
                if (!s->toolbar.search_text.empty()) {
                    s->toolbar.search_text.pop_back();
                    sync_filter_from_toolbar();
                }
                return true;
            }
            // Append letter/digit/punct to search unless the key is bound
            // to one of our command keys.
            constexpr std::string_view kCmdKeys = "jadfkmh/ \t";
            if (!std::isspace(static_cast<unsigned char>(c))
                && kCmdKeys.find(c) == std::string_view::npos) {
                s->toolbar.search_text.push_back(c);
                sync_filter_from_toolbar();
                return true;
            }
        }

        if (event == Event::Escape) {
            if (s->pending_move) {
                s->pending_move = false;
                return true;
            }
            if (!s->toolbar.search_text.empty()) {
                s->toolbar.search_text.clear();
                sync_filter_from_toolbar();
                return true;
            }
            return false;  // propagate up (dialog close)
        }

        // Cycle status dropdown (f)
        if (event == Event::Character('f')) {
            s->toolbar.status_selected = (s->toolbar.status_selected + 1)
                % static_cast<int>(kStatusOptions.size());
            sync_filter_from_toolbar();
            return true;
        }

        // Cycle assignee dropdown (u)
        if (event == Event::Character('u')) {
            int n = static_cast<int>(s->assignees_catalog.size()) + 1;
            s->toolbar.assignee_selected =
                (s->toolbar.assignee_selected + 1) % n;
            sync_filter_from_toolbar();
            return true;
        }

        // Cycle tag tabs (t)
        if (event == Event::Character('t')) {
            int n = std::min(9,
                static_cast<int>(s->tags_catalog.size()) + 1);
            s->active_tag_tab = (s->active_tag_tab + 1) % n;
            sync_filter_from_toolbar();
            return true;
        }

        // Priority filter toggles (p cycles through enabling the next
        // priority filter, Shift+P clears).
        if (event == Event::Character('p')) {
            static constexpr Priority cycle[] = {
                Priority::Urgent, Priority::High, Priority::Medium,
                Priority::Low, Priority::None,
            };
            static int cycle_idx = 0;
            auto next = cycle[cycle_idx++ % 5];
            if (s->toolbar.priority_toggles.contains(next))
                s->toolbar.priority_toggles.erase(next);
            else s->toolbar.priority_toggles.insert(next);
            sync_filter_from_toolbar();
            return true;
        }
        if (event == Event::Character('P')) {
            s->toolbar.priority_toggles.clear();
            sync_filter_from_toolbar();
            return true;
        }

        // Click column headers to sort (mouse).  Keyboard shortcut: 's'
        // cycles the primary sort column.
        if (event == Event::Character('s')) {
            auto& primary = s->sort.front();
            // Cycle column order: Status→Priority→DueDate→Title→Status…
            switch (primary.col) {
                case Column::Status:   primary.col = Column::Priority;   break;
                case Column::Priority: primary.col = Column::DueDate;    break;
                case Column::DueDate:  primary.col = Column::Title;      break;
                case Column::Title:    primary.col = Column::Progress;   break;
                default:               primary.col = Column::Status;     break;
            }
            return true;
        }
        if (event == Event::Character('S')) {
            // Flip sort direction
            auto& primary = s->sort.front();
            primary.dir = primary.dir == SortDir::Asc
                ? SortDir::Desc : SortDir::Asc;
            return true;
        }

        // Pending move: 1-4 move task
        if (s->pending_move) {
            char c = event.is_character() ? event.character()[0] : 0;
            if (c >= '1' && c <= '4') {
                int col = c - '1';
                Status new_status = kKanbanColumns[col].status;
                if (s->cb.on_move_status) {
                    s->cb.on_move_status(s->pending_move_id, new_status);
                    // Mirror locally so UI updates before re-render.
                    for (auto& t : s->tasks) {
                        if (t.id == s->pending_move_id) {
                            t.status = new_status;
                            t.is_done = (new_status == Status::Done);
                        }
                    }
                }
                s->pending_move = false;
                return true;
            }
        }

        // --- Mode-specific navigation ---
        if (s->mode == ViewMode::Table) {
            auto visible = build_visible(*s);
            int n = static_cast<int>(visible.size());

            if (event == Event::ArrowUp || event == Event::Character('k')) {
                s->selected = std::max(0, s->selected - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                s->selected = std::min(std::max(0, n - 1), s->selected + 1);
                return true;
            }
            if (event == Event::Character('g')) {
                s->selected = 0;
                return true;
            }
            if (event == Event::Character('G')) {
                s->selected = std::max(0, n - 1);
                return true;
            }
            if (event == Event::Character(' ') && n > 0) {
                auto id = visible[s->selected]->id;
                // Mirror locally
                for (auto& t : s->tasks) {
                    if (t.id == id) { t.is_done = !t.is_done; break; }
                }
                if (s->cb.on_done_toggle) {
                    s->cb.on_done_toggle(id, visible[s->selected]->is_done);
                }
                return true;
            }
            if (event == Event::Return && n > 0) {
                if (s->cb.on_open_details) {
                    s->cb.on_open_details(visible[s->selected]->id);
                }
                return true;
            }
            if (event == Event::Character('a')) {
                if (s->cb.on_new_task) s->cb.on_new_task();
                return true;
            }
            if (event == Event::Character('d') && n > 0) {
                if (s->cb.on_delete) s->cb.on_delete(visible[s->selected]->id);
                return true;
            }
            if (event == Event::Character('m') && n > 0) {
                s->pending_move = true;
                s->pending_move_id = visible[s->selected]->id;
                return true;
            }
            // Shortcuts 1-4 jump to kanban column by setting mode + col
            char c = event.is_character() ? event.character()[0] : 0;
            if (c >= '1' && c <= '4') {
                s->mode = ViewMode::Kanban;
                s->kb_column = c - '1';
                s->kb_row_in_col = 0;
                return true;
            }
        } else {
            // --- Kanban mode ---
            if (event == Event::ArrowLeft || event == Event::Character('h')) {
                s->kb_column = std::max(0, s->kb_column - 1);
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                s->kb_column = std::min(3, s->kb_column + 1);
                return true;
            }
            // 1-4 direct column jump
            char c = event.is_character() ? event.character()[0] : 0;
            if (c >= '1' && c <= '4') {
                s->kb_column = c - '1';
                return true;
            }

            auto visible = build_visible(*s);
            auto col_rows = build_kanban_col(
                visible, kKanbanColumns[s->kb_column].status);
            int n = static_cast<int>(col_rows.size());

            if (event == Event::ArrowUp || event == Event::Character('k')) {
                s->kb_column_row[s->kb_column] = std::max(0,
                    s->kb_column_row[s->kb_column] - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                s->kb_column_row[s->kb_column] = std::min(
                    std::max(0, n - 1),
                    s->kb_column_row[s->kb_column] + 1);
                return true;
            }
            if (event == Event::Return && n > 0) {
                if (s->cb.on_open_details) {
                    s->cb.on_open_details(
                        col_rows[s->kb_column_row[s->kb_column]]->id);
                }
                return true;
            }
            if (event == Event::Character(' ') && n > 0) {
                auto id = col_rows[s->kb_column_row[s->kb_column]]->id;
                for (auto& t : s->tasks) {
                    if (t.id == id) { t.is_done = !t.is_done; break; }
                }
                if (s->cb.on_done_toggle) {
                    s->cb.on_done_toggle(id,
                        col_rows[s->kb_column_row[s->kb_column]]->is_done);
                }
                return true;
            }
            if (event == Event::Character('a')) {
                if (s->cb.on_new_task) s->cb.on_new_task();
                return true;
            }
            if (event == Event::Character('m') && n > 0) {
                s->pending_move = true;
                s->pending_move_id =
                    col_rows[s->kb_column_row[s->kb_column]]->id;
                return true;
            }
            if (event == Event::Character('d') && n > 0) {
                if (s->cb.on_delete) {
                    s->cb.on_delete(
                        col_rows[s->kb_column_row[s->kb_column]]->id);
                }
                return true;
            }
        }

        return false;
    });
}

} // namespace cc::ui::tasks::list_view
