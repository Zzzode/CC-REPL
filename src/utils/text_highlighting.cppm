/// @file text_highlighting.cppm
/// @brief Text highlighting utilities: substring/regex match coloring plus
/// the priority-based segment system used by the prompt input widget.
///
/// TS REF (authority):
///   src/utils/textHighlighting.ts  – TextHighlight type, segmentTextByHighlights(),
///       HighlightSegmenter class, TextSegment type.
///   src/components/BaseTextInput.tsx – cursor filtering + viewport offset adjustment.
///   src/components/PromptInput/ShimmeredInput.tsx – HighlightedInput rendering.
///   src/components/PromptInput/PromptInput.tsx – combinedHighlights builder
///       (12+ sources with priority ordering).
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/screen/color.hpp>

export module cc.utils.text_highlighting;

export namespace cc::utils {

using ftxui::Color;

// ============================================================
// TextHighlight & TextSegment — mirror TS textHighlighting.ts
// ============================================================

/// A highlight annotation for a range of the input text.
/// Uses flat character offsets (start/end) into the full text buffer,
/// matching the TS TextHighlight type exactly.
/// TS REF: src/utils/textHighlighting.ts:11-19
struct TextHighlight {
    std::size_t start = 0;       ///< Inclusive character offset
    std::size_t end = 0;         ///< Exclusive character offset
    std::optional<Color> color;  ///< Foreground color (undefined = default)
    bool dim = false;            ///< TS dimColor — render dimmed
    bool inverse = false;        ///< TS inverse — invert fg/bg (cursor chip)
    std::optional<Color> shimmer_color;  ///< TS shimmerColor — animated sweep
    std::int32_t priority = 0;   ///< Higher = wins overlap resolution
};

/// A contiguous segment of text with an optional highlight.
/// Produced by segmentTextByHighlights().
/// TS REF: src/utils/textHighlighting.ts:21-25
struct TextSegment {
    std::string text;
    std::size_t start = 0;
    std::optional<TextHighlight> highlight;
};

// ============================================================
// segmentTextByHighlights — priority-based overlap resolution
// ============================================================

/// Split `text` into non-overlapping segments respecting highlight priorities.
///
/// Algorithm (matches TS segmentTextByHighlights exactly):
/// 1. Sort highlights by (start ascending, priority descending)
/// 2. Walk sorted highlights, skip any that overlap an already-claimed range
/// 3. Produce TextSegment[] covering the full text
///
/// TS REF: src/utils/textHighlighting.ts:27-60
[[nodiscard]] inline std::vector<TextSegment> segment_text_by_highlights(
    std::string_view text,
    const std::vector<TextHighlight>& highlights) {

    if (highlights.empty()) {
        return { TextSegment{ std::string(text), 0, std::nullopt } };
    }

    // Sort by start position, then by priority descending so that
    // higher-priority highlights at the same position win.
    // TS REF: textHighlighting.ts:35-38
    std::vector<TextHighlight> sorted = highlights;
    std::sort(sorted.begin(), sorted.end(),
        [](const TextHighlight& a, const TextHighlight& b) {
            if (a.start != b.start) return a.start < b.start;
            return a.priority > b.priority;
        });

    // Resolve overlaps: keep a highlight only if its range does not overlap
    // any already-claimed range.  TS REF: textHighlighting.ts:40-57
    std::vector<TextHighlight> resolved;
    std::vector<std::pair<std::size_t, std::size_t>> used_ranges;

    for (const auto& hl : sorted) {
        if (hl.start >= hl.end) continue;  // skip empty

        bool overlaps = false;
        for (const auto& [rstart, rend] : used_ranges) {
            // TS overlap check: textHighlighting.ts:46-51
            bool case1 = (hl.start >= rstart && hl.start < rend);
            bool case2 = (hl.end > rstart && hl.end <= rend);
            bool case3 = (hl.start <= rstart && hl.end >= rend);
            if (case1 || case2 || case3) {
                overlaps = true;
                break;
            }
        }

        if (!overlaps) {
            resolved.push_back(hl);
            used_ranges.emplace_back(hl.start, hl.end);
        }
    }

    // Build segments by walking through resolved highlights.
    // TS REF: textHighlighting.ts:62-162 (HighlightSegmenter)
    std::vector<TextSegment> segments;
    std::size_t pos = 0;

    for (const auto& hl : resolved) {
        // Text before this highlight
        if (hl.start > pos) {
            segments.push_back(TextSegment{
                std::string(text.substr(pos, hl.start - pos)),
                pos,
                std::nullopt
            });
        }
        // The highlighted range
        if (hl.end > hl.start && hl.start < text.size()) {
            std::size_t seg_end = std::min(hl.end, text.size());
            segments.push_back(TextSegment{
                std::string(text.substr(hl.start, seg_end - hl.start)),
                hl.start,
                hl
            });
        }
        pos = hl.end;
    }

    // Trailing text after last highlight
    if (pos < text.size()) {
        segments.push_back(TextSegment{
            std::string(text.substr(pos)),
            pos,
            std::nullopt
        });
    }

    return segments;
}

// ============================================================
// Cursor filtering & viewport adjustment — mirror BaseTextInput
// ============================================================

/// Filter highlights that contain the cursor position so the character
/// under the cursor is never styled (except dim highlights, which always
/// show).  Mirrors TS BaseTextInput.tsx:93 cursorFiltered logic.
///
/// TS REF: src/components/BaseTextInput.tsx:93
[[nodiscard]] inline std::vector<TextHighlight> filter_highlights_at_cursor(
    const std::vector<TextHighlight>& highlights,
    std::size_t cursor_offset,
    bool show_cursor = true) {

    if (!show_cursor) return highlights;

    std::vector<TextHighlight> filtered;
    filtered.reserve(highlights.size());
    for (const auto& h : highlights) {
        // Keep if: it's a dim highlight (always visible), OR
        // the cursor is strictly before the highlight, OR
        // the cursor is at-or-past the end
        if (h.dim || cursor_offset < h.start || cursor_offset >= h.end) {
            filtered.push_back(h);
        }
    }
    return filtered;
}

/// Adjust highlight offsets for a viewport window (when the input text is
/// scrolled horizontally).  Highlights outside the window are dropped;
/// those inside have their start/end rebased to viewport-relative offsets.
/// Mirrors TS BaseTextInput.tsx:98-102.
///
/// TS REF: src/components/BaseTextInput.tsx:98-102
[[nodiscard]] inline std::vector<TextHighlight> adjust_highlights_for_viewport(
    const std::vector<TextHighlight>& highlights,
    std::size_t viewport_char_offset,
    std::size_t viewport_char_end) {

    if (viewport_char_offset == 0) return highlights;

    std::vector<TextHighlight> adjusted;
    adjusted.reserve(highlights.size());
    for (const auto& h : highlights) {
        // Drop highlights entirely outside the viewport
        if (h.end <= viewport_char_offset || h.start >= viewport_char_end) {
            continue;
        }
        TextHighlight adj = h;
        adj.start = (h.start > viewport_char_offset)
            ? h.start - viewport_char_offset
            : 0;
        adj.end = h.end - viewport_char_offset;
        adjusted.push_back(adj);
    }
    return adjusted;
}

// ============================================================
// Priority constants — mirror PromptInput.tsx combinedHighlights
// ============================================================

/// Priority values used by the combined highlights builder in
/// PromptInput.tsx.  Higher values win overlap resolution.
/// TS REF: src/components/PromptInput/PromptInput.tsx:601-741
namespace highlight_priority {
    inline constexpr std::int32_t VoiceInterim    = 1;   // dim interim voice text
    inline constexpr std::int32_t SlashCommand    = 5;   // /command blue
    inline constexpr std::int32_t TokenBudget     = 5;   // token budget blue
    inline constexpr std::int32_t SlackChannel    = 5;   // #channel blue
    inline constexpr std::int32_t Mention         = 5;   // @name teammate color
    inline constexpr std::int32_t ImageChip       = 8;   // [Image #N] inverse cursor
    inline constexpr std::int32_t RainbowShimmer  = 10;  // think/ultraplan/buddy per-char
    inline constexpr std::int32_t BtwTrigger      = 15;  // btw yellow
    inline constexpr std::int32_t HistorySearch   = 20;  // history nav match
} // namespace highlight_priority

// ============================================================
// Legacy helpers (kept for backward compatibility)
// ============================================================

/// Highlight all occurrences of query in text with specified ANSI color.
/// Legacy helper — new code should use segment_text_by_highlights().
std::string highlight_matches(std::string_view text, std::string_view query, std::string_view color) {
    if (query.empty()) return std::string(text);

    static constexpr std::string_view reset = "\033[0m";
    std::string result;
    result.reserve(text.size() * 2);

    std::size_t pos = 0;
    while (pos < text.size()) {
        auto found = text.find(query, pos);
        if (found == std::string_view::npos) {
            result += text.substr(pos);
            break;
        }
        result += text.substr(pos, found - pos);
        result += color;
        result += query;
        result += reset;
        pos = found + query.size();
    }
    return result;
}

/// Highlight regex pattern matches in text.
/// Legacy helper — new code should use segment_text_by_highlights().
std::string highlight_regex(std::string_view text, std::string_view pattern, std::string_view color) {
    static constexpr std::string_view reset = "\033[0m";

    try {
        std::regex re(pattern.begin(), pattern.end());
        std::string input(text);
        std::string result;
        result.reserve(input.size() * 2);

        std::sregex_iterator it(input.begin(), input.end(), re);
        std::sregex_iterator end;

        std::size_t last_pos = 0;
        for (; it != end; ++it) {
            auto& match = *it;
            std::size_t match_start = static_cast<std::size_t>(match.position());
            std::size_t match_len = static_cast<std::size_t>(match.length());

            result += input.substr(last_pos, match_start - last_pos);
            result += color;
            result += match.str();
            result += reset;

            last_pos = match_start + match_len;
        }
        result += input.substr(last_pos);
        return result;
    } catch (const std::regex_error&) {
        return std::string(text);
    }
}

/// Count non-overlapping occurrences of query in text.
std::size_t count_matches(std::string_view text, std::string_view query) {
    if (query.empty()) return 0;

    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto found = text.find(query, pos);
        if (found == std::string_view::npos) break;
        ++count;
        pos = found + query.size();
    }
    return count;
}

} // namespace cc::utils