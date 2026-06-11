/// @file task_details_dialog.cppm
/// @brief Fullscreen task detail modal — editable header, metadata row
///   (tags/assignee/due/links), description, sub-task checklist,
///   activity+comments timeline, Save/Delete/Close footer.
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

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/input.hpp>
#include <ftxui/component/button.hpp>

export module cc.ui.tasks.task_details_dialog;

import cc.ui.tasks.task_components;

export namespace cc::ui::tasks::detail_dialog {
using namespace ftxui;
namespace comp = cc::ui::tasks::components;

using Priority  = comp::Priority;
using Status    = comp::Status;
using Tag       = comp::Tag;
using DueDate   = comp::DueDate;
using SubTask   = comp::SubTask;

// ============================================================
// Activity / Comments timeline
// ============================================================

enum class ActivityKind : std::uint8_t {
    StatusChange,   // Task moved between columns
    Comment,        // User / agent comment
    AssigneeChange, // Assignee added/removed
    PriorityChange, // Priority updated
    DueDateChange,  // Due date set/changed
    TagChange,      // Tags modified
    LinkAdded,      // Reference URL added
    SubTaskChange,  // Sub-task added/checked
};

struct ActivityEntry {
    ActivityKind kind;
    std::string author;
    std::chrono::system_clock::time_point timestamp;
    std::string body;  // human-readable diff / comment text
};

/// Format timestamp as relative ("5m ago", "2h ago", "yesterday").
[[nodiscard]] inline std::string format_relative(
    std::chrono::system_clock::time_point ts) {

    using namespace std::chrono;
    auto diff = system_clock::now() - ts;
    auto secs = duration_cast<seconds>(diff).count();
    if (secs < 60)       return std::format("{}s ago", secs);
    auto mins = secs / 60;
    if (mins < 60)       return std::format("{}m ago", mins);
    auto hrs  = mins / 60;
    if (hrs < 24)        return std::format("{}h ago", hrs);
    auto days = hrs / 24;
    if (days < 7)        return std::format("{}d ago", days);
    auto t = system_clock::to_time_t(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    return std::format("{:04d}-{:02d}-{:02d}",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

[[nodiscard]] inline Element RenderActivity(const ActivityEntry& a) {
    auto icon = [&]() -> std::pair<std::string, Color> {
        switch (a.kind) {
            case ActivityKind::StatusChange:   return {"⇄", Color::Blue};
            case ActivityKind::Comment:        return {"💬", Color::GrayLight};
            case ActivityKind::AssigneeChange: return {"👤", Color::Magenta};
            case ActivityKind::PriorityChange: return {"⚑",  Color::Yellow};
            case ActivityKind::DueDateChange:  return {"📅", Color::Cyan};
            case ActivityKind::TagChange:      return {"🏷",  Color::Green};
            case ActivityKind::LinkAdded:      return {"🔗", Color::CyanLight};
            case ActivityKind::SubTaskChange:  return {"☑",  Color::Orange1};
        }
        return {"•", Color::White};
    }();
    return vbox({
        hbox({
            text(" " + icon.first + " ") | color(icon.second),
            comp::AssigneeAvatar(a.author) | size(WIDTH, EQUAL, 4),
            text(" " + a.author + " ") | bold,
            text(format_relative(a.timestamp)) | dim,
        }),
        hbox({
            text("   "),
            paragraph(a.body) | size(WIDTH, LESS_THAN, 80),
        }) | indent(1),
    });
}

// ============================================================
// Full data model (what the dialog works on)
// ============================================================

struct TaskDetails {
    std::string id;
    std::string title;
    Status status = Status::Todo;
    Priority priority = Priority::Medium;
    std::vector<Tag> tags;
    std::vector<std::string> assignees;
    std::optional<DueDate> due_date;
    std::vector<std::string> links;  // issue/PR/commit URLs
    std::string description;
    std::vector<SubTask> subtasks;
    std::vector<ActivityEntry> activity;

    // For the "priority cycle click" feature we track last-clicked prio
    Priority current_priority = Priority::Medium;
};

// ============================================================
// Callbacks
// ============================================================

struct DialogCallbacks {
    /// Called when user clicks "Save" (after any edit).
    std::function<void(const TaskDetails& updated)> on_save;
    /// Called when user clicks "Delete".  The caller should typically
    /// open a TrustDialog (medium severity) and only proceed if
    /// confirmed.  Wire cc::ui::trust_dialog here once the trust
    /// modal infrastructure supports inline embedding.
    std::function<void(const std::string& task_id)> on_delete_request;
    /// Called on Esc or "Close" button.
    std::function<void()> on_close;
    /// Called when user clicks "+ tag" and submits a new tag label.
    std::function<void(const std::string& tag_text)> on_add_tag;
    /// Called when user removes a tag chip.
    std::function<void(const std::string& tag_text)> on_remove_tag;
    /// Called when a new sub-task is added.
    std::function<void(const std::string& text)> on_add_subtask;
    /// Called when a sub-task's done-flag is toggled.
    std::function<void(int subtask_index, bool done)> on_subtask_toggle;
    /// Called when user posts a comment.
    std::function<void(const std::string& text)> on_post_comment;
};

// ============================================================
// Internal state machine for interactive widgets
// ============================================================

/// Which section currently has focus (keyboard Tab cycles through them).
enum class FocusSection : std::uint8_t {
    Title,
    StatusDropdown,
    PriorityChip,
    TagsRow,
    Assignee,
    DueDate,
    Links,
    Description,
    Subtasks,
    Activity,
    CommentInput,
    FooterButtons,
};

struct DialogState {
    TaskDetails data;
    DialogCallbacks cb;

    // Focus
    FocusSection focus = FocusSection::Title;

    // Temporary input buffers (kept in sync with data for rendering).
    std::string comment_buffer;
    std::string new_tag_buffer;
    std::string new_subtask_buffer;
    std::string new_link_buffer;
    bool show_add_tag_modal = false;
    bool show_add_link_modal = false;
    bool show_add_assignee_modal = false;

    // Footer button selection (0 Save / 1 Delete / 2 Close)
    int footer_button = 0;

    // Status dropdown (expanded?)
    bool status_expanded = false;
    int status_hover = 0;

    // Assignee picker list (0 = Unassigned, 1..N = catalog)
    int assignee_hover = 0;
    std::vector<std::string> assignee_catalog;

    [[nodiscard]] FocusSection next_focus() const {
        switch (focus) {
            case FocusSection::Title:         return FocusSection::StatusDropdown;
            case FocusSection::StatusDropdown:return FocusSection::PriorityChip;
            case FocusSection::PriorityChip:  return FocusSection::TagsRow;
            case FocusSection::TagsRow:       return FocusSection::Assignee;
            case FocusSection::Assignee:      return FocusSection::DueDate;
            case FocusSection::DueDate:       return FocusSection::Links;
            case FocusSection::Links:         return FocusSection::Description;
            case FocusSection::Description:   return FocusSection::Subtasks;
            case FocusSection::Subtasks:      return FocusSection::Activity;
            case FocusSection::Activity:      return FocusSection::CommentInput;
            case FocusSection::CommentInput:  return FocusSection::FooterButtons;
            case FocusSection::FooterButtons: return FocusSection::Title;
        }
        return FocusSection::Title;
    }
    [[nodiscard]] FocusSection prev_focus() const {
        switch (focus) {
            case FocusSection::Title:         return FocusSection::FooterButtons;
            case FocusSection::StatusDropdown:return FocusSection::Title;
            case FocusSection::PriorityChip:  return FocusSection::StatusDropdown;
            case FocusSection::TagsRow:       return FocusSection::PriorityChip;
            case FocusSection::Assignee:      return FocusSection::TagsRow;
            case FocusSection::DueDate:       return FocusSection::Assignee;
            case FocusSection::Links:         return FocusSection::DueDate;
            case FocusSection::Description:   return FocusSection::Links;
            case FocusSection::Subtasks:      return FocusSection::Description;
            case FocusSection::Activity:      return FocusSection::Subtasks;
            case FocusSection::CommentInput:  return FocusSection::Activity;
            case FocusSection::FooterButtons: return FocusSection::CommentInput;
        }
        return FocusSection::Title;
    }
};

// ============================================================
// Rendering sub-sections
// ============================================================

[[nodiscard]] inline bool has_focus(FocusSection f, FocusSection current) {
    return f == current;
}

[[nodiscard]] inline Element focus_border_wrap(Element el, bool focused,
                                               Color accent = Color::Cyan) {
    if (focused) return el | borderStyled(BorderStyle::ROUNDED, accent);
    return el | borderEmpty;
}

/// Render section header (small label with an emoji).
[[nodiscard]] inline Element SectionHeader(std::string label) {
    return text(" " + label + " ") | bold | color(Color::Cyan)
        | bgcolor(Color::RGB(20, 25, 40));
}

/// Editable title + status pill + priority cycle chip.
[[nodiscard]] inline Element RenderHeader(const DialogState& s) {
    auto title_text = s.data.title.empty() ? "(untitled task)" : s.data.title;
    auto title = text(" " + title_text + " ")
        | (s.data.title.empty() ? (dim | strikethrough) : bold)
        | xflex
        | size(HEIGHT, EQUAL, 1);
    if (has_focus(FocusSection::Title, s.focus)) {
        title = title | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    }

    // Status pill (dropdown: shows arrow if expanded)
    auto status_el = hbox({
        comp::StatusPill(s.data.status),
        text(s.status_expanded ? " ▾" : " ▸") | dim,
    });
    if (has_focus(FocusSection::StatusDropdown, s.focus)) {
        status_el = status_el | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    }

    // Priority chip cycle
    auto prio = hbox({
        comp::PriorityBadge(s.data.priority, /*compact=*/true),
        text(" ⟳") | dim,
    });
    if (has_focus(FocusSection::PriorityChip, s.focus)) {
        prio = prio | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    }

    auto row = hbox({
        title | flex,
        text("   "),
        status_el,
        text("  "),
        prio,
    });

    Elements out;
    out.push_back(row);
    // Expanded status dropdown
    if (s.status_expanded) {
        Elements items;
        static constexpr std::pair<std::string_view, Status> opts[] = {
            {"Todo",       Status::Todo},
            {"In Progress",Status::InProgress},
            {"Review",     Status::Review},
            {"Done",       Status::Done},
            {"Cancelled",  Status::Cancelled},
        };
        for (std::size_t i = 0; i < 5; ++i) {
            auto& [label, val] = opts[i];
            bool sel = (val == s.data.status);
            bool hov = (static_cast<int>(i) == s.status_hover);
            items.push_back(hbox({
                text(sel ? " ● " : "   "),
                comp::StatusPill(val),
                text("  " + std::string{label}) | (hov ? bold : nothing),
            }) | (hov ? bgcolor(Color::RGB(25, 30, 45)) : nothing));
        }
        out.push_back(vbox(std::move(items)) | borderEmpty
            | bgcolor(Color::RGB(10, 12, 20)));
    }
    return vbox(std::move(out));
}

/// Meta row: tags, assignee, due date, links.
[[nodiscard]] inline Element RenderMetaRow(const DialogState& s) {
    // Tags
    Elements tag_els;
    for (auto& t : s.data.tags) {
        tag_els.push_back(comp::TagChip(t, true));
        tag_els.push_back(text(" "));
    }
    tag_els.push_back(text(" + tag")
        | color(Color::Cyan) | dim | borderEmpty);
    auto tags_box = vbox({
        SectionHeader("🏷  Tags"),
        flexbox(std::move(tag_els), FlexboxConfig{
            .direction = FlexboxConfig::Direction::Row,
            .wrap = FlexboxConfig::Wrap::Wrap,
        }),
    });
    if (has_focus(FocusSection::TagsRow, s.focus)) {
        tags_box = tags_box | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    } else {
        tags_box = tags_box | borderEmpty;
    }

    // Assignees
    Elements as_els;
    if (s.data.assignees.empty()) {
        as_els.push_back(comp::AssigneeAvatarUnassigned());
    } else {
        as_els.push_back(
            comp::AssigneeAvatarStack(s.data.assignees, 3));
    }
    auto ass_box = vbox({
        SectionHeader("👤 Assignee"),
        hbox(std::move(as_els)) | xflex,
    });
    if (has_focus(FocusSection::Assignee, s.focus)) {
        ass_box = ass_box | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    } else {
        ass_box = ass_box | borderEmpty;
    }

    // Due date
    auto due_inner = s.data.due_date
        ? comp::DueDateBadge(*s.data.due_date)
        : text(" 📅 Not set (click to set)") | dim;
    auto due_box = vbox({
        SectionHeader("📅 Due Date"),
        due_inner | xflex,
        // Natural language date parsing ("Tomorrow", "Next Monday") requires
        // a date-NLP library; for now only ISO format is accepted.
        text("        Format: YYYY-MM-DD") | dim,
    });
    if (has_focus(FocusSection::DueDate, s.focus)) {
        due_box = due_box | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    } else {
        due_box = due_box | borderEmpty;
    }

    // Links
    Elements link_els;
    int shown = 0;
    for (auto& l : s.data.links) {
        if (shown++ >= 3) break;
        link_els.push_back(hbox({
            text(" 🔗 ") | color(Color::CyanLight) | dim,
            text(l) | underlined | color(Color::CyanLight) | xflex,
        }));
    }
    link_els.push_back(text(" 🔗 + add link") | dim | color(Color::Cyan));
    auto link_box = vbox({
        SectionHeader("🔗 Links"),
        vbox(std::move(link_els)),
    });
    if (has_focus(FocusSection::Links, s.focus)) {
        link_box = link_box | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    } else {
        link_box = link_box | borderEmpty;
    }

    return gridbox({
        {tags_box  | xflex, ass_box | xflex},
        {due_box   | xflex, link_box | xflex},
    }) | size(WIDTH, EQUAL, 100);
}

/// Description area (simplified: large input style box).
[[nodiscard]] inline Element RenderDescription(const DialogState& s) {
    auto desc = s.data.description.empty()
        ? text("  (No description yet)") | dim
        : paragraph("  " + s.data.description) | color(Color::GrayLight);

    auto box = vbox({
        SectionHeader("📝 Description"),
        desc | xflex | size(HEIGHT, GREATER_THAN, 4),
    });
    if (has_focus(FocusSection::Description, s.focus)) {
        return box | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    }
    return box | borderEmpty;
}

/// Sub-task checklist.
[[nodiscard]] inline Element RenderSubTasks(const DialogState& s) {
    Elements rows;
    int done = 0, total = static_cast<int>(s.data.subtasks.size());
    for (int i = 0; i < total; ++i) {
        auto& st = s.data.subtasks[i];
        if (st.done) ++done;
        rows.push_back(comp::SubTaskRow(st, has_focus(FocusSection::Subtasks, s.focus)));
    }
    if (rows.empty()) {
        rows.push_back(text("   (No sub-tasks yet)") | dim);
    }
    rows.push_back(text("   + Add sub-task") | color(Color::Cyan) | dim);

    double pct = total > 0 ? done / double(total) : 0.0;

    auto box = vbox({
        hbox({
            SectionHeader("☑  Sub-tasks"),
            filler(),
            comp::ProgressBar(pct, 16),
        }),
        vbox(std::move(rows)),
    });
    if (has_focus(FocusSection::Subtasks, s.focus)) {
        return box | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    }
    return box | borderEmpty;
}

/// Activity timeline + comment input.
[[nodiscard]] inline Element RenderActivitySection(const DialogState& s) {
    Elements items;
    for (auto& a : s.data.activity) {
        items.push_back(RenderActivity(a));
        items.push_back(separatorEmpty());
    }
    if (items.empty()) {
        items.push_back(text("   (No activity yet)") | dim);
    }

    auto timeline = vbox(std::move(items)) | yframe | vscroll_indicator
        | size(HEIGHT, GREATER_THAN, 6);

    auto comment = hbox({
        text(" 💬 "),
        text(s.comment_buffer.empty() ? "Add a comment..." : s.comment_buffer)
            | (s.comment_buffer.empty() ? dim : color(Color::White))
            | xflex,
        text(" [Send] ") | bold | color(Color::Cyan),
    });
    if (has_focus(FocusSection::CommentInput, s.focus)) {
        comment = comment | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    } else {
        comment = comment | borderEmpty;
    }

    auto act_box = vbox({
        SectionHeader("🗒  Activity / Comments"),
        timeline | xflex,
        separator(),
        comment,
    });
    if (has_focus(FocusSection::Activity, s.focus)) {
        return act_box | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    }
    return act_box | borderEmpty;
}

/// Footer: Save / Delete / Close.
[[nodiscard]] inline Element RenderFooter(const DialogState& s) {
    auto make_btn = [&](const std::string& label, Color col, int idx) {
        bool sel = has_focus(FocusSection::FooterButtons, s.focus)
                   && s.footer_button == idx;
        auto el = text(" " + label + " ") | bold | color(col)
            | (sel ? (inverted | bgcolor(col)) : borderEmpty);
        return el;
    };
    return hbox({
        make_btn("Save",   Color::Green,  0),
        text("  "),
        make_btn("Delete", Color::Red,    1),
        text("  "),
        make_btn("Close",  Color::GrayLight, 2),
        filler(),
        text(" [Tab] cycle sections  [Esc] close") | dim,
    });
}

// ============================================================
// Main render function
// ============================================================

[[nodiscard]] inline Element RenderDetailsDialog(const DialogState& s) {
    // Header strip
    auto header = vbox({
        hbox({
            text(" 📝 Task Details ") | bold | color(Color::Cyan)
                | bgcolor(Color::RGB(15, 20, 40)),
            text("  #") | dim,
            text(s.data.id) | dim,
            filler(),
            text(" Esc to close ") | dim,
        }),
        separator(),
        RenderHeader(s),
    });

    auto meta = RenderMetaRow(s);
    auto desc = RenderDescription(s);
    auto subs = RenderSubTasks(s);
    auto act  = RenderActivitySection(s);
    auto foot = RenderFooter(s);

    // Any modal overlays (add-tag / add-link tiny modals) go on top.
    Element overlay = text("");
    if (s.show_add_tag_modal) {
        overlay = window(
            text(" Add tag ") | bold | color(Color::Cyan),
            vbox({
                text(s.new_tag_buffer.empty()
                        ? "Type a tag name (Enter to add)..."
                        : s.new_tag_buffer) | xflex,
                separatorEmpty(),
                text(" [Enter] confirm  [Esc] cancel") | dim,
            })
        ) | size(WIDTH, EQUAL, 40) | center;
    } else if (s.show_add_link_modal) {
        overlay = window(
            text(" Add link ") | bold | color(Color::Cyan),
            vbox({
                text(s.new_link_buffer.empty()
                        ? "Paste URL (Enter to add)..."
                        : s.new_link_buffer) | xflex,
                separatorEmpty(),
                text(" [Enter] confirm  [Esc] cancel") | dim,
            })
        ) | size(WIDTH, EQUAL, 50) | center;
    } else if (s.show_add_assignee_modal) {
        Elements names;
        names.push_back(hbox({
            text(" ○  Unassigned")
                | (s.assignee_hover == 0 ? (bold | bgcolor(Color::RGB(25,30,45)))
                                         : nothing),
        }));
        int n = static_cast<int>(s.assignee_catalog.size());
        for (int i = 0; i < n; ++i) {
            bool hov = (i + 1 == s.assignee_hover);
            names.push_back(hbox({
                text(" ○  ") + comp::AssigneeAvatar(s.assignee_catalog[i])
                    + text(" " + s.assignee_catalog[i])
                    | (hov ? (bold | bgcolor(Color::RGB(25,30,45))) : nothing),
            }));
        }
        overlay = window(
            text(" Pick assignee ") | bold | color(Color::Magenta),
            vbox({
                vbox(std::move(names)) | yframe | size(HEIGHT, LESS_THAN, 8),
                separatorEmpty(),
                text(" [↑/↓] move  [Enter] pick  [Esc] cancel") | dim,
            })
        ) | size(WIDTH, EQUAL, 40) | center;
    }

    return vbox({
        header,
        separator(),
        meta,
        separatorEmpty(),
        desc,
        separatorEmpty(),
        subs,
        separatorEmpty(),
        act | flex,
        separator(),
        foot,
        overlay,  // overlays are rendered on top as a vbox child; FTXUI
                  // rendering does not give true Z-index so we rely on the
                  // border/brightness for emphasis.
    }) | borderRounded | color(Color::Cyan);
}

// ============================================================
// Options + factory
// ============================================================

struct TaskDetailsDialogOptions {
    TaskDetails initial;
    DialogCallbacks callbacks;
    std::vector<std::string> assignee_catalog;
};

[[nodiscard]] inline Component TaskDetailsDialog(TaskDetailsDialogOptions opts) {
    auto s = std::make_shared<DialogState>();
    s->data = std::move(opts.initial);
    s->cb   = std::move(opts.callbacks);
    s->assignee_catalog = std::move(opts.assignee_catalog);

    return Renderer([s] { return RenderDetailsDialog(*s); })
        | CatchEvent([s](Event event) {
            // Modal overlays first.
            if (s->show_add_tag_modal) {
                if (event == Event::Escape) {
                    s->show_add_tag_modal = false;
                    s->new_tag_buffer.clear();
                    return true;
                }
                if (event == Event::Return) {
                    if (!s->new_tag_buffer.empty()) {
                        s->data.tags.push_back(Tag{s->new_tag_buffer, {}});
                        if (s->cb.on_add_tag)
                            s->cb.on_add_tag(s->new_tag_buffer);
                    }
                    s->show_add_tag_modal = false;
                    s->new_tag_buffer.clear();
                    return true;
                }
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1 && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!s->new_tag_buffer.empty()) s->new_tag_buffer.pop_back();
                    } else if (c.size() == 1 && std::isprint(static_cast<unsigned char>(c[0]))) {
                        s->new_tag_buffer.push_back(c[0]);
                    }
                    return true;
                }
                return true;
            }
            if (s->show_add_link_modal) {
                if (event == Event::Escape) {
                    s->show_add_link_modal = false;
                    s->new_link_buffer.clear();
                    return true;
                }
                if (event == Event::Return) {
                    if (!s->new_link_buffer.empty()) {
                        s->data.links.push_back(s->new_link_buffer);
                    }
                    s->show_add_link_modal = false;
                    s->new_link_buffer.clear();
                    return true;
                }
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1 && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!s->new_link_buffer.empty()) s->new_link_buffer.pop_back();
                    } else if (c.size() == 1 && std::isprint(static_cast<unsigned char>(c[0]))) {
                        s->new_link_buffer.push_back(c[0]);
                    }
                    return true;
                }
                return true;
            }
            if (s->show_add_assignee_modal) {
                int n = static_cast<int>(s->assignee_catalog.size());
                if (event == Event::Escape) {
                    s->show_add_assignee_modal = false;
                    return true;
                }
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    s->assignee_hover = std::max(0, s->assignee_hover - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    s->assignee_hover = std::min(n, s->assignee_hover + 1);
                    return true;
                }
                if (event == Event::Return) {
                    if (s->assignee_hover == 0) {
                        s->data.assignees.clear();
                    } else {
                        auto name = s->assignee_catalog[s->assignee_hover - 1];
                        s->data.assignees.clear();
                        s->data.assignees.push_back(std::move(name));
                    }
                    s->show_add_assignee_modal = false;
                    return true;
                }
                return true;
            }

            if (s->status_expanded) {
                if (event == Event::Escape) { s->status_expanded = false; return true; }
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    s->status_hover = std::max(0, s->status_hover - 1); return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    s->status_hover = std::min(4, s->status_hover + 1); return true;
                }
                if (event == Event::Return) {
                    static constexpr Status map[] = {
                        Status::Todo, Status::InProgress, Status::Review,
                        Status::Done, Status::Cancelled,
                    };
                    s->data.status = map[s->status_hover];
                    s->status_expanded = false;
                    return true;
                }
                return true;
            }

            if (event == Event::Escape) {
                if (s->cb.on_close) s->cb.on_close();
                return true;
            }

            // Tab navigation between sections.
            if (event == Event::Tab) {
                s->focus = s->next_focus();
                return true;
            }
            if (event == Event::TabReverse) {
                s->focus = s->prev_focus();
                return true;
            }

            // --- Per-section input handling ---

            // Footer buttons
            if (s->focus == FocusSection::FooterButtons) {
                if (event == Event::ArrowLeft || event == Event::Character('h')) {
                    s->footer_button = std::max(0, s->footer_button - 1);
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Character('l')) {
                    s->footer_button = std::min(2, s->footer_button + 1);
                    return true;
                }
                if (event == Event::Return) {
                    switch (s->footer_button) {
                        case 0:
                            if (s->cb.on_save) s->cb.on_save(s->data);
                            return true;
                        case 1:
                            if (s->cb.on_delete_request)
                                s->cb.on_delete_request(s->data.id);
                            return true;
                        case 2:
                            if (s->cb.on_close) s->cb.on_close();
                            return true;
                    }
                    return true;
                }
            }

            // Title (editable: we route any printable key to buffer)
            if (s->focus == FocusSection::Title) {
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1 && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!s->data.title.empty()) s->data.title.pop_back();
                        return true;
                    }
                    if (c.size() == 1 && std::isprint(static_cast<unsigned char>(c[0]))) {
                        s->data.title.push_back(c[0]);
                        return true;
                    }
                }
            }

            // Status dropdown toggle
            if (s->focus == FocusSection::StatusDropdown) {
                if (event == Event::Return || event == Event::Character(' ')) {
                    s->status_expanded = true;
                    // Set hover to current status index
                    static constexpr Status map[] = {
                        Status::Todo, Status::InProgress, Status::Review,
                        Status::Done, Status::Cancelled,
                    };
                    for (int i = 0; i < 5; ++i) {
                        if (map[i] == s->data.status) { s->status_hover = i; break; }
                    }
                    return true;
                }
            }

            // Priority cycle: Space/Enter advances priority by one step.
            if (s->focus == FocusSection::PriorityChip) {
                if (event == Event::Return || event == Event::Character(' ')) {
                    static constexpr Priority cycle[] = {
                        Priority::None,   Priority::Low, Priority::Medium,
                        Priority::High,   Priority::Urgent,
                    };
                    auto cur = s->data.priority;
                    int idx = 0;
                    for (int i = 0; i < 5; ++i) if (cycle[i] == cur) { idx = i; break; }
                    s->data.priority = cycle[(idx + 1) % 5];
                    return true;
                }
            }

            // Tags row: Enter = open +tag modal, x removes selected tag.
            if (s->focus == FocusSection::TagsRow) {
                if (event == Event::Return) {
                    s->show_add_tag_modal = true;
                    return true;
                }
                if (event == Event::Character('x') && !s->data.tags.empty()) {
                    auto removed = s->data.tags.back();
                    s->data.tags.pop_back();
                    if (s->cb.on_remove_tag) s->cb.on_remove_tag(removed.text);
                    return true;
                }
            }

            // Assignee picker
            if (s->focus == FocusSection::Assignee) {
                if (event == Event::Return) {
                    s->show_add_assignee_modal = true;
                    s->assignee_hover = 0;
                    return true;
                }
            }

            // Due date: Enter opens freeform input (simplified: type text).
            if (s->focus == FocusSection::DueDate) {
                if (event.is_character()) {
                    // Route printable chars to a scratch buffer via Enter
                    // For simplicity, we treat Enter as parse & commit.
                }
                if (event == Event::Return) {
                    // A proper date input modal would provide calendar-style
                    // selection; for now ISO string entry via keyboard suffices.
                    return true;
                }
                // Fast-path: user can also type `d 2026-06-15` to set; here
                // we simply provide a keyboard entry for the ISO string.
                static std::string scratch;
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1 && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!scratch.empty()) scratch.pop_back();
                        return true;
                    }
                    if (c.size() == 1 && std::isprint(static_cast<unsigned char>(c[0]))) {
                        scratch.push_back(c[0]);
                        // Auto-parse once fully typed.
                        auto parsed = DueDate::parse(scratch);
                        if (parsed && scratch.size() == 10) {
                            s->data.due_date = parsed;
                            scratch.clear();
                        }
                        return true;
                    }
                }
            }

            // Links
            if (s->focus == FocusSection::Links) {
                if (event == Event::Return) {
                    s->show_add_link_modal = true;
                    return true;
                }
            }

            // Description (very simplified: printable char append)
            if (s->focus == FocusSection::Description) {
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1 && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!s->data.description.empty()) s->data.description.pop_back();
                        return true;
                    }
                    if (c.size() == 1) {
                        s->data.description.push_back(c[0]);
                        return true;
                    }
                }
            }

            // Sub-tasks
            if (s->focus == FocusSection::Subtasks) {
                static int sel_st = 0;
                int n = static_cast<int>(s->data.subtasks.size());
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    sel_st = std::max(0, sel_st - 1); return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    sel_st = std::max(0, std::min(n - 1, sel_st + 1)); return true;
                }
                if ((event == Event::Character(' ') || event == Event::Return)
                    && n > 0 && sel_st < n) {
                    s->data.subtasks[sel_st].done = !s->data.subtasks[sel_st].done;
                    if (s->cb.on_subtask_toggle)
                        s->cb.on_subtask_toggle(sel_st, s->data.subtasks[sel_st].done);
                    return true;
                }
                if (event == Event::Character('a')) {
                    s->data.subtasks.push_back(
                        SubTask{"", "(new sub-task — edit)", false});
                    if (s->cb.on_add_subtask)
                        s->cb.on_add_subtask("(new sub-task — edit)");
                    return true;
                }
            }

            // Comment input
            if (s->focus == FocusSection::CommentInput) {
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1 && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!s->comment_buffer.empty()) s->comment_buffer.pop_back();
                        return true;
                    }
                    if (c.size() == 1) {
                        s->comment_buffer.push_back(c[0]);
                        return true;
                    }
                }
                if (event == Event::Return && !s->comment_buffer.empty()) {
                    if (s->cb.on_post_comment) s->cb.on_post_comment(s->comment_buffer);
                    s->data.activity.push_back(ActivityEntry{
                        ActivityKind::Comment,
                        "user",
                        std::chrono::system_clock::now(),
                        s->comment_buffer,
                    });
                    s->comment_buffer.clear();
                    return true;
                }
            }

            // Global Ctrl+S triggers save.
            if (event == Event::Character('s') && std::holds_alternative<bool>(true)) {
                // FTXUI does not expose a modifier bit easily; we fall
                // back to the Save button being the primary action.
                // (noop path here so clang doesn't warn.)
            }
            if (event.is_character() && event.character() == "\x13") {  // Ctrl+S
                if (s->cb.on_save) s->cb.on_save(s->data);
                return true;
            }

            return false;
        });
}

} // namespace cc::ui::tasks::detail_dialog
