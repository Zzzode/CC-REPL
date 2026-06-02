/// @file diff_view.cppm
/// @brief Diff difference comparison view - renders unified/split diffs with
/// syntax highlighting, line numbers, and navigation.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.diff_view;

import cc.types.types;

export namespace cc::ui::components::diff_view {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Type of diff line
enum class DiffLineType : std::uint8_t {
    Context,    // Unchanged line
    Added,      // New line (green)
    Removed,    // Deleted line (red)
    Modified,   // Modified (shown as remove+add pair)
    Header,     // @@ hunk header @@
    FileHeader, // --- a/ / +++ b/ header
    Empty,      // Empty line / no-newline marker
};

/// A single line in a diff
struct DiffLine {
    DiffLineType type;
    std::string content;
    std::optional<int> old_line_num;    // Line number in old file
    std::optional<int> new_line_num;    // Line number in new file
    bool has_trailing_whitespace = false;
    std::vector<std::pair<int, int>> highlights;  // Character ranges to highlight
};

/// A hunk in the diff
struct DiffHunk {
    int old_start = 0;
    int old_count = 0;
    int new_start = 0;
    int new_count = 0;
    std::string header;     // Optional function context
    std::vector<DiffLine> lines;
};

/// A complete file diff
struct FileDiff {
    std::string old_path;
    std::string new_path;
    std::string status;         // "added", "modified", "deleted", "renamed"
    std::vector<DiffHunk> hunks;
    int additions = 0;
    int deletions = 0;
    bool is_binary = false;
    bool is_truncated = false;
};

/// Display mode for the diff view
enum class DiffDisplayMode : std::uint8_t {
    Unified,    // Traditional unified diff
    Split,      // Side-by-side view
    Inline,     // Inline with word-level highlighting
};

/// Options for the diff view component
struct DiffViewOptions {
    std::vector<FileDiff> files;
    int selected_file = 0;
    int selected_line = 0;
    int scroll_offset = 0;
    DiffDisplayMode mode = DiffDisplayMode::Unified;
    bool show_line_numbers = true;
    bool show_whitespace = false;
    int context_lines = 3;
    std::function<void(int file_index, int line_index)> on_select;
    std::function<void(int file_index)> on_file_select;
    std::function<void()> on_close;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get color for diff line type
[[nodiscard]] inline Color line_color(DiffLineType type) {
    switch (type) {
        case DiffLineType::Added:      return Color::Green;
        case DiffLineType::Removed:    return Color::Red;
        case DiffLineType::Modified:   return Color::Yellow;
        case DiffLineType::Header:     return Color::Cyan;
        case DiffLineType::FileHeader: return Color::White;
        case DiffLineType::Context:    return Color::GrayLight;
        case DiffLineType::Empty:      return Color::GrayDark;
    }
    return Color::White;
}

/// Get prefix character for diff line
[[nodiscard]] inline std::string line_prefix(DiffLineType type) {
    switch (type) {
        case DiffLineType::Added:   return "+";
        case DiffLineType::Removed: return "-";
        case DiffLineType::Header:  return "@";
        default:                    return " ";
    }
}

/// Get status badge color and icon
[[nodiscard]] inline std::pair<std::string, Color> status_badge(const std::string& status) {
    if (status == "added")    return {"A", Color::Green};
    if (status == "modified") return {"M", Color::Yellow};
    if (status == "deleted")  return {"D", Color::Red};
    if (status == "renamed")  return {"R", Color::Cyan};
    return {"?", Color::White};
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single diff line
[[nodiscard]] inline Element RenderDiffLine(
    const DiffLine& line, bool selected, bool show_line_nums) {

    Elements parts;

    // Line numbers
    if (show_line_nums) {
        std::string old_num = line.old_line_num
            ? std::format("{:4}", *line.old_line_num) : "    ";
        std::string new_num = line.new_line_num
            ? std::format("{:4}", *line.new_line_num) : "    ";

        parts.push_back(text(old_num) | dim | color(Color::GrayDark));
        parts.push_back(text(" ") | dim);
        parts.push_back(text(new_num) | dim | color(Color::GrayDark));
        parts.push_back(text(" │ ") | dim | color(Color::GrayDark));
    }

    // Prefix
    auto prefix = line_prefix(line.type);
    parts.push_back(text(prefix) | color(line_color(line.type)) | bold);
    parts.push_back(text(" "));

    // Content with optional background
    auto content_el = text(line.content) | color(line_color(line.type));

    if (line.type == DiffLineType::Added) {
        content_el = content_el | bgcolor(Color::RGB(0, 30, 0));
    } else if (line.type == DiffLineType::Removed) {
        content_el = content_el | bgcolor(Color::RGB(30, 0, 0));
    }

    // Trailing whitespace indicator
    if (line.has_trailing_whitespace) {
        parts.push_back(content_el);
        parts.push_back(text("·") | color(Color::Red) | bold);
    } else {
        parts.push_back(content_el);
    }

    auto result = hbox(parts);
    if (selected) {
        result = result | bgcolor(Color::RGB(40, 40, 60));
    }
    return result;
}

/// Render the file list sidebar
[[nodiscard]] inline Element RenderFileList(
    const std::vector<FileDiff>& files, int selected) {

    Elements elements;
    elements.push_back(text(" Files") | bold);
    elements.push_back(separator());

    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        const auto& f = files[i];
        auto [badge, badge_color] = status_badge(f.status);

        auto line = hbox({
            text(" " + badge + " ") | color(badge_color) | bold,
            text(f.new_path.empty() ? f.old_path : f.new_path)
                | color(i == selected ? Color::White : Color::GrayLight),
            filler(),
            text(std::format("+{}", f.additions)) | color(Color::Green) | dim,
            text("/") | dim,
            text(std::format("-{}", f.deletions)) | color(Color::Red) | dim,
            text(" "),
        });
        if (i == selected) {
            line = line | inverted;
        }
        elements.push_back(line);
    }

    return vbox(elements) | border | size(WIDTH, EQUAL, 40);
}

/// Render the diff content for a single file
[[nodiscard]] inline Element RenderFileDiff(
    const FileDiff& file, int selected_line, int scroll_offset,
    bool show_line_nums) {

    if (file.is_binary) {
        return vbox({
            text(" Binary file") | dim | center,
            text(" (cannot display diff)") | dim | center,
        }) | border;
    }

    Elements elements;

    // File header
    auto [badge, badge_color] = status_badge(file.status);
    elements.push_back(hbox({
        text(" " + badge + " ") | color(badge_color) | bold,
        text(file.new_path.empty() ? file.old_path : file.new_path) | bold,
        filler(),
        text(std::format("+{} -{}", file.additions, file.deletions)) | dim,
        text(" "),
    }));
    elements.push_back(separator());

    // Render hunks
    int line_idx = 0;
    for (const auto& hunk : file.hunks) {
        // Hunk header
        auto hunk_header_text = std::format("@@ -{},{} +{},{} @@",
            hunk.old_start, hunk.old_count, hunk.new_start, hunk.new_count);
        if (!hunk.header.empty()) {
            hunk_header_text += " " + hunk.header;
        }
        elements.push_back(text(" " + hunk_header_text)
                           | color(Color::Cyan) | bgcolor(Color::RGB(0, 20, 30)));

        // Hunk lines
        for (const auto& line : hunk.lines) {
            if (line_idx >= scroll_offset) {
                elements.push_back(
                    RenderDiffLine(line, line_idx == selected_line, show_line_nums));
            }
            ++line_idx;
        }
    }

    if (file.is_truncated) {
        elements.push_back(text(" ... diff truncated ...") | dim | center);
    }

    return vbox(elements) | vscroll_indicator | yframe | flex | border;
}

/// Render the complete diff view
[[nodiscard]] inline Element RenderDiffView(const DiffViewOptions& opts) {
    auto file_list = RenderFileList(opts.files, opts.selected_file);

    Element diff_panel;
    if (!opts.files.empty() && opts.selected_file < static_cast<int>(opts.files.size())) {
        diff_panel = RenderFileDiff(
            opts.files[opts.selected_file],
            opts.selected_line,
            opts.scroll_offset,
            opts.show_line_numbers);
    } else {
        diff_panel = text(" No files to display") | dim | center | border | flex;
    }

    // Mode indicator and stats
    std::string mode_str;
    switch (opts.mode) {
        case DiffDisplayMode::Unified: mode_str = "Unified"; break;
        case DiffDisplayMode::Split:   mode_str = "Split"; break;
        case DiffDisplayMode::Inline:  mode_str = "Inline"; break;
    }

    int total_adds = 0, total_dels = 0;
    for (const auto& f : opts.files) {
        total_adds += f.additions;
        total_dels += f.deletions;
    }

    auto status_bar = hbox({
        text(" Mode: ") | dim,
        text(mode_str) | color(Color::Cyan),
        text("  ") | dim,
        text(std::format("{} files", opts.files.size())) | dim,
        text("  ") | dim,
        text(std::format("+{}", total_adds)) | color(Color::Green),
        text("/") | dim,
        text(std::format("-{}", total_dels)) | color(Color::Red),
        filler(),
        text("[j/k] navigate [Tab] switch [m] mode [q] close") | dim,
        text(" "),
    });

    return vbox({
        hbox({file_list, diff_panel}) | flex,
        status_bar,
    });
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a diff view component
[[nodiscard]] inline Component DiffView(DiffViewOptions options) {
    struct State {
        DiffViewOptions opts;
        enum class Focus { FileList, DiffContent } focus = Focus::FileList;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(options);

    return Renderer([state] {
        return RenderDiffView(state->opts);
    }) | CatchEvent([state](Event event) -> bool {
        auto& opts = state->opts;
        int file_count = static_cast<int>(opts.files.size());

        if (event == Event::Character('q') || event == Event::Escape) {
            if (opts.on_close) opts.on_close();
            return true;
        }

        if (event == Event::Tab) {
            state->focus = (state->focus == State::Focus::FileList)
                ? State::Focus::DiffContent : State::Focus::FileList;
            return true;
        }

        if (event == Event::Character('m')) {
            // Cycle display mode
            int m = static_cast<int>(opts.mode);
            opts.mode = static_cast<DiffDisplayMode>((m + 1) % 3);
            return true;
        }

        if (state->focus == State::Focus::FileList) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                opts.selected_file = std::max(0, opts.selected_file - 1);
                opts.selected_line = 0;
                opts.scroll_offset = 0;
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                opts.selected_file = std::min(file_count - 1, opts.selected_file + 1);
                opts.selected_line = 0;
                opts.scroll_offset = 0;
                return true;
            }
            if (event == Event::Return || event == Event::ArrowRight) {
                state->focus = State::Focus::DiffContent;
                if (opts.on_file_select) opts.on_file_select(opts.selected_file);
                return true;
            }
        } else {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                opts.selected_line = std::max(0, opts.selected_line - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                opts.selected_line++;
                return true;
            }
            if (event == Event::ArrowLeft) {
                state->focus = State::Focus::FileList;
                return true;
            }
            if (event == Event::Return) {
                if (opts.on_select) opts.on_select(opts.selected_file, opts.selected_line);
                return true;
            }
        }

        return false;
    });
}

} // namespace cc::ui::components::diff_view
