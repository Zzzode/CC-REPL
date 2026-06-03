/// @file structured_diff.cppm
/// @brief Structured diff rendering with word-level highlighting, syntax
/// coloring, and hunk navigation. Migrated from StructuredDiff/colorDiff.ts
/// and diff/DiffDetailView.tsx.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <format>
#include <cstdint>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.structured_diff;

import cc.types.types;

export namespace cc::ui::structured_diff {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Reason the color diff module might be unavailable
enum class ColorModuleUnavailableReason : std::uint8_t {
    Env,        // Disabled via environment variable
    Missing,    // Native module not found
};

/// Word-level change within a line
struct WordChange {
    int start_col;
    int end_col;
    bool is_added;      // true = added, false = removed
};

/// A single diff line with word-level annotations
struct StructuredDiffLine {
    enum class Type : std::uint8_t {
        Context,
        Added,
        Removed,
        Empty,
    };

    Type type;
    std::string content;
    std::optional<int> old_line_num;
    std::optional<int> new_line_num;
    std::vector<WordChange> word_changes;   // Word-level highlights
};

/// A hunk in a structured patch
struct StructuredPatchHunk {
    int old_start = 0;
    int old_lines = 0;
    int new_start = 0;
    int new_lines = 0;
    std::string header;
    std::vector<StructuredDiffLine> lines;
};

/// Props for DiffDetailView (single file diff display)
struct DiffDetailViewProps {
    std::string file_path;
    std::vector<StructuredPatchHunk> hunks;
    bool is_large_file = false;
    bool is_binary = false;
    bool is_truncated = false;
    bool is_untracked = false;
};

/// Syntax theme reference for diff coloring
struct DiffSyntaxTheme {
    std::string name;
    Color added_fg        = Color::Green;
    Color removed_fg      = Color::Red;
    Color context_fg      = Color::GrayLight;
    Color added_bg        = Color::RGB(0, 30, 0);
    Color removed_bg      = Color::RGB(30, 0, 0);
    Color added_word_bg   = Color::RGB(0, 60, 0);
    Color removed_word_bg = Color::RGB(60, 0, 0);
    Color hunk_header_fg  = Color::Cyan;
    Color line_num_fg     = Color::GrayDark;
};

/// Options for the structured diff component
struct StructuredDiffOptions {
    DiffDetailViewProps diff;
    std::optional<DiffSyntaxTheme> theme;
    bool show_line_numbers = true;
    int context_lines = 3;
    int max_display_lines = 400;
    int visible_height = 30;
    int scroll_offset = 0;
    std::function<void(int line)> on_line_select;
    std::function<void()> on_close;
};

// ============================================================
// Color Module Availability (from colorDiff.ts)
// ============================================================

/// Check if the color diff module is available
[[nodiscard]] inline std::optional<ColorModuleUnavailableReason>
get_color_module_unavailable_reason() {
    // The native color diff module is built into this target.
    return std::nullopt;
}

/// Get a syntax theme by name
[[nodiscard]] inline std::expected<DiffSyntaxTheme, std::string>
get_diff_syntax_theme(const std::string& theme_name) {
    DiffSyntaxTheme theme;
    theme.name = theme_name;

    if (theme_name == "dark" || theme_name == "default") {
        return theme;
    }
    if (theme_name == "light") {
        theme.added_fg = Color::RGB(0, 100, 0);
        theme.removed_fg = Color::RGB(150, 0, 0);
        theme.context_fg = Color::RGB(60, 60, 60);
        theme.added_bg = Color::RGB(220, 255, 220);
        theme.removed_bg = Color::RGB(255, 220, 220);
        theme.added_word_bg = Color::RGB(180, 255, 180);
        theme.removed_word_bg = Color::RGB(255, 180, 180);
        return theme;
    }
    if (theme_name == "monokai") {
        theme.added_fg = Color::RGB(166, 226, 46);
        theme.removed_fg = Color::RGB(249, 38, 114);
        theme.hunk_header_fg = Color::RGB(102, 217, 239);
        return theme;
    }
    return std::unexpected("Unknown diff theme: " + theme_name);
}

// ============================================================
// Rendering Helpers
// ============================================================

/// Get the default theme
[[nodiscard]] inline DiffSyntaxTheme default_theme() {
    return DiffSyntaxTheme{};
}

/// Render a word-highlighted line content
[[nodiscard]] inline Element RenderWordHighlightedContent(
    const StructuredDiffLine& line, const DiffSyntaxTheme& theme) {

    if (line.word_changes.empty()) {
        Color fg;
        switch (line.type) {
            case StructuredDiffLine::Type::Added:   fg = theme.added_fg; break;
            case StructuredDiffLine::Type::Removed: fg = theme.removed_fg; break;
            default:                                fg = theme.context_fg; break;
        }
        return text(line.content) | color(fg);
    }

    // Build element with word-level highlighting
    Elements parts;
    int pos = 0;
    Color base_fg = (line.type == StructuredDiffLine::Type::Added)
                    ? theme.added_fg : theme.removed_fg;

    for (const auto& change : line.word_changes) {
        // Text before the change
        if (change.start_col > pos) {
            parts.push_back(
                text(line.content.substr(pos, change.start_col - pos))
                | color(base_fg));
        }

        // The changed word
        int len = change.end_col - change.start_col;
        auto word_bg = change.is_added ? theme.added_word_bg
                                       : theme.removed_word_bg;
        parts.push_back(
            text(line.content.substr(change.start_col, len))
            | color(base_fg) | bold | bgcolor(word_bg));

        pos = change.end_col;
    }

    // Remaining text
    if (pos < static_cast<int>(line.content.size())) {
        parts.push_back(
            text(line.content.substr(pos)) | color(base_fg));
    }

    return hbox(parts);
}

/// Render a single structured diff line
[[nodiscard]] inline Element RenderStructuredDiffLine(
    const StructuredDiffLine& line, const DiffSyntaxTheme& theme,
    bool show_line_nums, int gutter_width, bool selected) {

    Elements parts;

    // Line numbers
    if (show_line_nums) {
        std::string old_num = line.old_line_num
            ? std::format("{:>{}}", *line.old_line_num, gutter_width) 
            : std::string(gutter_width, ' ');
        std::string new_num = line.new_line_num
            ? std::format("{:>{}}", *line.new_line_num, gutter_width)
            : std::string(gutter_width, ' ');

        parts.push_back(text(old_num) | color(theme.line_num_fg));
        parts.push_back(text(" "));
        parts.push_back(text(new_num) | color(theme.line_num_fg));
        parts.push_back(text(" │ ") | dim);
    }

    // Prefix character
    switch (line.type) {
        case StructuredDiffLine::Type::Added:
            parts.push_back(text("+") | color(theme.added_fg) | bold);
            break;
        case StructuredDiffLine::Type::Removed:
            parts.push_back(text("-") | color(theme.removed_fg) | bold);
            break;
        default:
            parts.push_back(text(" "));
            break;
    }
    parts.push_back(text(" "));

    // Content with word highlighting
    parts.push_back(RenderWordHighlightedContent(line, theme));

    auto result = hbox(parts);

    // Background tinting
    if (line.type == StructuredDiffLine::Type::Added) {
        result = result | bgcolor(theme.added_bg);
    } else if (line.type == StructuredDiffLine::Type::Removed) {
        result = result | bgcolor(theme.removed_bg);
    }

    if (selected) {
        result = result | bgcolor(Color::RGB(40, 40, 60));
    }

    return result;
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a binary file placeholder
[[nodiscard]] inline Element RenderBinaryFile() {
    return vbox({
        text("") | dim,
        text("  Binary file — cannot display diff") | dim | center,
        text("") | dim,
    });
}

/// Render an untracked file notice
[[nodiscard]] inline Element RenderUntrackedFile(const std::string& path) {
    return vbox({
        hbox({
            text(" + ") | color(Color::Green) | bold,
            text(path) | bold,
            text(" (new file)") | dim,
        }),
        separator(),
        text("  Untracked file — full content not shown in diff") | dim,
    });
}

/// Render a hunk header
[[nodiscard]] inline Element RenderHunkHeader(const StructuredPatchHunk& hunk,
                                               const DiffSyntaxTheme& theme) {
    auto header_text = std::format("@@ -{},{} +{},{} @@",
        hunk.old_start, hunk.old_lines, hunk.new_start, hunk.new_lines);
    if (!hunk.header.empty()) {
        header_text += " " + hunk.header;
    }
    return text(" " + header_text) | color(theme.hunk_header_fg)
           | bgcolor(Color::RGB(0, 20, 30));
}

/// Render the full structured diff view
[[nodiscard]] inline Element RenderStructuredDiff(const StructuredDiffOptions& opts) {
    const auto& diff = opts.diff;
    auto theme = opts.theme.value_or(default_theme());

    // Special cases
    if (diff.is_binary) {
        return RenderBinaryFile();
    }
    if (diff.is_untracked) {
        return RenderUntrackedFile(diff.file_path);
    }

    // File header
    Elements elements;
    elements.push_back(hbox({
        text(" ") | dim,
        text(diff.file_path) | bold,
        diff.is_large_file ? (text(" (large file)") | dim | color(Color::Yellow))
                           : text(""),
    }));
    elements.push_back(separator());

    // Compute total line count for gutter width
    int max_line = 0;
    for (const auto& hunk : diff.hunks) {
        for (const auto& line : hunk.lines) {
            if (line.new_line_num) max_line = std::max(max_line, *line.new_line_num);
            if (line.old_line_num) max_line = std::max(max_line, *line.old_line_num);
        }
    }
    int gutter_width = std::max(3, static_cast<int>(std::format("{}", max_line).size()));

    // Render hunks
    int total_rendered = 0;
    for (const auto& hunk : diff.hunks) {
        elements.push_back(RenderHunkHeader(hunk, theme));

        for (const auto& line : hunk.lines) {
            if (total_rendered >= opts.max_display_lines) break;

            if (total_rendered >= opts.scroll_offset &&
                total_rendered < opts.scroll_offset + opts.visible_height) {
                elements.push_back(RenderStructuredDiffLine(
                    line, theme, opts.show_line_numbers, gutter_width, false));
            }
            total_rendered++;
        }
        if (total_rendered >= opts.max_display_lines) break;
    }

    // Truncation notice
    if (diff.is_truncated || total_rendered >= opts.max_display_lines) {
        elements.push_back(
            text("  ... diff truncated (too many lines) ...") | dim | center);
    }

    if (elements.size() <= 2) {
        elements.push_back(text("  (no changes)") | dim);
    }

    return vbox(elements) | vscroll_indicator | yframe | flex;
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive structured diff component
[[nodiscard]] inline Component StructuredDiff(StructuredDiffOptions options) {
    struct State {
        StructuredDiffOptions opts;
        int selected_line = 0;
    };
    auto state = std::make_shared<State>();
    state->opts = std::move(options);

    return Renderer([state] {
        return RenderStructuredDiff(state->opts);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->opts.scroll_offset++;
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->opts.scroll_offset = std::max(0, state->opts.scroll_offset - 1);
            return true;
        }
        if (event == Event::PageDown) {
            state->opts.scroll_offset += state->opts.visible_height;
            return true;
        }
        if (event == Event::PageUp) {
            state->opts.scroll_offset = std::max(
                0, state->opts.scroll_offset - state->opts.visible_height);
            return true;
        }
        if (event == Event::Home) {
            state->opts.scroll_offset = 0;
            return true;
        }
        if (event == Event::Character('q') || event == Event::Escape) {
            if (state->opts.on_close) state->opts.on_close();
            return true;
        }
        if (event == Event::Return) {
            if (state->opts.on_line_select) {
                state->opts.on_line_select(state->opts.scroll_offset);
            }
            return true;
        }
        return false;
    });
}

/// Convenience: render a quick diff element (non-interactive)
[[nodiscard]] inline Element QuickDiff(
    const std::string& file_path,
    const std::vector<StructuredPatchHunk>& hunks) {

    DiffDetailViewProps diff;
    diff.file_path = file_path;
    diff.hunks = hunks;

    StructuredDiffOptions opts;
    opts.diff = std::move(diff);
    opts.visible_height = 50;
    opts.max_display_lines = 200;

    return RenderStructuredDiff(opts);
}

} // namespace cc::ui::structured_diff
