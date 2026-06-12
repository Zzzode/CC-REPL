/// @file task_components.cppm
/// @brief Reusable visual widgets shared across Tasks UI subsystems.
///   Consolidates 12 TS source components' common widgets: priority badges,
///   status pills, tag chips, assignee avatars, progress bars, due-date badges.
///   Shared by: task_list_view, task_details_dialog, task_wizard, and by
///   sibling agents UI13 (Agent view) and UI14 (Team view).
module;

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <format>
#include <functional>
#include <algorithm>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.tasks.task_components;

export namespace cc::ui::tasks::components {
using namespace ftxui;

// ============================================================
// Priority
// ============================================================

/// Five-level priority model matching the TypeScript tasks subsystem.
enum class Priority : std::uint8_t {
    None,      // ⚪  no priority set
    Low,       // 🟢  low impact
    Medium,    // 🟡  default
    High,      // 🟠  important
    Urgent,    // 🔴  drop-everything
};

/// Convert priority to its unicode emoji marker.
[[nodiscard]] inline std::string priority_emoji(Priority p) {
    switch (p) {
        case Priority::Urgent: return "🔴";
        case Priority::High:   return "🟠";
        case Priority::Medium: return "🟡";
        case Priority::Low:    return "🟢";
        case Priority::None:   return "⚪";
    }
    return "⚪";
}

/// Convert priority to a short display label.
[[nodiscard]] inline std::string priority_label(Priority p) {
    switch (p) {
        case Priority::Urgent: return "Urgent";
        case Priority::High:   return "High";
        case Priority::Medium: return "Medium";
        case Priority::Low:    return "Low";
        case Priority::None:   return "None";
    }
    return "None";
}

/// FTXUI accent color for each priority.
[[nodiscard]] inline Color priority_color(Priority p) {
    switch (p) {
        case Priority::Urgent: return Color::Red;
        case Priority::High:   return Color::Orange1;
        case Priority::Medium: return Color::Yellow;
        case Priority::Low:    return Color::Green;
        case Priority::None:   return Color::GrayLight;
    }
    return Color::GrayLight;
}

/// PriorityBadge(p) - left color bar + emoji + text label.
/// Layout:  [bar] [emoji] [label]
[[nodiscard]] inline Element PriorityBadge(Priority p, bool compact = false) {
    auto accent = priority_color(p);
    auto emoji = priority_emoji(p);
    auto label = priority_label(p);

    Elements parts;
    parts.push_back(separatorCharacter("│") | color(accent));
    parts.push_back(text(" " + emoji));
    if (!compact) {
        parts.push_back(text(" " + label) | color(accent));
    }
    return hbox(std::move(parts));
}

// ============================================================
// Status
// ============================================================

/// Task workflow status (standardised across list/dialog/wizard).
/// Maps to the five Kanban columns + Cancelled.
enum class Status : std::uint8_t {
    Todo,       // backlog, not started
    InProgress, // actively being worked on
    Review,     // code review / verification
    Done,       // completed
    Cancelled,  // explicitly cancelled
};

/// Status to display string.
[[nodiscard]] inline std::string status_label(Status s) {
    switch (s) {
        case Status::Todo:       return "Todo";
        case Status::InProgress: return "In Progress";
        case Status::Review:     return "Review";
        case Status::Done:       return "Done";
        case Status::Cancelled:  return "Cancelled";
    }
    return "Unknown";
}

/// Background fill color for StatusPill.
[[nodiscard]] inline Color status_pill_bg(Status s) {
    switch (s) {
        case Status::Todo:       return Color::GrayDark;    // grey
        case Status::InProgress: return Color::Blue;        // blue
        case Status::Review:     return Color::Purple;      // purple
        case Status::Done:       return Color::Green;       // green
        case Status::Cancelled:  return Color::Red;         // red
    }
    return Color::GrayDark;
}

/// Foreground text color for StatusPill.
[[nodiscard]] inline Color status_pill_fg(Status s) {
    switch (s) {
        case Status::Todo:       return Color::GrayLight;
        case Status::InProgress: return Color::White;
        case Status::Review:     return Color::White;
        case Status::Done:       return Color::White;
        case Status::Cancelled:  return Color::White;
    }
    return Color::White;
}

/// StatusPill(status) - rounded coloured pill with status label.
/// Layout:  ( [label] ) with the background tinted per status.
[[nodiscard]] inline Element StatusPill(Status s) {
    auto bg = status_pill_bg(s);
    auto fg = status_pill_fg(s);
    return hbox({
        text(" " + status_label(s) + " ") | color(fg) | bgcolor(bg)
            | size(WIDTH, GREATER_THAN, 10),
    });
}

// ============================================================
// Tag
// ============================================================

/// A single task tag (free-form label + optional color).
struct Tag {
    std::string text;
    std::optional<Color> color;
};

/// TagChip(text, removable) - capsule-shaped tag chip.
/// If `removable=true` shows ✕ character on the right that can be
/// wired into a click handler via TagChipComponent.
[[nodiscard]] inline Element TagChip(const Tag& tag, bool removable = true) {
    auto c = tag.color.value_or(Color::CyanLight);
    Elements parts;
    parts.push_back(text(" " + tag.text + " ") | color(c) | bgcolor(Color::RGB(30, 40, 60)));
    if (removable) {
        parts.push_back(text("✕") | color(Color::GrayDark) | dim);
    }
    return hbox(std::move(parts)) | borderEmpty;
}

/// Render a row of tag chips, wrapping via flexbox.
[[nodiscard]] inline Element TagChipRow(const std::vector<Tag>& tags,
                                        bool removable = true,
                                        int max_visible = 8) {
    Elements chips;
    int count = std::min(max_visible, static_cast<int>(tags.size()));
    for (int i = 0; i < count; ++i) {
        chips.push_back(TagChip(tags[i], removable));
        if (i < count - 1) chips.push_back(text(" "));
    }
    if (static_cast<int>(tags.size()) > max_visible) {
        chips.push_back(text(" "));
        chips.push_back(
            text(std::format("+{}", static_cast<int>(tags.size()) - max_visible))
            | dim | color(Color::GrayLight));
    }
    return flexbox(std::move(chips), FlexboxConfig{
        .direction = FlexboxConfig::Direction::Row,
        .wrap = FlexboxConfig::Wrap::Wrap,
    });
}

/// Interactive tag chip component that notifies when ✕ is clicked.
[[nodiscard]] inline Component TagChipComponent(Tag tag,
                                                std::function<void()> on_remove) {
    auto state = std::make_shared<Tag>(std::move(tag));
    auto cb = std::move(on_remove);
    return Renderer([state] { return TagChip(*state, true); })
        | CatchEvent([cb](Event event) {
            // FTXUI does not have per-chip mouse click routing; the
            // convention is that "x" keypress when focused removes.
            if (event == Event::Character('x') || event == Event::Delete) {
                if (cb) cb();
                return true;
            }
            return false;
        });
}

// ============================================================
// Assignee
// ============================================================

/// Compute a stable color for a name (hash-based so the same assignee
/// always displays with the same avatar tint).
[[nodiscard]] inline Color avatar_color(std::string_view name) {
    std::uint32_t h = 0;
    for (char c : name) {
        h = (h * 131) + static_cast<unsigned char>(c);
    }
    static const Color palette[] = {
        Color::BlueLight, Color::CyanLight, Color::GreenLight,
        Color::MagentaLight, Color::RedLight, Color::YellowLight,
        Color::Purple, Color::Orange1, Color::Aquamarine1,
    };
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

/// Extract the initials (first letter of up to the first two words).
[[nodiscard]] inline std::string initials(std::string_view name) {
    if (name.empty()) return "?";
    std::string out;
    bool next = true;
    for (char c : name) {
        if (next && std::isalpha(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(c)));
            next = false;
            if (out.size() >= 2) break;
        }
        if (c == ' ' || c == '-' || c == '.' || c == '_') next = true;
    }
    if (out.empty()) out.push_back(std::toupper(name[0]));
    return out;
}

/// AssigneeAvatar(name) - circular-ish badge with initials + tint.
[[nodiscard]] inline Element AssigneeAvatar(std::string_view name) {
    auto c = avatar_color(name);
    auto init = initials(name);
    return hbox({
        text(" " + init + " ") | bold | color(Color::Black) | bgcolor(c),
    });
}

/// AssigneeAvatar for an unassigned slot.
[[nodiscard]] inline Element AssigneeAvatarUnassigned() {
    return hbox({
        text(" 👤 ") | color(Color::GrayDark),
        text("Unassigned") | dim,
    });
}

/// Render N avatars stacked (+N overflow chip when >max).
[[nodiscard]] inline Element AssigneeAvatarStack(const std::vector<std::string>& names,
                                                 int max_visible = 3) {
    Elements parts;
    int n = static_cast<int>(names.size());
    int shown = std::min(n, max_visible);
    for (int i = 0; i < shown; ++i) {
        parts.push_back(AssigneeAvatar(names[i]));
        if (i < shown - 1) parts.push_back(text(" "));
    }
    if (n > max_visible) {
        parts.push_back(text(" "));
        parts.push_back(
            hbox({
                text(std::format("+{}", n - max_visible))
                    | dim | color(Color::GrayLight)
                    | bgcolor(Color::GrayDark),
            }));
    }
    return hbox(std::move(parts));
}

// ============================================================
// Progress bar (character-art variant for narrow spaces)
// ============================================================

/// ProgressBar(percent, width) - e.g. "[██████░░░░] 60%".
/// `width` is the width of the gauge (excluding brackets and label).
[[nodiscard]] inline Element ProgressBar(double percent, int width = 20,
                                         bool show_label = true) {
    percent = std::clamp(percent, 0.0, 1.0);
    int filled = static_cast<int>(percent * width);
    int empty = width - filled;

    Color fill_c;
    if (percent >= 1.0) fill_c = Color::Green;
    else if (percent >= 0.75) fill_c = Color::Cyan;
    else if (percent >= 0.3) fill_c = Color::Yellow;
    else fill_c = Color::GrayLight;

    std::string bar;
    bar.push_back('[');
    for (int i = 0; i < filled; ++i) bar += "█";
    for (int i = 0; i < empty;  ++i) bar += "░";
    bar.push_back(']');

    Elements parts;
    parts.push_back(text(bar.substr(0, 1)) | dim);
    parts.push_back(text(bar.substr(1, filled)) | color(fill_c));
    parts.push_back(text(bar.substr(1 + filled)) | dim);
    if (show_label) {
        parts.push_back(text(std::format(" {:>3.0f}%", percent * 100)));
    }
    return hbox(std::move(parts));
}

// ============================================================
// Due date
// ============================================================

/// Urgency tier for a due date.
enum class DueUrgency : std::uint8_t {
    Normal,   // more than 7 days left
    ThisWeek, // 4-7 days left  🟨
    Soon,     // 1-3 days left  🟧
    Overdue,  // past due date  🟥
};

/// A due date parsed from input (YYYY-MM-DD).
/// TODO(UI12): accept natural-language ("Tomorrow", "Next Monday") once
///  a date parser is available.
struct DueDate {
    std::chrono::system_clock::time_point date;

    /// Parse YYYY-MM-DD.  Returns std::nullopt on parse failure.
    [[nodiscard]] static std::optional<DueDate> parse(std::string_view s) {
        // Minimal parser: expects exactly the form "YYYY-MM-DD".
        if (s.size() != 10) return std::nullopt;
        if (s[4] != '-' || s[7] != '-') return std::nullopt;
        auto to_int = [](std::string_view sub) -> std::optional<int> {
            int v = 0;
            for (char c : sub) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
                v = v * 10 + (c - '0');
            }
            return v;
        };
        auto y = to_int(s.substr(0, 4));
        auto m = to_int(s.substr(5, 2));
        auto d = to_int(s.substr(8, 2));
        if (!y || !m || !d) return std::nullopt;

        std::tm tm{};
        tm.tm_year = *y - 1900;
        tm.tm_mon = *m - 1;
        tm.tm_mday = *d;
        // mktime treats as local; that is close enough for display math.
        auto t = std::mktime(&tm);
        if (t == -1) return std::nullopt;
        return DueDate{std::chrono::system_clock::from_time_t(t)};
    }

    /// Format as YYYY-MM-DD.
    [[nodiscard]] std::string format_iso() const {
        auto t = std::chrono::system_clock::to_time_t(date);
        std::tm tm{};
        localtime_r(&t, &tm);
        return std::format("{:04d}-{:02d}-{:02d}",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }

    /// Days between today and this due date.  Negative = overdue.
    [[nodiscard]] long long days_from_today() const {
        auto now = std::chrono::system_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::hours>(date - now).count();
        // Round to nearest day boundary.
        return (diff + 12) / 24;
    }

    [[nodiscard]] DueUrgency urgency() const {
        auto d = days_from_today();
        if (d < 0)  return DueUrgency::Overdue;
        if (d <= 3) return DueUrgency::Soon;
        if (d <= 7) return DueUrgency::ThisWeek;
        return DueUrgency::Normal;
    }

    /// Human-readable relative description ("3d left", "overdue 2d").
    [[nodiscard]] std::string relative() const {
        auto d = days_from_today();
        if (d == 0)     return "Today";
        if (d == 1)     return "Tomorrow";
        if (d >  0)     return std::format("{}d left", d);
        return std::format("overdue {}d", -d);
    }
};

/// Emoji + urgency color.
[[nodiscard]] inline std::pair<std::string, Color> due_urgency_display(DueUrgency u) {
    switch (u) {
        case DueUrgency::Overdue:  return {"🟥", Color::Red};
        case DueUrgency::Soon:     return {"🟧", Color::Orange1};
        case DueUrgency::ThisWeek: return {"🟨", Color::Yellow};
        case DueUrgency::Normal:   return {"⚪", Color::GrayLight};
    }
    return {"⚪", Color::GrayLight};
}

/// DueDateBadge(date) - emoji + ISO date + relative time, color-coded.
[[nodiscard]] inline Element DueDateBadge(const DueDate& due) {
    auto [emoji, col] = due_urgency_display(due.urgency());
    return hbox({
        text(emoji + " "),
        text(due.format_iso()) | color(col),
        text(" (" + due.relative() + ")") | dim,
    });
}

/// DueDateBadge for optional (empty) due date.
[[nodiscard]] inline Element DueDateBadge(std::optional<DueDate> due) {
    if (!due) {
        return text(" No due date") | dim;
    }
    return DueDateBadge(*due);
}

// ============================================================
// Checklist row (sub-tasks, used by list, dialog, wizard)
// ============================================================

/// A single sub-task entry.
struct SubTask {
    std::string id;
    std::string text;
    bool done = false;
};

/// Render one sub-task as a checkbox row.
[[nodiscard]] inline Element SubTaskRow(const SubTask& st, bool selected = false) {
    auto check = st.done ? "[✓]" : "[ ]";
    auto col = st.done ? Color::Green : Color::GrayLight;
    auto line = hbox({
        text(" " + std::string(check) + " ") | color(col),
        text(st.text)
            | (st.done ? color(Color::GrayDark) : color(Color::White))
            | (st.done ? strikethrough : nothing)
            | (selected ? bold : nothing),
    });
    if (selected) {
        line = line | bgcolor(Color::RGB(25, 30, 45));
    }
    return line;
}

/// SubTaskList - renders a checklist with keyboard navigation.
/// On Enter toggles completion of the currently selected sub-task.
/// Exposes `on_add` callback for "+ Add sub-task".
struct SubTaskListOptions {
    std::vector<SubTask> tasks;
    int selected = 0;
    std::function<void(int)> on_toggle;
    std::function<void()> on_add;
};

[[nodiscard]] inline Component SubTaskList(SubTaskListOptions opts) {
    struct State {
        SubTaskListOptions opts;
    };
    auto s = std::make_shared<State>();
    s->opts = std::move(opts);

    return Renderer([s] {
        Elements rows;
        for (int i = 0; i < static_cast<int>(s->opts.tasks.size()); ++i) {
            rows.push_back(
                SubTaskRow(s->opts.tasks[i], i == s->opts.selected));
        }
        if (s->opts.on_add) {
            rows.push_back(text("   + Add sub-task") | color(Color::Cyan) | dim);
        }
        if (rows.empty()) {
            rows.push_back(text(" No sub-tasks") | dim);
        }
        return vbox(std::move(rows));
    }) | CatchEvent([s](Event event) {
        int n = static_cast<int>(s->opts.tasks.size());
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            s->opts.selected = std::max(0, s->opts.selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            s->opts.selected = std::max(0, std::min(n - 1, s->opts.selected + 1));
            return true;
        }
        if (event == Event::Character(' ') || event == Event::Return) {
            if (n > 0 && s->opts.selected < n && s->opts.on_toggle) {
                s->opts.tasks[s->opts.selected].done
                    = !s->opts.tasks[s->opts.selected].done;
                s->opts.on_toggle(s->opts.selected);
                return true;
            }
        }
        if (event == Event::Character('a') && s->opts.on_add) {
            s->opts.on_add();
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::tasks::components
