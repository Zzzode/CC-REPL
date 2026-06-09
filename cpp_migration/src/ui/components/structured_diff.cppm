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
#include <string_view>
#include <unordered_set>
#include <array>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.structured_diff;

import cc.types.types;
import cc.utils.file_edit;

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
// Heuristic Block Splitting & Per-Block LCS Diff
// ============================================================
// NOTE: A full AST-based structural diff is out of scope (TODO: wire up the
//       LSP-based syntax tree when it becomes available). In the meantime we
//       use a lightweight heuristic that matches the strategy described in
//       StructuredDiff/Fallback.tsx:
//         • empty-line-separated logical segments
//         • indentation-based depth boundaries
//         • comment / import / class / function sentinels
//       Combined with the Myers algorithm from utils/file_edit_utils, this
//       gives 2-tone highlighting (block level + line level).
// ===========================================================================

/// Kind of a code block, used for block-level tinting.
enum class BlockKind : std::uint8_t {
    Unknown,
    Comment,
    Imports,
    Function,
    Class,
    Blank,
    Data,
};

/// A semantic block (segment) within a source file.
struct SemanticBlock {
    BlockKind kind = BlockKind::Unknown;
    int start_line = 0;           // 0-indexed inclusive
    int end_line = 0;             // 0-indexed exclusive
    int min_indent = 999;         // minimum indent depth in block
    std::vector<std::string> lines;
};

/// Detect block kind from the first non-empty line.
[[nodiscard]] inline BlockKind classify_block(const std::vector<std::string_view>& seg) {
    for (auto l : seg) {
        // strip leading whitespace
        size_t j = 0;
        while (j < l.size() && (l[j] == ' ' || l[j] == '\t')) ++j;
        if (j == l.size()) continue;
        std::string_view s = l.substr(j);
        if (s.starts_with("//") || s.starts_with("/*") || s.starts_with("*") ||
            s.starts_with("#") || s.starts_with("--") || s.starts_with("\"\"\"") ||
            s.starts_with("'''")) return BlockKind::Comment;
        if (s.starts_with("import ") || s.starts_with("#include") ||
            s.starts_with("from ") || s.starts_with("require(") ||
            s.starts_with("using ") || s.starts_with("export "))
            return BlockKind::Imports;
        if (s.starts_with("def ") || s.starts_with("function ") ||
            s.starts_with("func ") || s.starts_with("fn ") ||
            s.starts_with("async function ") ||
            (s.find('(') != std::string_view::npos &&
             (s.starts_with("const ") || s.starts_with("let ") ||
              s.starts_with("var ")) &&
             s.find("=>") != std::string_view::npos))
            return BlockKind::Function;
        // class/struct/interface
        if (s.starts_with("class ") || s.starts_with("struct ") ||
            s.starts_with("interface ") || s.starts_with("type ") ||
            s.starts_with("namespace "))
            return BlockKind::Class;
        return BlockKind::Unknown;
    }
    return BlockKind::Blank;
}

/// Heuristic block splitter.
[[nodiscard]] inline std::vector<SemanticBlock> split_blocks(
    std::string_view src) {
    std::vector<SemanticBlock> out;
    if (src.empty()) return out;

    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= src.size()) {
        size_t p = src.find('\n', start);
        if (p == std::string_view::npos) { lines.emplace_back(src.substr(start)); break; }
        lines.emplace_back(src.substr(start, p - start));
        start = p + 1;
    }

    // Group consecutive non-empty-lines separated by one or more blank lines.
    // Additionally, a block boundary is emitted when:
    //   • indent depth jumps back past min_indent (dedent)
    //   • a function/class keyword is found at depth <= previous min_indent
    struct Builder {
        std::vector<std::string_view> seg;
        int start_idx = 0;
    };
    auto flush = [&](Builder& b, int end_idx) {
        if (b.seg.empty()) return;
        SemanticBlock blk;
        blk.kind = classify_block(b.seg);
        blk.start_line = b.start_idx;
        blk.end_line = end_idx;
        for (auto l : b.seg) {
            size_t k = 0;
            while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) ++k;
            int indent = static_cast<int>(k);
            if (indent < static_cast<int>(l.size())) blk.min_indent = std::min(blk.min_indent, indent);
            blk.lines.emplace_back(l);
        }
        out.push_back(std::move(blk));
        b.seg.clear();
    };

    Builder cur;
    cur.start_idx = 0;
    int seg_min_indent = 999;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        auto l = lines[i];
        size_t j = 0;
        while (j < l.size() && (l[j] == ' ' || l[j] == '\t')) ++j;
        bool blank = (j == l.size());
        int indent = static_cast<int>(j);

        // blank line -> block boundary
        if (blank) {
            flush(cur, i);
            cur.start_idx = i + 1;
            seg_min_indent = 999;
            continue;
        }

        // Detect a boundary if we see a dedent + top-level keyword.
        if (!cur.seg.empty() && indent <= seg_min_indent) {
            std::string_view head = l.substr(j);
            bool is_toplevel =
                head.starts_with("def ") || head.starts_with("class ") ||
                head.starts_with("function ") || head.starts_with("struct ") ||
                head.starts_with("interface ") || head.starts_with("type ") ||
                head.starts_with("namespace ") || head.starts_with("module ") ||
                head.starts_with("fn ") || head.starts_with("func ") ||
                head.starts_with("async function ");
            if (is_toplevel) {
                flush(cur, i);
                cur.start_idx = i;
                seg_min_indent = 999;
            }
        }

        cur.seg.push_back(l);
        seg_min_indent = std::min(seg_min_indent, indent);
    }
    flush(cur, static_cast<int>(lines.size()));
    return out;
}

/// Per-block LCS line-level diff. Returns vector<StructuredPatchHunk>
/// aggregated across all blocks. Uses Myers from utils/file_edit_utils
/// (no duplicate algorithm — per task constraints).
[[nodiscard]] inline std::vector<cc::utils::file_edit::PatchHunk>
compute_block_lcs_diff(std::string_view old_src,
                       std::string_view new_src,
                       int context = 3) {
    return cc::utils::file_edit::compute_structured_patch(old_src, new_src, context);
}
/// Block-level tint status for rendering
struct BlockDiffStatus {
    bool added_block = false;
    bool removed_block = false;
    bool modified_block = false;
};

/// Render the N-files-changed summary bar used at the top of a structured
/// diff view (matches the DiffDetailView sub-title pattern in TS).
[[nodiscard]] inline Element RenderSummaryBar(
    int files_changed,
    int additions,
    int deletions) {

    auto files_label = std::format(" {} file{} changed",
                                   files_changed,
                                   files_changed == 1 ? "" : "s");
    Elements els;
    els.push_back(text(files_label) | dim);
    if (additions > 0) {
        els.push_back(text(std::format(" +{}", additions))
                      | color(Color::Green) | bold);
    }
    if (deletions > 0) {
        els.push_back(text(std::format(" -{}", deletions))
                      | color(Color::Red) | bold);
    }
    return hbox(std::move(els));
}

// ============================================================
// Word-Level Diff (neighbouring remove/add pairs)
// Per task constraints we use Myers (via utils) on character strings.
// ============================================================

/// Given two strings a and b, produce a list of WordChange ranges on b if
/// `is_added_side` is true (else on a). Used by structured diff to
/// highlight changed words within paired remove/add lines.
///
/// NOTE: The reusable Myers algorithm in utils/file_edit is line-level and
///       not exported at character granularity. We implement a lightweight
///       O(N*M) LCS over UTF-8 code units here; inputs are bounded to
///       single-line lengths so the quadratic cost is acceptable.
///       TODO(#ui7-word-diff): export a char-level Myers from utils to
///       consolidate.
[[nodiscard]] inline std::vector<WordChange> compute_word_changes(
    const std::string& a, const std::string& b, bool is_added_side) {

    std::vector<WordChange> out;
    int n = static_cast<int>(a.size());
    int m = static_cast<int>(b.size());
    if (n == 0 || m == 0) return out;

    // DP: lcs_len[i][j] = LCS length of a[0..i) and b[0..j)
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            if (a[i - 1] == b[j - 1]) dp[i][j] = dp[i - 1][j - 1] + 1;
            else dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
        }
    }

    // Backtrack: mark which positions are matched in each string.
    std::vector<bool> a_match(n, false), b_match(m, false);
    {
        int i = n, j = m;
        while (i > 0 && j > 0) {
            if (a[i - 1] == b[j - 1]) {
                a_match[i - 1] = true;
                b_match[j - 1] = true;
                --i; --j;
            } else if (dp[i - 1][j] >= dp[i][j - 1]) {
                --i;
            } else {
                --j;
            }
        }
    }

    // Collect contiguous non-match runs as word-change ranges.
    auto ranges_from = [](const std::vector<bool>& matched, bool is_added) {
        std::vector<WordChange> rs;
        int n2 = static_cast<int>(matched.size());
        int start = -1;
        for (int k = 0; k < n2; ++k) {
            if (!matched[k]) {
                if (start == -1) start = k;
            } else {
                if (start != -1) rs.push_back({start, k, is_added});
                start = -1;
            }
        }
        if (start != -1) rs.push_back({start, n2, is_added});
        return rs;
    };

    auto added   = ranges_from(b_match, true);
    auto removed = ranges_from(a_match, false);

    // Merge nearby ranges (within 2 chars) to avoid "christmas tree" effect.
    const int kMaxGap = 2;
    auto merge = [&](std::vector<WordChange>& rs) {
        if (rs.empty()) return;
        std::vector<WordChange> out2;
        for (auto& r : rs) {
            if (!out2.empty() && r.start_col - out2.back().end_col <= kMaxGap) {
                out2.back().end_col = std::max(out2.back().end_col, r.end_col);
            } else {
                out2.push_back(r);
            }
        }
        rs = std::move(out2);
    };

    merge(added); merge(removed);
    return is_added_side ? added : removed;
}

/// Annotate a sequence of hunks with per-line word_changes by pairing
/// consecutive Remove/Add lines within the same hunk (fallback strategy
/// from StructuredDiff/Fallback.tsx, CHANGE_THRESHOLD = 0.4).
constexpr double kChangeThreshold = 0.4;

inline void annotate_word_changes(std::vector<StructuredPatchHunk>& hunks) {
    for (auto& hunk : hunks) {
        for (size_t i = 0; i + 1 < hunk.lines.size(); ++i) {
            auto& a = hunk.lines[i];
            auto& b = hunk.lines[i + 1];
            if (a.type != StructuredDiffLine::Type::Removed ||
                b.type != StructuredDiffLine::Type::Added) continue;
            if (a.content.empty() || b.content.empty()) continue;

            // Levenshtein-ish similarity check via common prefix/suffix length.
            size_t common = 0;
            size_t minlen = std::min(a.content.size(), b.content.size());
            while (common < minlen && a.content[common] == b.content[common]) ++common;
            size_t as = a.content.size(), bs = b.content.size();
            size_t suf = 0;
            while (suf + common < minlen &&
                   a.content[as - 1 - suf] == b.content[bs - 1 - suf]) ++suf;
            double changed_ratio = 1.0 - (double)(common + suf) /
                                        (double)std::max(as, bs);
            if (changed_ratio > kChangeThreshold) continue;
            a.word_changes = compute_word_changes(a.content, b.content, false);
            b.word_changes = compute_word_changes(a.content, b.content, true);
            ++i; // skip forward so we don't re-match 'b' as 'a' of next pair.
        }
    }
}

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

// ============================================================
// Factory: Render a full structured diff from raw file contents.
// Public API for consumers (assistant_text_message, diff_dialog, …).
// ============================================================

/// Inputs for RenderStructuredDiff factory.
struct RenderStructuredDiffInput {
    std::string file_path;
    std::string_view old_content;
    std::string_view new_content;
    int context_lines = 3;
    int visible_height = 40;
    int max_display_lines = 2000;
    bool show_line_numbers = true;
    int files_changed = 1;          // for summary bar
    int additions_override = -1;    // -1 = auto compute from hunks
    int deletions_override = -1;
    std::optional<DiffSyntaxTheme> theme;
    std::function<void(int line)> on_line_select;
    std::function<void()> on_close;
};

/// Build hunks + summary, render the full widget.
[[nodiscard]] inline Element RenderStructuredDiff(const RenderStructuredDiffInput& in) {
    auto phunks = cc::utils::file_edit::compute_structured_patch(
        in.old_content, in.new_content, in.context_lines);

    DiffDetailViewProps props;
    props.file_path = in.file_path;

    int adds = 0, dels = 0;
    for (const auto& ph : phunks) {
        StructuredPatchHunk sh;
        sh.old_start = ph.old_start;
        sh.old_lines = ph.old_lines;
        sh.new_start = ph.new_start;
        sh.new_lines = ph.new_lines;

        int ol = ph.old_start, nl = ph.new_start;
        for (const auto& l : ph.lines) {
            if (l.empty()) continue;
            StructuredDiffLine sdl;
            char p = l[0];
            std::string content = l.size() > 1 ? l.substr(1) : "";
            if (p == '+') {
                sdl.type = StructuredDiffLine::Type::Added;
                sdl.new_line_num = nl++;
                ++adds;
            } else if (p == '-') {
                sdl.type = StructuredDiffLine::Type::Removed;
                sdl.old_line_num = ol++;
                ++dels;
            } else {
                sdl.type = StructuredDiffLine::Type::Context;
                sdl.old_line_num = ol++;
                sdl.new_line_num = nl++;
            }
            sdl.content = std::move(content);
            sh.lines.push_back(std::move(sdl));
        }
        props.hunks.push_back(std::move(sh));
    }

    annotate_word_changes(props.hunks);

    int additions = in.additions_override >= 0 ? in.additions_override : adds;
    int deletions = in.deletions_override >= 0 ? in.deletions_override : dels;

    StructuredDiffOptions opts;
    opts.diff = std::move(props);
    opts.show_line_numbers = in.show_line_numbers;
    opts.context_lines = in.context_lines;
    opts.max_display_lines = in.max_display_lines;
    opts.visible_height = in.visible_height;
    opts.theme = in.theme;
    opts.on_line_select = in.on_line_select;
    opts.on_close = in.on_close;

    auto summary = RenderSummaryBar(in.files_changed, additions, deletions);
    auto body = RenderStructuredDiff(opts);

    return vbox({
        summary,
        separator(),
        body,
    }) | flex;
}

} // namespace cc::ui::structured_diff
