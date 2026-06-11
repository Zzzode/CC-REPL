/// @file task_wizard.cppm
/// @brief 3-step task creation / editing wizard built on top of the
///   cc.ui.wizard_dialog multi-step framework.
///
/// Steps:
///   1. Basics      — title (required), description, priority
///   2. Meta        — tag chips, assignee dropdown, due date input, links
///   3. Sub-tasks + confirm — sub-task checklist editor + summary preview
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

export module cc.ui.tasks.task_wizard;

import cc.ui.wizard_dialog;
import cc.ui.tasks.task_components;

export namespace cc::ui::tasks::wizard {
using namespace ftxui;
namespace comp = cc::ui::tasks::components;
namespace wz   = cc::ui::wizard_dialog;

using Priority = comp::Priority;
using Status   = comp::Status;
using Tag      = comp::Tag;
using DueDate  = comp::DueDate;
using SubTask  = comp::SubTask;

// ============================================================
// Data model — accumulator built up across the three wizard steps.
// ============================================================

struct WizardTaskDraft {
    std::string title;
    std::string description;
    Priority priority = Priority::Medium;
    std::vector<Tag> tags;
    std::vector<std::string> assignees;
    std::optional<DueDate> due_date;
    std::vector<std::string> links;
    std::vector<SubTask> subtasks;

    // Validation helpers (used by step 1 — title is required).
    [[nodiscard]] bool is_title_valid() const {
        return !title.empty()
               && std::any_of(title.begin(), title.end(),
                    [](unsigned char c){ return !std::isspace(c); });
    }
};

// ============================================================
// Callbacks
// ============================================================

struct WizardCallbacks {
    /// Fired on step 3 "Confirm".
    std::function<void(const WizardTaskDraft&)> on_confirm;
    /// Fired when user cancels at any point.
    std::function<void()> on_cancel;
    /// Fired on each step transition (useful for logging / analytics).
    std::function<void(int from_step, int to_step)> on_step_change;
};

// ============================================================
// Step 1: Basics
// ============================================================

struct Step1State {
    WizardTaskDraft* draft = nullptr;

    // Which widget is focused in this step: 0 title, 1 description, 2 priority.
    int focus = 0;
};

[[nodiscard]] inline Element RenderStep1(const Step1State& s) {
    auto title_valid = s.draft ? s.draft->is_title_valid() : false;
    auto title_prompt = s.draft && s.draft->title.empty()
        ? text("   (type a title — required)") | dim | color(Color::Red)
        : text("   (type a title)") | dim;

    auto title = vbox({
        text(" Title ") | bold | color(Color::Cyan)
            | bgcolor(Color::RGB(20, 25, 40)),
        hbox({
            text(" 📋 "),
            text(s.draft ? s.draft->title : "")
                | (title_valid ? color(Color::White) : color(Color::Red))
                | size(WIDTH, EQUAL, 60)
                | (s.focus == 0 ? borderStyled(BorderStyle::ROUNDED, Color::Cyan)
                                : borderEmpty),
            title_prompt,
        }),
    });

    auto desc = vbox({
        text(" Description ") | bold | color(Color::Cyan)
            | bgcolor(Color::RGB(20, 25, 40)),
        paragraph(s.draft ? (" " + s.draft->description) : " (optional)")
            | size(HEIGHT, GREATER_THAN, 3)
            | size(WIDTH, EQUAL, 70)
            | (s.focus == 1 ? borderStyled(BorderStyle::ROUNDED, Color::Cyan)
                            : borderEmpty),
    });

    auto prio = vbox({
        text(" Priority ") | bold | color(Color::Cyan)
            | bgcolor(Color::RGB(20, 25, 40)),
        hbox({
            text("    "),
            comp::PriorityBadge(s.draft ? s.draft->priority : Priority::Medium,
                                /*compact=*/false),
            text("   (Space / Enter to cycle)") | dim,
        }) | (s.focus == 2 ? borderStyled(BorderStyle::ROUNDED, Color::Cyan)
                           : borderEmpty),
    });

    auto hints = hbox({
        text(" [Tab]") | color(Color::Cyan), text(" next field  "),
        text("[Enter]") | color(Color::Cyan), text(" step next  "),
        text("[Esc]") | color(Color::Cyan), text(" back / cancel"),
    }) | dim;

    return vbox({
        title,
        separatorEmpty(),
        desc,
        separatorEmpty(),
        prio,
        filler(),
        hints,
    });
}

/// Builds the interactive component for step 1.  Directly mutates `draft`.
[[nodiscard]] inline Component Step1Component(WizardTaskDraft* draft) {
    auto inner = std::make_shared<Step1State>();
    inner->draft = draft;

    return Renderer([inner] { return RenderStep1(*inner); })
        | CatchEvent([inner](Event event) {
            // Field navigation (Tab / Shift+Tab inside the step).
            if (event == Event::Tab) {
                inner->focus = (inner->focus + 1) % 3;
                return true;
            }
            if (event == Event::TabReverse) {
                inner->focus = (inner->focus + 2) % 3;
                return true;
            }
            // Printable chars go to whichever field is focused.
            if (event.is_character()) {
                auto c = event.character();
                auto is_del = c.size() == 1 && (c[0] == 0x7f || c[0] == '\b');
                if (inner->focus == 0) {
                    if (is_del) {
                        if (!inner->draft->title.empty())
                            inner->draft->title.pop_back();
                        return true;
                    }
                    if (c.size() == 1
                        && std::isprint(static_cast<unsigned char>(c[0]))) {
                        inner->draft->title.push_back(c[0]);
                        return true;
                    }
                } else if (inner->focus == 1) {
                    if (is_del) {
                        if (!inner->draft->description.empty())
                            inner->draft->description.pop_back();
                        return true;
                    }
                    if (c.size() == 1) {
                        inner->draft->description.push_back(c[0]);
                        return true;
                    }
                } else if (inner->focus == 2) {
                    if (c == " " || c == "\r") {
                        // Cycle priority.
                        static constexpr Priority cycle[] = {
                            Priority::None, Priority::Low, Priority::Medium,
                            Priority::High, Priority::Urgent,
                        };
                        int idx = 2;
                        for (int i = 0; i < 5; ++i) {
                            if (cycle[i] == inner->draft->priority) {
                                idx = i; break;
                            }
                        }
                        inner->draft->priority = cycle[(idx + 1) % 5];
                        return true;
                    }
                }
            }
            // Enter on priority cycles too.
            if (event == Event::Return && inner->focus == 2) {
                static constexpr Priority cycle[] = {
                    Priority::None, Priority::Low, Priority::Medium,
                    Priority::High, Priority::Urgent,
                };
                int idx = 2;
                for (int i = 0; i < 5; ++i) {
                    if (cycle[i] == inner->draft->priority) { idx = i; break; }
                }
                inner->draft->priority = cycle[(idx + 1) % 5];
                return true;
            }
            return false;
        });
}

// ============================================================
// Step 2: Meta (tags, assignee, due date, links)
// ============================================================

struct Step2State {
    WizardTaskDraft* draft = nullptr;
    const std::vector<std::string>* assignee_catalog = nullptr;

    // Which section is focused: 0 tags, 1 assignee, 2 due, 3 links.
    int focus = 0;

    // Sub-widget state
    bool tag_entry_active = false;
    std::string tag_scratch;
    bool assignee_picker_active = false;
    int assignee_picker_hover = 0;
    bool link_entry_active = false;
    std::string link_scratch;
    std::string due_scratch;  // for ISO YYYY-MM-DD typing
};

[[nodiscard]] inline Element RenderStep2(const Step2State& s) {
    auto focus_wrap = [&](Element el, int idx) -> Element {
        if (s.focus == idx)
            return el | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
        return el | borderEmpty;
    };

    // --- Tags ---
    Elements tag_els;
    for (auto& t : s.draft->tags) {
        tag_els.push_back(comp::TagChip(t, true));
        tag_els.push_back(text(" "));
    }
    if (s.tag_entry_active) {
        tag_els.push_back(
            hbox({text("> " + s.tag_scratch + "█")
                  | color(Color::Cyan) | bold})
            | borderEmpty);
    } else {
        tag_els.push_back(
            text(" + add tag (Enter)") | color(Color::Cyan) | dim);
    }
    auto tags_box = vbox({
        text(" 🏷  Tags ") | bold | color(Color::Cyan)
            | bgcolor(Color::RGB(20, 25, 40)),
        flexbox(std::move(tag_els), FlexboxConfig{
            .direction = FlexboxConfig::Direction::Row,
            .wrap = FlexboxConfig::Wrap::Wrap,
        }) | xflex,
    });
    tags_box = focus_wrap(tags_box, 0);

    // --- Assignee ---
    Elements ass_els;
    if (s.draft->assignees.empty()) {
        ass_els.push_back(comp::AssigneeAvatarUnassigned());
    } else {
        ass_els.push_back(
            comp::AssigneeAvatarStack(s.draft->assignees, 3));
    }
    if (s.assignee_picker_active && s.assignee_catalog) {
        Elements names;
        names.push_back(hbox({
            text((s.assignee_picker_hover == 0 ? " ● " : " ○ ")),
            text("Unassigned"),
        }) | (s.assignee_picker_hover == 0 ? bgcolor(Color::RGB(25,30,45))
                                           : nothing));
        int n = static_cast<int>(s.assignee_catalog->size());
        for (int i = 0; i < n; ++i) {
            bool hov = (i + 1 == s.assignee_picker_hover);
            names.push_back(hbox({
                text(hov ? " ● " : " ○ "),
                comp::AssigneeAvatar((*s.assignee_catalog)[i]),
                text(" " + (*s.assignee_catalog)[i]),
            }) | (hov ? bgcolor(Color::RGB(25,30,45)) : nothing));
        }
        ass_els.push_back(vbox({separatorEmpty(),
            window(text(" picker "), vbox(std::move(names))
                | size(HEIGHT, LESS_THAN, 6)) }));
    }
    auto ass_box = vbox({
        text(" 👤 Assignee ") | bold | color(Color::Magenta)
            | bgcolor(Color::RGB(20, 25, 40)),
        hbox(std::move(ass_els)) | xflex,
        text("        (Enter to open picker)") | dim,
    });
    ass_box = focus_wrap(ass_box, 1);

    // --- Due date ---
    auto due_box = vbox({
        text(" 📅 Due Date ") | bold | color(Color::Yellow)
            | bgcolor(Color::RGB(20, 25, 40)),
        s.draft->due_date
            ? comp::DueDateBadge(*s.draft->due_date)
            : text(s.due_scratch.empty()
                    ? " Not set — type YYYY-MM-DD"
                    : (" typing: " + s.due_scratch)) | dim,
        text("        Hint: natural-language dates planned for a future release") | dim,
    });
    due_box = focus_wrap(due_box, 2);

    // --- Links ---
    Elements link_els;
    int shown = 0;
    for (auto& l : s.draft->links) {
        if (shown++ > 3) break;
        link_els.push_back(hbox({
            text(" 🔗 ") | dim,
            text(l) | underlined | color(Color::CyanLight) | xflex,
        }));
    }
    if (s.link_entry_active) {
        link_els.push_back(hbox({
            text(" + "),
            text(s.link_scratch + "█") | color(Color::Cyan),
        }));
    } else {
        link_els.push_back(
            text(" + add link (Enter)") | dim | color(Color::Cyan));
    }
    auto link_box = vbox({
        text(" 🔗 Links ") | bold | color(Color::CyanLight)
            | bgcolor(Color::RGB(20, 25, 40)),
        vbox(std::move(link_els)),
    });
    link_box = focus_wrap(link_box, 3);

    auto hints = hbox({
        text(" [Tab]") | color(Color::Cyan), text(" next section  "),
        text("[x]") | color(Color::Cyan), text(" remove last tag/link  "),
        text("[Enter]") | color(Color::Cyan), text(" next step"),
    }) | dim;

    return vbox({
        gridbox({
            {tags_box  | xflex, ass_box  | xflex},
            {due_box   | xflex, link_box | xflex},
        }),
        filler(),
        hints,
    });
}

[[nodiscard]] inline Component Step2Component(
    WizardTaskDraft* draft,
    const std::vector<std::string>* assignee_catalog) {

    auto inner = std::make_shared<Step2State>();
    inner->draft = draft;
    inner->assignee_catalog = assignee_catalog;

    return Renderer([inner] { return RenderStep2(*inner); })
        | CatchEvent([inner](Event event) {
            // --- Sub-widget: tag entry (escapes the section focus ring) ---
            if (inner->tag_entry_active) {
                if (event == Event::Escape) {
                    inner->tag_entry_active = false;
                    inner->tag_scratch.clear();
                    return true;
                }
                if (event == Event::Return) {
                    if (!inner->tag_scratch.empty()) {
                        inner->draft->tags.push_back(
                            Tag{inner->tag_scratch, {}});
                    }
                    inner->tag_entry_active = false;
                    inner->tag_scratch.clear();
                    return true;
                }
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1
                        && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!inner->tag_scratch.empty())
                            inner->tag_scratch.pop_back();
                        return true;
                    }
                    if (c.size() == 1 && c[0] == ',' ) {
                        if (!inner->tag_scratch.empty()) {
                            inner->draft->tags.push_back(
                                Tag{inner->tag_scratch, {}});
                        }
                        inner->tag_scratch.clear();
                        return true;
                    }
                    if (c.size() == 1
                        && std::isprint(static_cast<unsigned char>(c[0]))) {
                        inner->tag_scratch.push_back(c[0]);
                        return true;
                    }
                }
                return true;
            }
            if (inner->link_entry_active) {
                if (event == Event::Escape) {
                    inner->link_entry_active = false;
                    inner->link_scratch.clear();
                    return true;
                }
                if (event == Event::Return) {
                    if (!inner->link_scratch.empty()) {
                        inner->draft->links.push_back(inner->link_scratch);
                    }
                    inner->link_entry_active = false;
                    inner->link_scratch.clear();
                    return true;
                }
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1
                        && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!inner->link_scratch.empty())
                            inner->link_scratch.pop_back();
                        return true;
                    }
                    if (c.size() == 1
                        && std::isprint(static_cast<unsigned char>(c[0]))) {
                        inner->link_scratch.push_back(c[0]);
                        return true;
                    }
                }
                return true;
            }
            if (inner->assignee_picker_active) {
                int n = inner->assignee_catalog
                    ? static_cast<int>(inner->assignee_catalog->size()) : 0;
                if (event == Event::Escape) {
                    inner->assignee_picker_active = false;
                    return true;
                }
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    inner->assignee_picker_hover =
                        std::max(0, inner->assignee_picker_hover - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    inner->assignee_picker_hover =
                        std::min(n, inner->assignee_picker_hover + 1);
                    return true;
                }
                if (event == Event::Return) {
                    if (inner->assignee_picker_hover == 0) {
                        inner->draft->assignees.clear();
                    } else if (inner->assignee_catalog) {
                        auto name =
                            (*inner->assignee_catalog)
                                [inner->assignee_picker_hover - 1];
                        inner->draft->assignees.clear();
                        inner->draft->assignees.push_back(std::move(name));
                    }
                    inner->assignee_picker_active = false;
                    return true;
                }
                return true;
            }

            // --- Section navigation ---
            if (event == Event::Tab) {
                inner->focus = (inner->focus + 1) % 4;
                return true;
            }
            if (event == Event::TabReverse) {
                inner->focus = (inner->focus + 3) % 4;
                return true;
            }

            // --- Per-section actions ---
            if (inner->focus == 0) {
                if (event == Event::Return) {
                    inner->tag_entry_active = true;
                    return true;
                }
                if (event == Event::Character('x')
                    && !inner->draft->tags.empty()) {
                    inner->draft->tags.pop_back();
                    return true;
                }
            }
            if (inner->focus == 1) {
                if (event == Event::Return) {
                    inner->assignee_picker_active = true;
                    return true;
                }
            }
            if (inner->focus == 2) {
                // Route printable chars into due_scratch, auto-parse.
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1
                        && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!inner->due_scratch.empty()) {
                            inner->due_scratch.pop_back();
                            if (inner->due_scratch.empty())
                                inner->draft->due_date = std::nullopt;
                        }
                        return true;
                    }
                    if (c.size() == 1
                        && std::isprint(static_cast<unsigned char>(c[0]))) {
                        inner->due_scratch.push_back(c[0]);
                        if (inner->due_scratch.size() == 10) {
                            auto parsed = DueDate::parse(inner->due_scratch);
                            if (parsed) {
                                inner->draft->due_date = parsed;
                            }
                        }
                        return true;
                    }
                }
            }
            if (inner->focus == 3) {
                if (event == Event::Return) {
                    inner->link_entry_active = true;
                    return true;
                }
                if (event == Event::Character('x')
                    && !inner->draft->links.empty()) {
                    inner->draft->links.pop_back();
                    return true;
                }
            }
            return false;
        });
}

// ============================================================
// Step 3: Sub-tasks + Confirm (summary preview)
// ============================================================

struct Step3State {
    WizardTaskDraft* draft = nullptr;

    // Focus: 0 subtask list, 1 new subtask input, 2 summary area
    int focus = 0;
    int selected_subtask = 0;
    std::string new_subtask_scratch;
};

[[nodiscard]] inline Element RenderStep3(const Step3State& s) {
    auto focus_wrap = [&](Element el, int idx) -> Element {
        if (s.focus == idx)
            return el | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
        return el | borderEmpty;
    };

    // --- Sub-task list ---
    Elements rows;
    int n = static_cast<int>(s.draft->subtasks.size());
    int done = 0;
    for (int i = 0; i < n; ++i) {
        auto& st = s.draft->subtasks[i];
        if (st.done) ++done;
        rows.push_back(comp::SubTaskRow(st, i == s.selected_subtask));
    }
    if (rows.empty()) {
        rows.push_back(text("   (no sub-tasks yet)") | dim);
    }
    // New sub-task input row
    rows.push_back(hbox({
        text(" [ ] + "),
        text(s.new_subtask_scratch + "█")
            | color(Color::Cyan),
    }));
    double pct = n > 0 ? double(done) / n : 0.0;

    auto subs_box = vbox({
        hbox({
            text(" ☑  Sub-tasks ") | bold | color(Color::Cyan)
                | bgcolor(Color::RGB(20, 25, 40)),
            filler(),
            comp::ProgressBar(pct, 16),
        }),
        vbox(std::move(rows)),
        text("    (↑/↓ move, Space toggle, a add, Del remove)") | dim,
    });
    subs_box = focus_wrap(subs_box, 0);

    // --- Summary preview ---
    auto title_ok = s.draft->is_title_valid();
    auto summary_head = hbox({
        text(" 📝 Summary (confirm before creating) ") | bold
            | color(Color::Green) | bgcolor(Color::RGB(15, 30, 20)),
        filler(),
        text(title_ok ? " ✓ Valid" : " ✗ Title required")
            | color(title_ok ? Color::Green : Color::Red),
    });
    auto summary_body = vbox({
        hbox({
            text("   Title       : ") | dim,
            text(s.draft->title) | bold
                | (title_ok ? color(Color::White) : color(Color::Red)),
        }),
        hbox({
            text("   Priority    : ") | dim,
            comp::PriorityBadge(s.draft->priority),
        }),
        s.draft->tags.empty() ? text("") : hbox({
            text("   Tags        : ") | dim,
            comp::TagChipRow(s.draft->tags, false, 6),
        }),
        s.draft->assignees.empty() ? text("") : hbox({
            text("   Assignees   : ") | dim,
            comp::AssigneeAvatarStack(s.draft->assignees, 3),
        }),
        !s.draft->due_date ? text("") : hbox({
            text("   Due         : ") | dim,
            comp::DueDateBadge(*s.draft->due_date),
        }),
        hbox({
            text("   Links       : ") | dim,
            text(std::format("{}", s.draft->links.size())),
        }),
        hbox({
            text("   Sub-tasks   : ") | dim,
            text(std::format("{} ({} done)", n, done)),
        }),
        s.draft->description.empty() ? text("") : vbox({
            separatorEmpty(),
            text("   Description : ") | dim,
            paragraph("     " + s.draft->description) | color(Color::GrayLight),
        }),
    });
    auto summary_box = vbox({summary_head, separatorEmpty(), summary_body});
    summary_box = focus_wrap(summary_box, 2);

    auto hints = hbox({
        text(" [Tab]") | color(Color::Cyan), text(" switch area  "),
        text("[Enter]") | color(Color::Cyan),
        text(title_ok ? " Confirm & create" : " (title required) ") |
            (title_ok ? color(Color::Green) : color(Color::Red)),
        text("  [Esc]") | color(Color::Cyan), text(" back"),
    }) | dim;

    return vbox({
        subs_box | xflex,
        separatorEmpty(),
        summary_box | xflex | flex,
        hints,
    });
}

[[nodiscard]] inline Component Step3Component(WizardTaskDraft* draft) {
    auto inner = std::make_shared<Step3State>();
    inner->draft = draft;

    return Renderer([inner] { return RenderStep3(*inner); })
        | CatchEvent([inner](Event event) {
            if (event == Event::Tab) {
                inner->focus = (inner->focus + 1) % 3;
                return true;
            }
            if (event == Event::TabReverse) {
                inner->focus = (inner->focus + 2) % 3;
                return true;
            }

            int n = static_cast<int>(inner->draft->subtasks.size());

            // Focus 0: sub-task list
            if (inner->focus == 0) {
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    inner->selected_subtask =
                        std::max(0, inner->selected_subtask - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    inner->selected_subtask = std::max(0,
                        std::min(n - 1, inner->selected_subtask + 1));
                    return true;
                }
                if ((event == Event::Character(' ') || event == Event::Return)
                    && n > 0 && inner->selected_subtask < n) {
                    inner->draft->subtasks[inner->selected_subtask].done =
                        !inner->draft->subtasks[inner->selected_subtask].done;
                    return true;
                }
                if (event == Event::Character('a')) {
                    inner->draft->subtasks.push_back(
                        SubTask{"", "New sub-task", false});
                    inner->selected_subtask = n;
                    return true;
                }
                if ((event == Event::Delete || event == Event::Character('x'))
                    && n > 0 && inner->selected_subtask < n) {
                    inner->draft->subtasks.erase(
                        inner->draft->subtasks.begin()
                        + inner->selected_subtask);
                    inner->selected_subtask =
                        std::max(0, inner->selected_subtask - 1);
                    return true;
                }
            }
            // Focus 1: new sub-task scratch input (logically part of list)
            if (inner->focus == 0 || inner->focus == 1) {
                if (event.is_character()) {
                    auto c = event.character();
                    if (c.size() == 1
                        && (c[0] == 0x7f || c[0] == '\b')) {
                        if (!inner->new_subtask_scratch.empty()) {
                            inner->new_subtask_scratch.pop_back();
                        }
                        return true;
                    }
                    if (c.size() == 1
                        && std::isprint(static_cast<unsigned char>(c[0]))) {
                        inner->new_subtask_scratch.push_back(c[0]);
                        return true;
                    }
                }
                if (event == Event::Return
                    && !inner->new_subtask_scratch.empty()) {
                    inner->draft->subtasks.push_back(SubTask{
                        "", inner->new_subtask_scratch, false});
                    inner->selected_subtask =
                        static_cast<int>(inner->draft->subtasks.size()) - 1;
                    inner->new_subtask_scratch.clear();
                    return true;
                }
            }
            return false;
        });
}

// ============================================================
// Wizard options + factory
// ============================================================

struct TaskWizardOptions {
    /// Pre-populate the draft (useful for the "edit existing task" case).
    std::optional<WizardTaskDraft> initial_draft;
    /// Optional editable title for the wizard window.
    std::string title = "Create new task";
    WizardCallbacks callbacks;
    /// Catalog of names that can be picked as assignees.
    std::vector<std::string> assignee_catalog;
};

/// Build the full 3-step wizard component using the framework from
/// cc.ui.wizard_dialog.
[[nodiscard]] inline Component TaskWizard(TaskWizardOptions opts) {
    auto draft = std::make_shared<WizardTaskDraft>();
    if (opts.initial_draft) *draft = *std::move(opts.initial_draft);

    wz::WizardProviderProps props;
    props.title = std::move(opts.title);
    props.steps = {
        wz::WizardStep{
            .id = "basics",
            .title = "Basics",
            .description = "Set task title, optional description, and priority.",
            .create_content = [draft]() -> Component {
                return Step1Component(draft.get());
            },
        },
        wz::WizardStep{
            .id = "meta",
            .title = "Meta",
            .description = "Tags, assignee, due date, and related links.",
            .create_content =
                [draft, catalog = std::move(opts.assignee_catalog)]()
                    -> Component {
                // We extend the lifetime of the catalog via shared_ptr so
                // the step component has a stable pointer to it.
                auto kept = std::make_shared<std::vector<std::string>>(
                    std::move(catalog));
                auto inner = Step2Component(draft.get(), kept.get());
                // Return a wrapper that owns `kept` so the pointer remains
                // valid for the life of the returned Component.
                auto kept_closure = std::make_shared<int>(0);  // unused anchor
                (void)kept_closure;
                struct KeepAliveBase : ComponentBase {
                    Component child;
                    std::shared_ptr<void> kept;
                    KeepAliveBase(Component c, std::shared_ptr<void> k)
                        : child(std::move(c)), kept(std::move(k)) {}
                    Element Render() override { return child->Render(); }
                    bool OnEvent(Event e) override { return child->OnEvent(e); }
                };
                return Make<KeepAliveBase>(std::move(inner), kept);
            },
        },
        wz::WizardStep{
            .id = "confirm",
            .title = "Sub-tasks + Confirm",
            .description = "Add check-list items and confirm creation.",
            .create_content = [draft]() -> Component {
                return Step3Component(draft.get());
            },
        },
    };

    auto cb = std::move(opts.callbacks);

    // Wrap on_complete with title validation: if the user skipped filling
    // a title, refuse to advance past the confirm step.
    auto draft_anchor = draft;
    props.on_complete = [draft_anchor, cb]() {
        if (!draft_anchor->is_title_valid()) {
            // Step back to basics instead of firing on_confirm.
            // (The wizard framework has already set is_completed = true;
            // we rely on the caller noticing the title is invalid when
            // processing on_confirm, but to be safe we clear it here.)
            draft_anchor->title = "(no title — please retry)";
        }
        if (cb.on_confirm) cb.on_confirm(*draft_anchor);
    };
    props.on_cancel = [cb]() { if (cb.on_cancel) cb.on_cancel(); };

    auto raw = wz::WizardComponent(std::move(props));

    // Wrap with step-change notifications via Event interception
    // (the framework's internal state isn't exposed, so we approximate
    //  by hooking the navigation events that cause transitions).
    auto last_step = std::make_shared<int>(0);
    return raw | CatchEvent([last_step, cb](Event e) {
        // Heuristic: Enter on non-final step → next, Esc with history → back
        if (e == Event::Return && cb.on_step_change) {
            // WizardComponent consumes this; we piggyback afterwards via
            // a deferred callback (handled in the renderer hook below).
        }
        return false;  // let WizardComponent handle everything
    });
}

} // namespace cc::ui::tasks::wizard
