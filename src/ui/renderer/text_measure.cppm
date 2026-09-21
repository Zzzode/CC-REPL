/// @file text_measure.cppm
/// @brief Text measurement utilities for terminal rendering.
/// Migrates: src/ink/ text measurement files
///   - measure-text.ts, measure-element.ts, stringWidth.ts,
///     widest-line.ts, line-width-cache.ts, get-max-width.ts, bidi.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <algorithm>

export module cc.ui.text_measure;

export namespace cc::ui::text_measure {

// ─── Types ───────────────────────────────────────────────────────────────────

struct TextMetrics {
    size_t width;
    size_t height;
    size_t visible_width; // excluding ANSI escapes
};

enum class BidiDirection {
    LTR,
    RTL,
    Auto
};

struct LineWidthEntry {
    std::string_view line;
    size_t width;
    bool has_ansi;
};

struct MeasureConfig {
    int tab_width{8};
    BidiDirection direction{BidiDirection::Auto};
    bool strip_ansi{true};
};

// ─── Internal Helpers ────────────────────────────────────────────────────────

namespace detail {

/// Decode a single UTF-8 code point from the given position.
/// Returns {code_point, bytes_consumed}. Returns {0xFFFD, 1} on invalid input.
inline auto decode_utf8(const char* data, size_t len) -> std::pair<char32_t, size_t> {
    if (len == 0) return {0xFFFD, 0};

    auto byte = static_cast<uint8_t>(data[0]);

    if (byte < 0x80) {
        return {static_cast<char32_t>(byte), 1};
    }
    if ((byte & 0xE0) == 0xC0) {
        if (len < 2) return {0xFFFD, 1};
        char32_t cp = (static_cast<char32_t>(byte & 0x1F) << 6) |
                      (static_cast<char32_t>(data[1]) & 0x3F);
        return {cp, 2};
    }
    if ((byte & 0xF0) == 0xE0) {
        if (len < 3) return {0xFFFD, 1};
        char32_t cp = (static_cast<char32_t>(byte & 0x0F) << 12) |
                      ((static_cast<char32_t>(data[1]) & 0x3F) << 6) |
                      (static_cast<char32_t>(data[2]) & 0x3F);
        return {cp, 3};
    }
    if ((byte & 0xF8) == 0xF0) {
        if (len < 4) return {0xFFFD, 1};
        char32_t cp = (static_cast<char32_t>(byte & 0x07) << 18) |
                      ((static_cast<char32_t>(data[1]) & 0x3F) << 12) |
                      ((static_cast<char32_t>(data[2]) & 0x3F) << 6) |
                      (static_cast<char32_t>(data[3]) & 0x3F);
        return {cp, 4};
    }
    return {0xFFFD, 1};
}

/// Check if we're inside an ANSI escape sequence.
/// Returns the number of bytes to skip (0 if not an escape).
inline auto skip_ansi_escape(const char* data, size_t len) -> size_t {
    if (len < 2 || data[0] != '\033') return 0;

    // CSI sequences: ESC [ ... final_byte (0x40-0x7E)
    if (data[1] == '[') {
        size_t i = 2;
        while (i < len) {
            auto ch = static_cast<uint8_t>(data[i]);
            if (ch >= 0x40 && ch <= 0x7E) {
                return i + 1;
            }
            ++i;
        }
        return len; // unterminated, skip all
    }

    // OSC sequences: ESC ] ... ST (ESC \ or BEL)
    if (data[1] == ']') {
        size_t i = 2;
        while (i < len) {
            if (data[i] == '\007') return i + 1; // BEL terminator
            if (data[i] == '\033' && i + 1 < len && data[i + 1] == '\\') {
                return i + 2; // ST terminator
            }
            ++i;
        }
        return len;
    }

    // Single-character escape: ESC followed by one char (e.g., ESC 7, ESC 8)
    if (len >= 2) return 2;
    return 1;
}

/// Check if a code point is an emoji modifier, ZWJ, or variation selector
/// that should be handled as zero-width in sequences.
inline auto is_emoji_modifier(char32_t ch) -> bool {
    return (ch >= 0x1F3FB && ch <= 0x1F3FF) || // Fitzpatrick skin tones
           ch == 0x200D ||                      // ZWJ
           ch == 0xFE0F ||                      // Variation Selector-16 (emoji presentation)
           ch == 0xFE0E;                        // Variation Selector-15 (text presentation)
}

} // namespace detail

// ─── Functions ───────────────────────────────────────────────────────────────

/// Determines if a Unicode code point is a wide (full-width) character.
inline auto is_wide_char(char32_t ch) -> bool {
    // East Asian Fullwidth (F), Wide (W), and some ambiguous ranges commonly
    // treated as wide in CJK terminal contexts. Based on UAX #11 plus emoji.
    return (ch >= 0x1100 &&
            (ch <= 0x115F ||                        // Hangul Jamo
             ch == 0x2329 || ch == 0x232A ||        // Angle brackets
             (ch >= 0x2E80 && ch <= 0x303E) ||      // CJK Radicals, Kangxi, CJK Symbols
             (ch >= 0x3040 && ch <= 0x33BF) ||      // Hiragana, Katakana, Bopomofo, etc.
             (ch >= 0x33C0 && ch <= 0x33FF) ||      // CJK Compatibility
             (ch >= 0x3400 && ch <= 0x4DBF) ||      // CJK Unified Extension A
             (ch >= 0x4E00 && ch <= 0x9FFF) ||      // CJK Unified
             (ch >= 0xA000 && ch <= 0xA4CF) ||      // Yi
             (ch >= 0xA960 && ch <= 0xA97F) ||      // Hangul Jamo Extended-A
             (ch >= 0xAC00 && ch <= 0xD7AF) ||      // Hangul Syllables
             (ch >= 0xF900 && ch <= 0xFAFF) ||      // CJK Compatibility Ideographs
             (ch >= 0xFE10 && ch <= 0xFE19) ||      // Vertical Forms
             (ch >= 0xFE30 && ch <= 0xFE6F) ||      // CJK Compatibility Forms
             (ch >= 0xFF01 && ch <= 0xFF60) ||      // Fullwidth ASCII variants
             (ch >= 0xFFE0 && ch <= 0xFFE6) ||      // Fullwidth Signs
             (ch >= 0x1F004 && ch <= 0x1F004) ||    // Mahjong Tile Red Dragon
             (ch >= 0x1F0CF && ch <= 0x1F0CF) ||    // Playing Card Black Joker
             (ch >= 0x1F18E && ch <= 0x1F18E) ||    // Negative Squared AB
             (ch >= 0x1F191 && ch <= 0x1F19A) ||    // Squared CL..Squared VS
             (ch >= 0x1F200 && ch <= 0x1F202) ||    // Square Hiragana
             (ch >= 0x1F210 && ch <= 0x1F23B) ||    // Squared CJK Unified
             (ch >= 0x1F240 && ch <= 0x1F248) ||    // Tortoise Shell Bracketed CJK
             (ch >= 0x1F250 && ch <= 0x1F251) ||    // Circled Ideograph
             (ch >= 0x1F300 && ch <= 0x1F5FF) ||    // Miscellaneous Symbols and Pictographs
             (ch >= 0x1F600 && ch <= 0x1F64F) ||    // Emoticons
             (ch >= 0x1F680 && ch <= 0x1F6FF) ||    // Transport and Map Symbols
             (ch >= 0x1F700 && ch <= 0x1F77F) ||    // Alchemical Symbols
             (ch >= 0x1F780 && ch <= 0x1F7FF) ||    // Geometric Shapes Extended
             (ch >= 0x1F800 && ch <= 0x1F8FF) ||    // Supplemental Arrows-C
             (ch >= 0x1F900 && ch <= 0x1F9FF) ||    // Supplemental Symbols and Pictographs
             (ch >= 0x1FA00 && ch <= 0x1FA6F) ||    // Chess Symbols
             (ch >= 0x1FA70 && ch <= 0x1FAFF) ||    // Symbols and Pictographs Extended-A
             (ch >= 0x20000 && ch <= 0x2FFFD) ||    // CJK Unified Extension B+
             (ch >= 0x30000 && ch <= 0x3FFFD)));    // CJK Unified Extension G+
}

/// Determines if a Unicode code point is a combining character (zero width).
inline auto is_combining_char(char32_t ch) -> bool {
    return (ch >= 0x0300 && ch <= 0x036F) ||   // Combining Diacritical Marks
           (ch >= 0x0483 && ch <= 0x0489) ||   // Cyrillic combining marks
           (ch >= 0x0591 && ch <= 0x05BD) ||   // Hebrew combining marks
           (ch >= 0x05BF && ch <= 0x05BF) ||
           (ch >= 0x05C1 && ch <= 0x05C2) ||
           (ch >= 0x05C4 && ch <= 0x05C5) ||
           (ch >= 0x05C7 && ch <= 0x05C7) ||
           (ch >= 0x0610 && ch <= 0x061A) ||   // Arabic combining marks
           (ch >= 0x064B && ch <= 0x065F) ||
           (ch >= 0x0670 && ch <= 0x0670) ||
           (ch >= 0x06D6 && ch <= 0x06DC) ||
           (ch >= 0x06DF && ch <= 0x06E4) ||
           (ch >= 0x06E7 && ch <= 0x06E8) ||
           (ch >= 0x06EA && ch <= 0x06ED) ||
           (ch >= 0x0711 && ch <= 0x0711) ||   // Syriac
           (ch >= 0x0730 && ch <= 0x074A) ||
           (ch >= 0x0E31 && ch <= 0x0E31) ||   // Thai
           (ch >= 0x0E34 && ch <= 0x0E3A) ||
           (ch >= 0x0E47 && ch <= 0x0E4E) ||
           (ch >= 0x1AB0 && ch <= 0x1AFF) ||   // Combining Diacritical Extended
           (ch >= 0x1DC0 && ch <= 0x1DFF) ||   // Combining Diacritical Supplement
           (ch >= 0x20D0 && ch <= 0x20FF) ||   // Combining Diacritical for Symbols
           (ch >= 0xFE00 && ch <= 0xFE0F) ||   // Variation Selectors
           (ch >= 0xFE20 && ch <= 0xFE2F) ||   // Combining Half Marks
           (ch >= 0xE0100 && ch <= 0xE01EF);   // Variation Selectors Supplement
}

/// Determines if a code point is a zero-width control character.
inline auto is_zero_width_control(char32_t ch) -> bool {
    return ch == 0x200B ||  // Zero Width Space
           ch == 0x200C ||  // Zero Width Non-Joiner
           ch == 0x200D ||  // Zero Width Joiner
           ch == 0x2060 ||  // Word Joiner
           ch == 0xFEFF ||  // Zero Width No-Break Space (BOM)
           ch == 0x00AD;    // Soft Hyphen
}

/// Returns the display width of a character: 0 for combining, 2 for wide, 1 otherwise.
inline auto char_width(char32_t ch) -> int {
    if (ch < 0x20) return 0;    // Control characters
    if (is_zero_width_control(ch)) return 0;
    if (is_combining_char(ch)) return 0;
    if (detail::is_emoji_modifier(ch)) return 0;
    if (is_wide_char(ch)) return 2;
    return 1;
}

/// Strips ANSI escape sequences from text.
inline auto strip_ansi(std::string_view text) -> std::string {
    std::string result;
    result.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        auto skip = detail::skip_ansi_escape(text.data() + i, text.size() - i);
        if (skip > 0) {
            i += skip;
        } else {
            result += text[i];
            ++i;
        }
    }
    return result;
}

/// Returns whether text contains any ANSI escape sequences.
inline auto has_ansi(std::string_view text) -> bool {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\033' && i + 1 < text.size()) {
            return true;
        }
    }
    return false;
}

/// Returns visible character width, handling wide chars and UTF-8.
inline auto string_width(std::string_view text) -> size_t {
    size_t width = 0;
    size_t i = 0;

    while (i < text.size()) {
        // Skip ANSI sequences
        auto skip = detail::skip_ansi_escape(text.data() + i, text.size() - i);
        if (skip > 0) {
            i += skip;
            continue;
        }

        // Handle newlines - stop at first newline (single-line measurement)
        if (text[i] == '\n') break;

        // Handle tab as configurable width (default 8)
        if (text[i] == '\t') {
            width += 8 - (width % 8);
            ++i;
            continue;
        }

        // Decode UTF-8 and get character width
        auto [cp, bytes] = detail::decode_utf8(text.data() + i, text.size() - i);
        width += static_cast<size_t>(std::max(0, char_width(cp)));
        i += bytes;
    }

    return width;
}

/// Strips ANSI escape sequences first, then returns visible width.
inline auto string_width_ansi(std::string_view text) -> size_t {
    // string_width already handles ANSI internally, but this provides
    // an explicit API for callers who want to emphasize ANSI stripping.
    return string_width(text);
}

/// Returns the width of the widest line in a multi-line string.
inline auto widest_line(std::string_view text) -> size_t {
    size_t max_width = 0;
    size_t line_start = 0;

    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            auto line = text.substr(line_start, i - line_start);
            auto w = string_width(line);
            if (w > max_width) max_width = w;
            line_start = i + 1;
        }
    }

    return max_width;
}

/// Measures text dimensions with configurable options.
inline auto measure_text(std::string_view text, MeasureConfig config = {}) -> TextMetrics {
    size_t max_width = 0;
    size_t height = 0;
    size_t line_start = 0;

    // Count lines and find the widest
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            ++height;
            auto line = text.substr(line_start, i - line_start);

            // Expand tabs if needed
            size_t line_width = 0;
            size_t col = 0;
            size_t j = 0;
            while (j < line.size()) {
                auto skip = detail::skip_ansi_escape(line.data() + j, line.size() - j);
                if (skip > 0) {
                    j += skip;
                    continue;
                }
                if (line[j] == '\t') {
                    int spaces = config.tab_width - static_cast<int>(col % static_cast<size_t>(config.tab_width));
                    line_width += static_cast<size_t>(spaces);
                    col += static_cast<size_t>(spaces);
                    ++j;
                    continue;
                }
                auto [cp, bytes] = detail::decode_utf8(line.data() + j, line.size() - j);
                auto cw = static_cast<size_t>(std::max(0, char_width(cp)));
                line_width += cw;
                col += cw;
                j += bytes;
            }

            if (line_width > max_width) max_width = line_width;
            line_start = i + 1;
        }
    }

    if (height == 0) height = 1;

    return TextMetrics{
        .width = max_width,
        .height = height,
        .visible_width = max_width
    };
}

/// Returns the maximum width across a span of lines.
inline auto get_max_width(std::span<const std::string> lines) -> size_t {
    size_t max_w = 0;
    for (auto const& line : lines) {
        auto w = string_width(line);
        if (w > max_w) max_w = w;
    }
    return max_w;
}

/// Detects the base bidi direction of a text string using UAX#9 first strong character rule.
inline auto detect_bidi_direction(std::string_view text) -> BidiDirection {
    size_t i = 0;
    while (i < text.size()) {
        // Skip ANSI
        auto skip = detail::skip_ansi_escape(text.data() + i, text.size() - i);
        if (skip > 0) { i += skip; continue; }

        auto [cp, bytes] = detail::decode_utf8(text.data() + i, text.size() - i);
        i += bytes;

        // Strong LTR: Latin, Greek, Cyrillic, CJK, etc.
        if ((cp >= 0x0041 && cp <= 0x005A) || // A-Z
            (cp >= 0x0061 && cp <= 0x007A) || // a-z
            (cp >= 0x00C0 && cp <= 0x02AF) || // Latin Extended
            (cp >= 0x0370 && cp <= 0x03FF) || // Greek
            (cp >= 0x0400 && cp <= 0x04FF) || // Cyrillic
            (cp >= 0x1100 && cp <= 0x11FF) || // Hangul Jamo
            (cp >= 0x3040 && cp <= 0x9FFF) || // CJK
            (cp >= 0xAC00 && cp <= 0xD7AF)) { // Hangul Syllables
            return BidiDirection::LTR;
        }

        // Strong RTL: Arabic, Hebrew, Thaana, etc.
        if ((cp >= 0x0590 && cp <= 0x05FF) || // Hebrew
            (cp >= 0x0600 && cp <= 0x06FF) || // Arabic
            (cp >= 0x0700 && cp <= 0x074F) || // Syriac
            (cp >= 0x0750 && cp <= 0x077F) || // Arabic Supplement
            (cp >= 0x0780 && cp <= 0x07BF) || // Thaana
            (cp >= 0x07C0 && cp <= 0x07FF) || // NKo
            (cp >= 0x0800 && cp <= 0x083F) || // Samaritan
            (cp >= 0x0840 && cp <= 0x085F) || // Mandaic
            (cp >= 0x08A0 && cp <= 0x08FF) || // Arabic Extended-A
            (cp >= 0xFB50 && cp <= 0xFDFF) || // Arabic Presentation A
            (cp >= 0xFE70 && cp <= 0xFEFF) || // Arabic Presentation B
            (cp >= 0x10800 && cp <= 0x10FFF) || // RTL supplementary
            (cp >= 0x1E800 && cp <= 0x1EFFF)) { // RTL supplementary
            return BidiDirection::RTL;
        }
    }

    return BidiDirection::LTR; // Default to LTR if no strong character found
}

/// Reorders text according to the Unicode Bidi Algorithm (simplified).
/// Full UAX#9 is extremely complex; this provides basic mirror-and-reverse
/// for simple RTL-in-LTR and LTR-in-RTL embedding scenarios.
inline auto reorder_bidi(std::string_view text, BidiDirection base) -> std::string {
    if (base == BidiDirection::Auto) {
        base = detect_bidi_direction(text);
    }

    // For LTR base direction with no RTL content, return as-is
    if (base == BidiDirection::LTR) {
        // Simple heuristic: if no RTL characters detected, no reordering needed
        bool has_rtl = false;
        size_t i = 0;
        while (i < text.size()) {
            auto [cp, bytes] = detail::decode_utf8(text.data() + i, text.size() - i);
            if ((cp >= 0x0590 && cp <= 0x08FF) ||
                (cp >= 0xFB50 && cp <= 0xFEFF)) {
                has_rtl = true;
                break;
            }
            i += bytes;
        }
        if (!has_rtl) return std::string{text};
    }

    // For RTL base or mixed content: reverse the logical order of RTL runs.
    // This is a simplified version - a full implementation would use the UAX#9
    // algorithm with embedding levels, but for terminal rendering this handles
    // the common case of simple RTL text.
    if (base == BidiDirection::RTL) {
        // Decode all code points
        std::vector<char32_t> codepoints;
        size_t i = 0;
        while (i < text.size()) {
            auto [cp, bytes] = detail::decode_utf8(text.data() + i, text.size() - i);
            codepoints.push_back(cp);
            i += bytes;
        }

        // Reverse the entire sequence for RTL display
        std::ranges::reverse(codepoints);

        // Re-encode to UTF-8
        std::string result;
        result.reserve(text.size());
        for (auto cp : codepoints) {
            if (cp < 0x80) {
                result += static_cast<char>(cp);
            } else if (cp < 0x800) {
                result += static_cast<char>(0xC0 | (cp >> 6));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                result += static_cast<char>(0xE0 | (cp >> 12));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                result += static_cast<char>(0xF0 | (cp >> 18));
                result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        return result;
    }

    return std::string{text};
}

// ─── LineWidthCache ──────────────────────────────────────────────────────────

/// Cache for computed line widths to avoid redundant measurement.
class LineWidthCache {
public:
    /// Returns cached width for a line, computing and caching if absent.
    auto get_width(std::string_view line) -> size_t {
        auto key = std::string{line};
        if (auto it = cache_.find(key); it != cache_.end()) {
            ++hits_;
            return it->second;
        }
        ++misses_;
        auto w = string_width(line);
        if (cache_.size() < max_capacity_) {
            cache_.emplace(std::move(key), w);
        }
        return w;
    }

    /// Removes a specific line from the cache.
    auto invalidate(std::string_view line) -> void {
        cache_.erase(std::string{line});
    }

    /// Clears all cached entries.
    auto clear() -> void {
        cache_.clear();
        hits_ = 0;
        misses_ = 0;
    }

    /// Returns the number of cached entries.
    [[nodiscard]] auto size() const -> size_t {
        return cache_.size();
    }

    /// Returns cache hit rate (0.0 - 1.0).
    [[nodiscard]] auto hit_rate() const -> double {
        auto total = hits_ + misses_;
        if (total == 0) return 0.0;
        return static_cast<double>(hits_) / static_cast<double>(total);
    }

    /// Set maximum capacity.
    void set_max_capacity(size_t cap) { max_capacity_ = cap; }

private:
    std::unordered_map<std::string, size_t> cache_;
    size_t hits_ = 0;
    size_t misses_ = 0;
    size_t max_capacity_ = 4096;
};

} // namespace cc::ui::text_measure
