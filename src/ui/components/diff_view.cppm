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
import cc.utils.file_edit;

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
// Parsing
// ============================================================

/// Limits for automatic folding / truncation
constexpr int kMaxHunksBeforeTruncate = 100;
constexpr int kMaxLinesBeforeTruncate = 5000;

/// Split a string_view into lines (no heap copies for line views).
[[nodiscard]] inline std::vector<std::string_view>
split_lines_view(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find('\n', start);
        if (p == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

/// Parse a hunk header: `@@ -old_start,old_count +new_start,new_count @@ [context]`
[[nodiscard]] inline bool parse_hunk_header(std::string_view line,
                                            int& old_start, int& old_count,
                                            int& new_start, int& new_count,
                                            std::string& context) {
    // Minimal parser — validates prefix and format.
    if (line.size() < 4 || line.substr(0, 2) != "@@") return false;
    size_t i = 2;
    auto skip_ws = [&] { while (i < line.size() && line[i] == ' ') ++i; };
    auto parse_int = [&](int& out) -> bool {
        if (i >= line.size()) return false;
        bool neg = false;
        if (line[i] == '-') { neg = true; ++i; }
        else if (line[i] == '+') { ++i; }
        if (i >= line.size() || !isdigit(static_cast<unsigned char>(line[i]))) return false;
        out = 0;
        while (i < line.size() && isdigit(static_cast<unsigned char>(line[i]))) {
            out = out * 10 + (line[i] - '0');
            ++i;
        }
        if (neg) out = -out;
        return true;
    };

    skip_ws();
    int os = 0, oc = 1, ns = 0, nc = 1;
    if (!parse_int(os)) return false;
    if (i < line.size() && line[i] == ',') { ++i; if (!parse_int(oc)) return false; }
    skip_ws();
    if (!parse_int(ns)) return false;
    if (i < line.size() && line[i] == ',') { ++i; if (!parse_int(nc)) return false; }
    skip_ws();
    if (i + 1 >= line.size() || line[i] != '@' || line[i + 1] != '@') return false;
    i += 2;
    skip_ws();
    old_start = os; old_count = oc;
    new_start = ns; new_count = nc;
    context = std::string(line.substr(i));
    return true;
}

/// Parse a unified-diff string into a list of FileDiff structs.
[[nodiscard]] inline std::vector<FileDiff> ParseUnifiedDiff(std::string_view diff) {
    std::vector<FileDiff> result;
    auto lines = split_lines_view(diff);

    FileDiff* cur = nullptr;
    DiffHunk* cur_hunk = nullptr;
    int old_ln = 0, new_ln = 0;
    int total_lines = 0;
    bool truncated = false;

    auto start_new_file = [&] {
        result.emplace_back();
        cur = &result.back();
        cur->status = "modified";
        cur_hunk = nullptr;
        old_ln = new_ln = 0;
        total_lines = 0;
        truncated = false;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view line = lines[i];

        if (line.substr(0, 4) == "diff") {
            start_new_file();
            continue;
        }
        if (!cur) start_new_file();

        if (line.substr(0, 3) == "---") {
            cur->old_path = std::string(line.substr(4));
            if (cur->old_path.starts_with("a/")) cur->old_path = cur->old_path.substr(2);
            // pure insert
            if (cur->old_path == "/dev/null") cur->status = "added";
            continue;
        }
        if (line.substr(0, 3) == "+++") {
            cur->new_path = std::string(line.substr(4));
            if (cur->new_path.starts_with("b/")) cur->new_path = cur->new_path.substr(2);
            if (cur->new_path == "/dev/null") cur->status = "deleted";
            continue;
        }
        if (line.substr(0, 7) == "Binary ") {
            cur->is_binary = true;
            continue;
        }
        if (line.substr(0, 6) == "index " ||
            line.substr(0, 7) == "old mod" ||
            line.substr(0, 7) == "new mod" ||
            line.substr(0, 10) == "rename fro" ||
            line.substr(0, 8) == "rename t" ||
            line.substr(0, 7) == "copy fr" ||
            line.substr(0, 6) == "copy t" ||
            line.substr(0, 4) == "GIT ") {
            if (line.substr(0, 10) == "rename fro") cur->status = "renamed";
            continue;
        }

        // Hunk header
        if (line.substr(0, 2) == "@@") {
            int os, oc, ns, nc;
            std::string ctx;
            if (parse_hunk_header(line, os, oc, ns, nc, ctx)) {
                if (static_cast<int>(cur->hunks.size()) >= kMaxHunksBeforeTruncate) {
                    truncated = true;
                    cur->is_truncated = true;
                    break;
                }
                cur->hunks.emplace_back();
                cur_hunk = &cur->hunks.back();
                cur_hunk->old_start = os;
                cur_hunk->old_count = oc;
                cur_hunk->new_start = ns;
                cur_hunk->new_count = nc;
                cur_hunk->header = ctx;
                old_ln = os; new_ln = ns;

                DiffLine header;
                header.type = DiffLineType::Header;
                header.content = std::string(line);
                cur_hunk->lines.push_back(std::move(header));
            }
            continue;
        }

        if (!cur_hunk) continue;
        if (total_lines >= kMaxLinesBeforeTruncate) {
            truncated = true;
            cur->is_truncated = true;
            break;
        }

        DiffLine dl;
        if (!line.empty() && line[0] == '+') {
            dl.type = DiffLineType::Added;
            dl.content = std::string(line.substr(1));
            dl.new_line_num = new_ln++;
            cur->additions++;
        } else if (!line.empty() && line[0] == '-') {
            dl.type = DiffLineType::Removed;
            dl.content = std::string(line.substr(1));
            dl.old_line_num = old_ln++;
            cur->deletions++;
        } else if (!line.empty() && line[0] == ' ') {
            dl.type = DiffLineType::Context;
            dl.content = std::string(line.substr(1));
            dl.old_line_num = old_ln++;
            dl.new_line_num = new_ln++;
        } else if (line == "\\ No newline at end of file") {
            dl.type = DiffLineType::Empty;
            dl.content = std::string(line);
        } else {
            dl.type = DiffLineType::Context;
            dl.content = std::string(line);
        }
        // trailing whitespace detection
        if (!dl.content.empty()) {
            char b = dl.content.back();
            if (b == ' ' || b == '\t') dl.has_trailing_whitespace = true;
        }
        cur_hunk->lines.push_back(std::move(dl));
        ++total_lines;
    }

    if (truncated) {
        for (auto& f : result) {
            if (!f.is_truncated && &f == cur) f.is_truncated = true;
        }
    }

    // auto-detect additions / renames from path
    for (auto& f : result) {
        if (f.status == "modified") {
            if (!f.old_path.empty() && f.new_path.empty()) f.new_path = f.old_path;
            if (f.old_path.empty() && !f.new_path.empty()) f.old_path = f.new_path;
            if (f.deletions == 0 && f.additions > 0 && f.old_path == "/dev/null") f.status = "added";
        }
    }

    return result;
}

/// Build a FileDiff from two file contents via Myers + structured patch.
[[nodiscard]] inline FileDiff BuildFileDiffFromContents(
    std::string_view old_path,
    std::string_view new_path,
    std::string_view old_content,
    std::string_view new_content,
    int context = 3) {

    FileDiff f;
    f.old_path = std::string(old_path);
    f.new_path = std::string(new_path);

    auto hunks = cc::utils::file_edit::compute_structured_patch(
        old_content, new_content, context);
    int total_lines = 0;
    for (const auto& ph : hunks) {
        if (static_cast<int>(f.hunks.size()) >= kMaxHunksBeforeTruncate) {
            f.is_truncated = true; break;
        }
        DiffHunk dh;
        dh.old_start = ph.old_start;
        dh.old_count = ph.old_lines;
        dh.new_start = ph.new_start;
        dh.new_count = ph.new_lines;

        int old_ln = ph.old_start, new_ln = ph.new_start;
        DiffLine header;
        header.type = DiffLineType::Header;
        header.content = std::format("@@ -{},{} +{},{} @@",
                                     ph.old_start, ph.old_lines,
                                     ph.new_start, ph.new_lines);
        dh.lines.push_back(std::move(header));

        for (const auto& l : ph.lines) {
            if (total_lines >= kMaxLinesBeforeTruncate) {
                f.is_truncated = true; break;
            }
            if (l.empty()) { ++total_lines; continue; }
            DiffLine dl;
            char prefix = l[0];
            std::string content = l.size() > 1 ? l.substr(1) : "";
            if (prefix == '+') {
                dl.type = DiffLineType::Added;
                dl.new_line_num = new_ln++;
                f.additions++;
            } else if (prefix == '-') {
                dl.type = DiffLineType::Removed;
                dl.old_line_num = old_ln++;
                f.deletions++;
            } else {
                dl.type = DiffLineType::Context;
                dl.old_line_num = old_ln++;
                dl.new_line_num = new_ln++;
            }
            dl.content = std::move(content);
            if (!dl.content.empty()) {
                char b = dl.content.back();
                if (b == ' ' || b == '\t') dl.has_trailing_whitespace = true;
            }
            dh.lines.push_back(std::move(dl));
            ++total_lines;
        }
        f.hunks.push_back(std::move(dh));
        if (f.is_truncated) break;
    }

    if (f.additions == 0 && f.deletions == 0) f.status = "modified";
    else if (f.old_path.empty() || f.deletions == 0) f.status = "added";
    else if (f.new_path.empty() || f.additions == 0) f.status = "deleted";
    else f.status = "modified";

    return f;
}

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
    const DiffLine& line, bool selected, bool show_line_nums,
    int gutter_width = 4) {

    Elements parts;

    // Line numbers — width adapts to the maximum line number.
    if (show_line_nums) {
        std::string old_num = line.old_line_num
            ? std::format("{:>{}}", *line.old_line_num, gutter_width)
            : std::string(gutter_width, ' ');
        std::string new_num = line.new_line_num
            ? std::format("{:>{}}", *line.new_line_num, gutter_width)
            : std::string(gutter_width, ' ');

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

    // Work out gutter width from max line number.
    int max_line = 1;
    for (const auto& hunk : file.hunks) {
        max_line = std::max(max_line, hunk.old_start + hunk.old_count - 1);
        max_line = std::max(max_line, hunk.new_start + hunk.new_count - 1);
    }
    int gutter_width = std::max(4, static_cast<int>(
        std::format("{}", max_line).size()));

    // Render hunks
    int line_idx = 0;
    bool overflow_warned = false;
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
            if (line_idx >= kMaxLinesBeforeTruncate && !overflow_warned) {
                elements.push_back(
                    text(" Diff too large, only first 5000 lines shown")
                    | color(Color::Yellow) | bold | center);
                overflow_warned = true;
                break;
            }
            if (line_idx >= scroll_offset) {
                elements.push_back(
                    RenderDiffLine(line, line_idx == selected_line, show_line_nums,
                                    gutter_width));
            }
            ++line_idx;
        }
    }

    if (file.is_truncated && !overflow_warned) {
        elements.push_back(
            text(" ... diff truncated (hunks exceeded limit ...") | dim | center);
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
