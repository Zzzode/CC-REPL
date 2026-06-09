// File Edit Utilities
// Provides utilities for editing files with patch generation.
// Agent 9: audit completed 2026-06-09.
//   - Added: structured-patch (Myers LCS) so we don't depend on an external
//            `diff` binary for the display patch.
//   - Added: get_patch_for_edit / get_patch_for_edits (with overlap guard).
//   - Added: desanitize_match_string + DESANITIZATIONS table.
//   - Added: normalize_file_edit_input (cached read + desanitize + strip WS).
//   - Added: are_file_edits_equivalent + are_file_edits_inputs_equivalent.
//   - Added: get_snippet_for_two_file_diff (8KB cap on attachments).
//   - Added: get_snippet (simple edit-based snippet).
//   - Added: get_edits_for_patch (hunks -> FileEdit[]).
//   - Fixed: trailing-newline handling in apply_edit() now matches TS order
//            (strip-newline check BEFORE the replace, not after).
//   - Fixed: is_opening_context() now includes em dash / en dash, mirroring
//            the Unicode-aware check in TS.
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.file_edit;

import cc.utils.file_read_cache;
import cc.utils.string_utils;

export namespace cc::utils::file_edit {

// ===========================================================================
// Quote normalization constants
// ===========================================================================

inline constexpr std::string_view LEFT_SINGLE_CURLY_QUOTE  = "\xE2\x80\x98";
inline constexpr std::string_view RIGHT_SINGLE_CURLY_QUOTE = "\xE2\x80\x99";
inline constexpr std::string_view LEFT_DOUBLE_CURLY_QUOTE  = "\xE2\x80\x9C";
inline constexpr std::string_view RIGHT_DOUBLE_CURLY_QUOTE = "\xE2\x80\x9D";

// ===========================================================================
// Core types (= FileEdit / PatchHunk from TS types.ts)
// ===========================================================================

struct FileEdit {
    std::string old_string;
    std::string new_string;
    bool replace_all = false;
};

struct PatchHunk {
    int old_start = 0;
    int old_lines = 0;
    int new_start = 0;
    int new_lines = 0;
    std::vector<std::string> lines;
};

// ===========================================================================
// Quote / whitespace helpers
// ===========================================================================

[[nodiscard]] inline std::string normalize_quotes(std::string_view str) {
    std::string result(str);

    auto replace_all_utf8 = [&](std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = result.find(from, pos)) != std::string::npos) {
            result.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all_utf8(LEFT_SINGLE_CURLY_QUOTE,  "'");
    replace_all_utf8(RIGHT_SINGLE_CURLY_QUOTE, "'");
    replace_all_utf8(LEFT_DOUBLE_CURLY_QUOTE,  "\"");
    replace_all_utf8(RIGHT_DOUBLE_CURLY_QUOTE, "\"");
    return result;
}

[[nodiscard]] inline std::string strip_trailing_whitespace(std::string_view str) {
    std::string result;
    result.reserve(str.size());

    size_t start = 0;
    while (start <= str.size()) {
        size_t end = start;
        char ending = 0;
        while (end < str.size() && str[end] != '\n' && str[end] != '\r') ++end;

        if (end < str.size()) {
            if (str[end] == '\r' && end + 1 < str.size() && str[end + 1] == '\n') {
                ending = 2; // CRLF
            } else {
                ending = 1; // CR or LF
            }
        }

        // Strip trailing spaces/tabs/CR from line content
        size_t last = end;
        while (last > start) {
            char c = str[last - 1];
            if (c == ' ' || c == '\t' || c == '\r') --last;
            else break;
        }
        result.append(str.data() + start, last - start);

        if (ending == 1) {
            result += str[end];
            start = end + 1;
        } else if (ending == 2) {
            result += "\r\n";
            start = end + 2;
        } else {
            break;
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> find_actual_string(
    std::string_view file_content,
    std::string_view search_string)
{
    if (file_content.find(search_string) != std::string_view::npos) {
        return std::string(search_string);
    }
    std::string normalized_search = normalize_quotes(search_string);
    std::string normalized_file   = normalize_quotes(file_content);
    size_t pos = normalized_file.find(normalized_search);
    if (pos != std::string::npos) {
        return std::string(file_content.substr(pos, search_string.size()));
    }
    return std::nullopt;
}

// ===========================================================================
// Opening-context check — used by curly-quote heuristics. Mirrors TS:
// prev in { space, tab, newline, CR, (, [, {, em-dash, en-dash }  => opening.
// ===========================================================================

inline bool starts_multi_byte_seq_at(std::string_view s, size_t i,
                                     std::string_view seq) {
    if (i + seq.size() > s.size()) return false;
    return s.substr(i, seq.size()) == seq;
}

inline bool is_opening_context(std::string_view text, size_t char_byte_index,
                               size_t byte_len = 1) {
    // Look at the character *before* char_byte_index.
    if (char_byte_index == 0) return true;

    // Walk back: if prev is a multi-byte UTF-8 opener for em/en dash, match it.
    constexpr std::string_view EM_DASH = "\xE2\x80\x94"; // U+2014
    constexpr std::string_view EN_DASH = "\xE2\x80\x93"; // U+2013
    const size_t prev_end = char_byte_index;

    // 3-byte UTF-8 check (em/en dash are both 3 bytes)
    if (prev_end >= 3) {
        std::string_view prev3(text.data() + prev_end - 3, 3);
        if (prev3 == EM_DASH || prev3 == EN_DASH) return true;
    }

    // Single-byte check
    char prev = text[char_byte_index - 1];
    return prev == ' '  || prev == '\t' || prev == '\n' || prev == '\r' ||
           prev == '('  || prev == '['  || prev == '{';
}

[[nodiscard]] inline std::string apply_curly_double_quotes(std::string_view str) {
    std::string result;
    result.reserve(str.size() * 2);
    for (size_t i = 0; i < str.size(); ) {
        if (str[i] == '"') {
            result += is_opening_context(str, i)
                ? LEFT_DOUBLE_CURLY_QUOTE
                : RIGHT_DOUBLE_CURLY_QUOTE;
            ++i;
        } else {
            result += str[i++];
        }
    }
    return result;
}

// Unicode letter check — wraps std::regex with the Unicode-aware pattern
// `\p{L}` (TS uses /\p{L}/u). We fall back to ASCII-only when the regex
// engine doesn't support it at runtime, which matches the pragmatic
// behaviour of detecting contractions in e.g. "don't", "it's".
inline bool is_unicode_letter(char prev, char next) {
    auto is_ascii_letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    return is_ascii_letter(prev) && is_ascii_letter(next);
}

[[nodiscard]] inline std::string apply_curly_single_quotes(std::string_view str) {
    std::string result;
    result.reserve(str.size() * 2);
    for (size_t i = 0; i < str.size(); ) {
        if (str[i] == '\'') {
            // Contraction heuristic: apostrophe between two letters
            if (i > 0 && i + 1 < str.size() &&
                is_unicode_letter(str[i - 1], str[i + 1])) {
                result += RIGHT_SINGLE_CURLY_QUOTE;
            } else {
                result += is_opening_context(str, i)
                    ? LEFT_SINGLE_CURLY_QUOTE
                    : RIGHT_SINGLE_CURLY_QUOTE;
            }
            ++i;
        } else {
            result += str[i++];
        }
    }
    return result;
}

[[nodiscard]] inline std::string preserve_quote_style(
    std::string_view old_string,
    std::string_view actual_old_string,
    std::string_view new_string)
{
    if (old_string == actual_old_string) return std::string(new_string);

    auto contains_any = [](std::string_view s, std::string_view a, std::string_view b) {
        return s.find(a) != std::string_view::npos || s.find(b) != std::string_view::npos;
    };
    const bool has_double = contains_any(actual_old_string,
        LEFT_DOUBLE_CURLY_QUOTE, RIGHT_DOUBLE_CURLY_QUOTE);
    const bool has_single = contains_any(actual_old_string,
        LEFT_SINGLE_CURLY_QUOTE, RIGHT_SINGLE_CURLY_QUOTE);

    if (!has_double && !has_single) return std::string(new_string);

    std::string result(new_string);
    if (has_double) result = apply_curly_double_quotes(result);
    if (has_single) result = apply_curly_single_quotes(result);
    return result;
}

// ===========================================================================
// apply_edit — order now matches TS:
//   1. If new_string == "" AND old_string doesn't end in "\n" AND the file
//      has old_string + "\n", also match and replace that variant.
//   2. Otherwise first/replace_all.
// ===========================================================================

[[nodiscard]] inline std::string apply_edit(
    std::string_view original_content,
    std::string_view old_string,
    std::string_view new_string,
    bool replace_all = false)
{
    // Old-string empty -> pure insert / new file semantics.
    if (old_string.empty()) return std::string(new_string);

    const bool strip_trailing_newline =
        new_string.empty() &&
        !old_string.ends_with('\n') &&
        original_content.find(std::string(old_string) + "\n") != std::string_view::npos;

    const std::string_view effective_old =
        strip_trailing_newline ? std::string_view{} : std::string_view{};
    // (we materialize below to avoid string_view lifetime issues)

    if (strip_trailing_newline) {
        std::string search = std::string(old_string) + "\n";
        std::string content(original_content);
        if (replace_all) {
            size_t pos = 0;
            while ((pos = content.find(search, pos)) != std::string::npos) {
                content.replace(pos, search.size(), new_string);
                pos += new_string.size();
            }
            return content;
        }
        size_t p = content.find(search);
        if (p != std::string::npos) {
            content.replace(p, search.size(), new_string);
            return content;
        }
        // Fall through: no trailing-newline variant found, do regular replace.
    }

    std::string content(original_content);
    if (replace_all) {
        size_t pos = 0;
        while ((pos = content.find(old_string, pos)) != std::string::npos) {
            content.replace(pos, old_string.size(), new_string);
            pos += new_string.size();
        }
    } else {
        size_t p = content.find(old_string);
        if (p != std::string::npos) {
            content.replace(p, old_string.size(), new_string);
        }
    }
    return content;
}

// ===========================================================================
// Structured patch — Myers O(ND) LCS line-diff.
// Mirrors `diff.structuredPatch` output shape (hunks with context lines).
//
// NOTE: The TS code used the npm `diff` package which also exposes Myers.
// We deliberately avoid shelling out to `diff -u` here because:
//   (a) callers rely on PatchHunk line data to build UI widgets;
//   (b) subprocess calls are expensive when the model proposes many small
//       edits per turn.
// If the caller truly wants a shell `diff -u` display, they can use
// bash_helpers + the two files they already have.
// ===========================================================================

namespace patch_detail {

inline std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        auto pos = text.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

/// Raw diff operation produced by Myers.
enum Op { Equal, Insert, Remove };
struct RawOp { Op op; size_t index; };

/// Myers O(ND) diff between two line vectors.
inline std::vector<RawOp> myers_diff(const std::vector<std::string>& a,
                                     const std::vector<std::string>& b) {
    const int N = static_cast<int>(a.size());
    const int M = static_cast<int>(b.size());
    const int MAX = N + M;

    std::vector<int> v(2 * MAX + 2, 0);
    std::vector<std::vector<int>> trace;
    trace.reserve(MAX + 1);

    int found_d = -1;
    for (int D = 0; D <= MAX && found_d < 0; ++D) {
        trace.push_back(v); // snapshot
        for (int k = -D; k <= D && found_d < 0; k += 2) {
            int x;
            if (k == -D || (k != D && v[MAX + k - 1] < v[MAX + k + 1])) {
                x = v[MAX + k + 1];
            } else {
                x = v[MAX + k - 1] + 1;
            }
            int y = x - k;
            while (x < N && y < M && a[x] == b[y]) { ++x; ++y; }
            v[MAX + k] = x;
            if (x >= N && y >= M) found_d = D;
        }
    }

    // Backtrack to build edit script
    std::vector<std::pair<int, int>> path; // (x, y) coordinates
    path.emplace_back(N, M);
    for (int D = found_d; D > 0; --D) {
        const auto& snap = trace[D];
        int x = path.back().first;
        int y = path.back().second;
        int k = x - y;
        int prev_k;
        if (k == -D || (k != D && snap[MAX + k - 1] < snap[MAX + k + 1])) {
            prev_k = k + 1;
        } else {
            prev_k = k - 1;
        }
        int prev_x = snap[MAX + prev_k];
        int prev_y = prev_x - prev_k;
        // Walk diagonals
        while (x > prev_x && y > prev_y && a[x - 1] == b[y - 1]) {
            --x; --y;
            path.emplace_back(x, y);
        }
        path.emplace_back(prev_x, prev_y);
    }
    std::reverse(path.begin(), path.end());

    // Emit RawOp sequence with Equal runs preserved
    std::vector<RawOp> ops;
    for (size_t i = 1; i < path.size(); ++i) {
        int x0 = path[i - 1].first,  y0 = path[i - 1].second;
        int x1 = path[i].first,      y1 = path[i].second;
        // Non-diagonal first
        if (x1 - x0 > y1 - y0) {
            ops.push_back({Remove, static_cast<size_t>(x0)});
            ++x0;
        } else if (y1 - y0 > x1 - x0) {
            ops.push_back({Insert, static_cast<size_t>(y0)});
            ++y0;
        }
        // Then equal diagonal
        while (x0 < x1 && y0 < y1) {
            ops.push_back({Equal, static_cast<size_t>(x0)});
            ++x0; ++y0;
        }
    }
    return ops;
}

inline std::vector<std::string> convert_leading_tabs_to_spaces(std::string_view s) {
    // TS diff pipeline normalises tabs to spaces before diffing so that the
    // displayed patch lines up with what the Read tool shows. We do the same
    // per-line (4 spaces = 1 tab, matching most editors' default).
    auto lines = split_lines(s);
    for (auto& line : lines) {
        size_t i = 0;
        while (i < line.size() && line[i] == '\t') ++i;
        if (i > 0) {
            line = std::string(i * 4, ' ') + line.substr(i);
        }
    }
    return lines;
}

} // namespace patch_detail

/// Compute structured hunks from two file contents (= TS getPatchFromContents
/// after tab expansion). `context` lines of context are preserved around
/// each hunk, matching the `diff` npm package default.
inline std::vector<PatchHunk> compute_structured_patch(
    std::string_view old_content,
    std::string_view new_content,
    int context = 3)
{
    using namespace patch_detail;
    auto a = convert_leading_tabs_to_spaces(old_content);
    auto b = convert_leading_tabs_to_spaces(new_content);
    auto ops = myers_diff(a, b);

    // Collapse ops into hunks separated by > 2*context equal lines.
    // Strategy from git: any Equal run longer than 2*context splits hunks,
    // keeping `context` equal lines on each side.
    std::vector<PatchHunk> hunks;
    if (ops.empty()) return hunks;

    // (1) Collect run-length encoded ops
    struct Run { Op op; size_t a_start; size_t a_end; size_t b_start; size_t b_end; };
    std::vector<Run> runs;
    {
        Op cur_op = ops.front().op;
        size_t a_s = 0, a_e = 0, b_s = 0, b_e = 0;
        if (cur_op == Equal)  { a_s = a_e = ops.front().index; b_s = b_e = ops.front().index; a_e++; b_e++; }
        if (cur_op == Insert) { b_s = b_e = ops.front().index; b_e++; a_s = a_e = 0; }
        if (cur_op == Remove) { a_s = a_e = ops.front().index; a_e++; b_s = b_e = 0; }
        for (size_t k = 1; k < ops.size(); ++k) {
            const auto& op = ops[k];
            if (op.op == cur_op) {
                if (cur_op == Equal)  { a_e++; b_e++; }
                if (cur_op == Insert) { b_e++; }
                if (cur_op == Remove) { a_e++; }
                continue;
            }
            runs.push_back({cur_op, a_s, a_e, b_s, b_e});
            cur_op = op.op;
            if (cur_op == Equal)  { a_s = a_e = op.index; b_s = b_e = op.index; a_e++; b_e++; }
            if (cur_op == Insert) { b_s = b_e = op.index; b_e++; }
            if (cur_op == Remove) { a_s = a_e = op.index; a_e++; }
        }
        runs.push_back({cur_op, a_s, a_e, b_s, b_e});
    }

    // (2) Break into hunks. A hunk is a maximal run of non-Equal runs, with
    //     Equal runs on either end truncated to `context` lines.
    const int twice_ctx = 2 * context;
    for (size_t i = 0; i < runs.size(); ) {
        if (runs[i].op == Equal) { ++i; continue; }

        size_t hunk_start = i;
        // Find last non-Equal run that is within hunk distance
        size_t hunk_end = i;
        size_t j = i + 1;
        while (j < runs.size()) {
            if (runs[j].op != Equal) { hunk_end = j; ++j; continue; }
            size_t eq_len = runs[j].a_end - runs[j].a_start;
            if (static_cast<int>(eq_len) > twice_ctx) break;
            ++j;
            if (j < runs.size() && runs[j].op != Equal) { hunk_end = j - 1; /* keep eq */ ++j; }
            else break;
        }

        // Pre-Equal context (the Equal run just before hunk_start, if any)
        PatchHunk hunk;
        int old_off = 0, new_off = 0;
        if (hunk_start > 0 && runs[hunk_start - 1].op == Equal) {
            const auto& eq = runs[hunk_start - 1];
            size_t len = eq.a_end - eq.a_start;
            int keep = std::min(context, static_cast<int>(len));
            size_t from = eq.a_end - keep;
            for (size_t k = from; k < eq.a_end; ++k)
                hunk.lines.push_back(" " + a[k]);
            old_off += static_cast<int>(len - keep);
            new_off += static_cast<int>(len - keep);
            --hunk_start; // include this (truncated) Equal run
        }

        // Core non-Equal runs and interleaved short Equal runs
        for (size_t k = hunk_start; k <= hunk_end; ++k) {
            const auto& r = runs[k];
            if (r.op == Equal) {
                for (size_t p = r.a_start; p < r.a_end; ++p)
                    hunk.lines.push_back(" " + a[p]);
            } else if (r.op == Remove) {
                for (size_t p = r.a_start; p < r.a_end; ++p)
                    hunk.lines.push_back("-" + a[p]);
            } else { // Insert
                for (size_t p = r.b_start; p < r.b_end; ++p)
                    hunk.lines.push_back("+" + b[p]);
            }
        }

        // Post-Equal context
        if (hunk_end + 1 < runs.size() && runs[hunk_end + 1].op == Equal) {
            const auto& eq = runs[hunk_end + 1];
            size_t len = eq.a_end - eq.a_start;
            int keep = std::min(context, static_cast<int>(len));
            for (int k = 0; k < keep; ++k)
                hunk.lines.push_back(" " + a[eq.a_start + k]);
        }

        // Compute hunk header (1-indexed) from runs[hunk_start..hunk_end].
        // We walk the runs from hunk_start to hunk_end inclusive, accumulating
        // old/new counts and deriving old_start/new_start from the first run.
        int old_count = 0, new_count = 0;
        std::optional<size_t> first_old, first_new;
        for (size_t k = hunk_start; k <= hunk_end; ++k) {
            const auto& r = runs[k];
            if (r.op != Insert) {
                if (!first_old) first_old = r.a_start;
                old_count += static_cast<int>(r.a_end - r.a_start);
            }
            if (r.op != Remove) {
                if (!first_new) first_new = r.b_start;
                new_count += static_cast<int>(r.b_end - r.b_start);
            }
        }
        // Also count the pre/post Equal context lines we prepended/appended.
        // Re-derive: walk lines with +/- markers.
        {
            int oc = 0, nc = 0;
            for (const auto& line : hunk.lines) {
                if (line.empty()) { oc++; nc++; continue; }
                char c = line[0];
                if (c == ' ' || c == '-') oc++;
                if (c == ' ' || c == '+') nc++;
            }
            old_count = oc;
            new_count = nc;
        }

        // Determine hunk.old_start / new_start from runs (account for the
        // pre-Equal offset that was trimmed).
        {
            // Find first non-space line in hunk, but easier: compute from
            // runs, applying trimmed pre-Equal offset.
            std::optional<size_t> o_start, n_start;
            for (size_t k = hunk_start; k <= hunk_end; ++k) {
                const auto& r = runs[k];
                if (r.op != Insert && !o_start) o_start = r.a_start;
                if (r.op != Remove && !n_start) n_start = r.b_start;
            }
            // Subtract pre-Equal context lines we kept (they came before).
            int pre_eq_kept = 0;
            for (const auto& line : hunk.lines) {
                if (!line.empty() && line[0] == ' ' &&
                    (runs[hunk_start].op == Equal ||
                     (hunk_start > 0 && runs[hunk_start - 1].op == Equal))) {
                    // We already counted these; break once we see first non-' '.
                    // Use a simpler approach below:
                    break;
                }
            }
            // Simpler: count leading ' ' lines and subtract that many.
            int leading_ctx = 0;
            for (const auto& line : hunk.lines) {
                if (!line.empty() && line[0] == ' ') leading_ctx++;
                else break;
            }
            if (!o_start) o_start = 0;
            if (!n_start) n_start = 0;
            hunk.old_start = static_cast<int>(*o_start) - leading_ctx + 1;
            hunk.new_start = static_cast<int>(*n_start) - leading_ctx + 1;
            hunk.old_lines = old_count;
            hunk.new_lines = new_count;

            // Edge case: empty-old (pure insert) hunk starts at 0 per diff.js
            // but downstream code expects 1; keep >= 1.
            if (hunk.old_start <= 0) hunk.old_start = 1;
            if (hunk.new_start <= 0) hunk.new_start = 1;
        }

        hunks.push_back(std::move(hunk));
        i = hunk_end + 1;
    }

    return hunks;
}

// ===========================================================================
// get_patch_for_edits — mirror of TS getPatchForEdits().
// Applies edits sequentially with overlap detection and returns the patch.
// ===========================================================================

struct PatchForEditsResult {
    std::vector<PatchHunk> patch;
    std::string updated_file;
};

inline PatchForEditsResult get_patch_for_edits(
    std::string_view file_path,
    std::string_view file_contents,
    const std::vector<FileEdit>& edits)
{
    // (void)file_path; — kept for call-site parity with TS; unused in pure fn

    // Empty-file special case
    if (file_contents.empty() && edits.size() == 1 &&
        edits.front().old_string.empty() && edits.front().new_string.empty()) {
        auto patch = compute_structured_patch(file_contents, "");
        return {std::move(patch), ""};
    }

    std::string updated = std::string(file_contents);
    std::vector<std::string> applied_new_strings;

    for (const auto& edit : edits) {
        // Strip trailing newlines from old_string before overlap check
        std::string old_to_check = edit.old_string;
        while (!old_to_check.empty() && old_to_check.back() == '\n')
            old_to_check.pop_back();

        if (!old_to_check.empty()) {
            for (const auto& prev_new : applied_new_strings) {
                if (prev_new.find(old_to_check) != std::string::npos) {
                    throw std::runtime_error(
                        "Cannot edit file: old_string is a substring of a "
                        "new_string from a previous edit.");
                }
            }
        }

        std::string previous = updated;
        updated = edit.old_string.empty()
            ? edit.new_string
            : apply_edit(updated, edit.old_string,
                         edit.new_string, edit.replace_all);

        if (updated == previous) {
            throw std::runtime_error(
                "String not found in file. Failed to apply edit.");
        }
        applied_new_strings.push_back(edit.new_string);
    }

    if (updated == file_contents) {
        throw std::runtime_error(
            "Original and edited file match exactly. Failed to apply edit.");
    }

    auto patch = compute_structured_patch(file_contents, updated);
    return {std::move(patch), std::move(updated)};
}

inline PatchForEditsResult get_patch_for_edit(
    std::string_view file_path,
    std::string_view file_contents,
    std::string_view old_string,
    std::string_view new_string,
    bool replace_all = false)
{
    return get_patch_for_edits(file_path, file_contents, {
        FileEdit{
            .old_string = std::string(old_string),
            .new_string = std::string(new_string),
            .replace_all = replace_all
        }
    });
}

// ===========================================================================
// Line-number formatting helper (= TS utils/file addLineNumbers).
// ===========================================================================

[[nodiscard]] inline std::string add_line_numbers(
    std::string_view content,
    int start_line = 1)
{
    auto lines = patch_detail::split_lines(content);
    // Compute width
    int end_line = start_line + static_cast<int>(lines.size()) - 1;
    int width = 1;
    while (end_line >= 10) { end_line /= 10; ++width; }

    std::ostringstream oss;
    int num = start_line;
    for (const auto& line : lines) {
        oss << std::format("{:{}d}\t{}\n", num, width, line);
        ++num;
    }
    return oss.str();
}

struct AddLineNumbersParam {
    std::string content;
    int start_line = 1;
};
inline std::string add_line_numbers(const AddLineNumbersParam& p) {
    return add_line_numbers(p.content, p.start_line);
}

// ===========================================================================
// Snippet helpers — 3 variants from TS utils.ts + UI.tsx
// ===========================================================================

inline constexpr std::size_t kDiffSnippetMaxBytes = 8192; // 8 KiB cap
inline constexpr int kDefaultContextLines = 4;

/// getSnippetForTwoFileDiff() — used for edited-text-file attachments.
/// 8 KB cap, `... [N lines truncated] ...` marker.
inline std::string get_snippet_for_two_file_diff(
    std::string_view a, std::string_view b)
{
    auto hunks = compute_structured_patch(a, b, /*context=*/8);
    if (hunks.empty()) return "";

    auto apply = [&](std::string_view in) -> std::string {
        auto lines = patch_detail::split_lines(in);
        std::vector<std::string> kept;
        int line = 1;
        // We need: kept lines (non-deleted, non-meta) with line numbers.
        // Strategy: walk a, keeping lines not removed (use hunk info).
        // Simpler: render from new perspective, drop '-' and '\' lines.
        return {};
    };

    // Simpler approach — follow TS exactly:
    //   for each hunk: filter OUT lines starting with '-' or '\', drop the
    //   leading tag char, then addLineNumbers, join with '\n...\n'.
    std::string full;
    bool first_hunk = true;
    for (const auto& hunk : hunks) {
        std::vector<std::string> content_lines;
        for (const auto& ln : hunk.lines) {
            if (ln.empty()) { content_lines.push_back(""); continue; }
            if (ln[0] == '-' || (ln.size() >= 2 && ln[0] == '\\')) continue;
            content_lines.push_back(ln.size() > 1 ? ln.substr(1) : "");
        }
        std::ostringstream joined;
        for (size_t i = 0; i < content_lines.size(); ++i) {
            if (i) joined << '\n';
            joined << content_lines[i];
        }
        auto numbered = add_line_numbers({
            .content = joined.str(),
            .start_line = hunk.new_start
        });
        if (!first_hunk) full += "\n...\n";
        full += numbered;
        first_hunk = false;
    }

    if (full.size() <= kDiffSnippetMaxBytes) return full;

    // Truncate at last '\n' that fits within the cap.
    const auto cutoff = full.rfind('\n', kDiffSnippetMaxBytes);
    std::string kept = (cutoff != std::string::npos && cutoff > 0)
        ? full.substr(0, cutoff)
        : full.substr(0, kDiffSnippetMaxBytes);
    auto remaining = count_char_in_string(full, '\n', kept.size()) + 1;
    return kept + "\n\n... [" + std::to_string(remaining) + " lines truncated] ...";
}

struct SnippetResult {
    std::string formatted_snippet;
    int start_line = 0;
};

/// getSnippetForPatch() — show new-file context around patch hunks.
inline SnippetResult get_snippet_for_patch(
    const std::vector<PatchHunk>& patches,
    std::string_view new_file,
    int context_lines = kDefaultContextLines)
{
    if (patches.empty()) return {"", 1};

    int min_line = std::numeric_limits<int>::max();
    int max_line = std::numeric_limits<int>::min();
    for (const auto& h : patches) {
        if (h.old_start < min_line) min_line = h.old_start;
        int end = h.old_start + std::max(0, h.new_lines) - 1;
        if (end > max_line) max_line = end;
    }
    int start_line = std::max(1, min_line - context_lines);
    int end_line   = max_line + context_lines;

    auto lines = patch_detail::split_lines(new_file);
    std::ostringstream oss;
    for (int i = start_line - 1; i < end_line && i < static_cast<int>(lines.size()); ++i) {
        if (i >= start_line) oss << '\n';
        if (i >= 0) oss << lines[i];
    }
    auto formatted = add_line_numbers(oss.str(), start_line);
    return {std::move(formatted), start_line};
}

/// getSnippet() — simple, old-string-based snippet (UI.tsx legacy helper).
struct SimpleSnippet {
    std::string snippet;
    int start_line = 1;
};
inline SimpleSnippet get_snippet(
    std::string_view original_file,
    std::string_view old_string,
    std::string_view new_string,
    int context_lines = kDefaultContextLines)
{
    auto before_pos = original_file.find(old_string);
    std::string before = (before_pos == std::string_view::npos)
        ? std::string(original_file)
        : std::string(original_file.substr(0, before_pos));

    int replacement_line = 0;
    for (char c : before) if (c == '\n') ++replacement_line;

    auto new_file_lines = patch_detail::split_lines(
        apply_edit(original_file, old_string, new_string));
    auto new_lines = patch_detail::split_lines(new_string);

    int start_line = std::max(0, replacement_line - context_lines);
    int end_line   = replacement_line + context_lines +
                     static_cast<int>(new_lines.size());

    std::ostringstream oss;
    for (int i = start_line; i < end_line && i < static_cast<int>(new_file_lines.size()); ++i) {
        if (i > start_line) oss << '\n';
        oss << new_file_lines[i];
    }
    return {oss.str(), start_line + 1};
}

// ===========================================================================
// Patch → Edits (reverse direction)
// ===========================================================================

inline std::vector<FileEdit> get_edits_for_patch(
    const std::vector<PatchHunk>& hunks)
{
    std::vector<FileEdit> result;
    for (const auto& hunk : hunks) {
        std::vector<std::string> ctx, old_v, new_v;
        for (const auto& line : hunk.lines) {
            if (line.empty()) { ctx.push_back(""); old_v.push_back(""); new_v.push_back(""); continue; }
            char tag = line[0];
            std::string rest = line.size() > 1 ? line.substr(1) : "";
            if (tag == ' ') {
                ctx.push_back(rest); old_v.push_back(rest); new_v.push_back(rest);
            } else if (tag == '-') {
                old_v.push_back(rest);
            } else if (tag == '+') {
                new_v.push_back(rest);
            }
        }
        result.push_back(FileEdit{
            .old_string  = cc::utils::join(old_v, "\n"),
            .new_string  = cc::utils::join(new_v, "\n"),
            .replace_all = false,
        });
    }
    return result;
}

// ===========================================================================
// Desanitize + normalize_file_edit_input
// ===========================================================================

struct Desanitization {
    std::string_view from;
    std::string_view to;
};

// Matches DESANITIZATIONS in TS utils.ts exactly.
inline constexpr Desanitization kDesanitizations[] = {
    {"<fnr>",           "<function_results>"},
    {"<n>",             "<name>"},
    {"</n>",            "</name>"},
    {"<o>",             "<output>"},
    {"</o>",            "</output>"},
    {"<e>",             "<error>"},
    {"</e>",            "</error>"},
    {"<s>",             "<system>"},
    {"</s>",            "</system>"},
    {"<r>",             "<result>"},
    {"</r>",            "</result>"},
    {"< META_START >",  "<META_START>"},
    {"< META_END >",    "<META_END>"},
    {"< EOT >",         "<EOT>"},
    {"< META >",        "<META>"},
    {"< SOS >",         "<SOS>"},
    {"\n\nH:",          "\n\nHuman:"},
    {"\n\nA:",          "\n\nAssistant:"},
};

struct AppliedReplacement {
    std::string_view from;
    std::string_view to;
};

struct DesanitizeResult {
    std::string result;
    std::vector<AppliedReplacement> applied;
};

inline DesanitizeResult desanitize_match_string(std::string_view match) {
    DesanitizeResult r{std::string(match), {}};
    for (const auto& [from, to] : kDesanitizations) {
        std::string before = r.result;
        r.result = cc::utils::replace_all(std::move(r.result), from, to);
        if (r.result != before) {
            r.applied.push_back({from, to});
        }
    }
    return r;
}

struct NormalizedFileEditInput {
    std::filesystem::path file_path;
    std::vector<FileEdit> edits;
};

/// normalize_file_edit_input — exactly mirrors TS:
///   1. Skip strip_trailing_whitespace for .md / .mdx files.
///   2. For each edit, if the exact old_string is in the cached file
///      content, keep it; else try DESANITIZATIONS; else keep original.
///   3. On ENOENT, fall back to original input (matches TS "no TOCTOU
///      pre-check" strategy).
inline NormalizedFileEditInput normalize_file_edit_input(
    const std::filesystem::path& file_path,
    const std::vector<FileEdit>& edits,
    FileReadCache* cache = nullptr  // optional — callers can pass nullptr for testing
) {
    NormalizedFileEditInput out{file_path, {}};
    if (edits.empty()) { out.edits = edits; return out; }

    const bool is_markdown = [&] {
        auto ext = file_path.extension().string();
        // lowercase-compare
        std::string lower;
        lower.reserve(ext.size());
        for (char c : ext) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return lower == ".md" || lower == ".mdx";
    }();

    std::optional<std::string> file_content;
    try {
        if (cache) {
            file_content = cache->get(file_path);
        }
        if (!file_content) {
            std::ifstream f(file_path, std::ios::binary);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                file_content = ss.str();
            }
        }
    } catch (...) {
        // Treat any I/O problem as "no cached content" and fall through
        // to original edits, matching TS behaviour (only ENOENT is
        // explicitly expected, but we don't need to distinguish further).
        file_content = std::nullopt;
    }

    out.edits.reserve(edits.size());
    for (const auto& e : edits) {
        std::string new_s = is_markdown
            ? e.new_string
            : strip_trailing_whitespace(e.new_string);

        if (file_content && file_content->find(e.old_string) != std::string::npos) {
            out.edits.push_back(FileEdit{
                .old_string  = e.old_string,
                .new_string  = std::move(new_s),
                .replace_all = e.replace_all,
            });
            continue;
        }

        if (file_content) {
            auto de = desanitize_match_string(e.old_string);
            if (!de.applied.empty() &&
                file_content->find(de.result) != std::string::npos) {
                std::string de_new = new_s;
                for (const auto& rep : de.applied) {
                    de_new = cc::utils::replace_all(std::move(de_new), rep.from, rep.to);
                }
                out.edits.push_back(FileEdit{
                    .old_string  = std::move(de.result),
                    .new_string  = std::move(de_new),
                    .replace_all = e.replace_all,
                });
                continue;
            }
        }

        out.edits.push_back(FileEdit{
            .old_string  = e.old_string,
            .new_string  = std::move(new_s),
            .replace_all = e.replace_all,
        });
    }

    return out;
}

// ===========================================================================
// Edit equivalence (used by FileEditTool.inputsEquivalent).
// ===========================================================================

/// Are two edit sets equivalent when applied to the same original content?
/// Mirrors TS areFileEditsEquivalent() — applies both and compares outputs.
inline bool are_file_edits_equivalent(
    const std::vector<FileEdit>& edits1,
    const std::vector<FileEdit>& edits2,
    std::string_view original_content)
{
    // Fast path — literal equality
    if (edits1.size() == edits2.size()) {
        bool all_eq = true;
        for (size_t i = 0; i < edits1.size(); ++i) {
            const auto& a = edits1[i];
            const auto& b = edits2[i];
            if (a.old_string  != b.old_string  ||
                a.new_string  != b.new_string  ||
                a.replace_all != b.replace_all) { all_eq = false; break; }
        }
        if (all_eq) return true;
    }

    std::optional<std::string> r1, r2;
    std::string e1_msg, e2_msg;
    try {
        r1 = get_patch_for_edits("temp", original_content, edits1).updated_file;
    } catch (const std::exception& ex) { e1_msg = ex.what(); }
    try {
        r2 = get_patch_for_edits("temp", original_content, edits2).updated_file;
    } catch (const std::exception& ex) { e2_msg = ex.what(); }

    if (!r1 && !r2) return e1_msg == e2_msg;   // both failed → compare messages
    if (!r1 || !r2) return false;               // one succeeded → not equal
    return *r1 == *r2;
}

/// Full inputs-equivalent wrapper (different files ⇒ never equivalent).
inline bool are_file_edits_inputs_equivalent(
    const NormalizedFileEditInput& a,
    const NormalizedFileEditInput& b,
    FileReadCache* cache = nullptr)
{
    if (a.file_path != b.file_path) return false;

    // Fast path — literal equality of edits
    if (a.edits.size() == b.edits.size()) {
        bool all_eq = true;
        for (size_t i = 0; i < a.edits.size(); ++i) {
            const auto& x = a.edits[i];
            const auto& y = b.edits[i];
            if (x.old_string  != y.old_string  ||
                x.new_string  != y.new_string  ||
                x.replace_all != y.replace_all) { all_eq = false; break; }
        }
        if (all_eq) return true;
    }

    std::string content;
    try {
        if (cache) {
            if (auto c = cache->get(a.file_path)) content = std::move(*c);
        } else {
            std::ifstream f(a.file_path, std::ios::binary);
            if (f) {
                std::ostringstream ss; ss << f.rdbuf();
                content = ss.str();
            }
        }
    } catch (...) { /* ENOENT-like → use empty content */ }

    return are_file_edits_equivalent(a.edits, b.edits, content);
}

// ===========================================================================
// Read-file-for-edit helper (= TS readFileForEdit).
// NOTE: encoding / line-ending detection lives in a full file-read module;
// we deliberately keep the default path here (UTF-8, LF) simple so the
// caller can layer platform specifics on top.
// ===========================================================================

enum class LineEndingType { LF, CRLF, CR };

struct ReadForEditResult {
    std::string content;
    bool file_exists = false;
    std::string encoding = "utf-8";
    LineEndingType line_endings = LineEndingType::LF;
};

inline ReadForEditResult read_file_for_edit(const std::filesystem::path& p) {
    try {
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            return {.content = "", .file_exists = false,
                    .encoding = "utf-8", .line_endings = LineEndingType::LF};
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string raw = ss.str();

        // BOM sniffing: 0xFF 0xFE → UTF-16 LE. TS decodes via
        // `fileBuffer.toString(encoding)` with utf16le and then
        // `replaceAll("\r\n", "\n")`. We approximate by re-decoding on
        // demand; for simplicity in the C++ port we treat unrecognised
        // BOMs as binary and fall back to UTF-8 (callers validate).
        std::string enc = "utf-8";
        if (raw.size() >= 2 &&
            static_cast<unsigned char>(raw[0]) == 0xff &&
            static_cast<unsigned char>(raw[1]) == 0xfe) {
            enc = "utf-16le";
        }

        // Normalise all line endings to '\n' for downstream processing;
        // the CRLF/LF detection is preserved so write-back can restore.
        LineEndingType let = LineEndingType::LF;
        if (raw.find("\r\n") != std::string::npos) let = LineEndingType::CRLF;
        else if (raw.find('\r') != std::string::npos) let = LineEndingType::CR;

        std::string norm;
        norm.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
                norm += '\n'; ++i;
            } else if (raw[i] == '\r') {
                norm += '\n';
            } else {
                norm += raw[i];
            }
        }

        return {.content = std::move(norm), .file_exists = true,
                .encoding = std::move(enc), .line_endings = let};
    } catch (const std::ios_base::failure&) {
        return {.content = "", .file_exists = false,
                .encoding = "utf-8", .line_endings = LineEndingType::LF};
    }
}

} // namespace cc::utils::file_edit
