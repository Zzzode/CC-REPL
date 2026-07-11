/// @file combined_highlights.cppm
/// @brief 8-tier combined highlights builder for the prompt input widget.
///
/// Assembles TextHighlight annotations from 8+ sources in priority order,
/// mirroring the TS combinedHighlights useMemo in PromptInput.tsx:601-741.
///
/// Sources (highest priority first):
///   1. History search highlights (priority 20)
///   2. Btw trigger highlights (priority 15)
///   3. Rainbow shimmer per-char (priority 10)
///   4. Image chip inverse (priority 8)
///   5. Slash command (priority 5)
///   6. Token budget (priority 5)
///   7. Member mention (priority 5)
///   8. Voice interim dim (priority 1)
///
/// TS REF (authority):
///   src/components/PromptInput/PromptInput.tsx:601-741 (combinedHighlights builder)
///   src/utils/thinking.ts:60-86 (RAINBOW_COLORS, getRainbowColor)
///   src/utils/sideQuestion.ts:16-41 (findBtwTriggerPositions)
///   src/utils/suggestions/commandSuggestions.ts:552-567 (findSlashCommandPositions)
///   src/utils/tokenBudget.ts:31-50 (findTokenBudgetPositions)
///   src/utils/ultraplan/keyword.ts (findUltraplanTriggerPositions, findUltrareviewTriggerPositions)
///   src/buddy/useBuddyNotification.tsx:79-97 (findBuddyTriggerPositions)
///   src/history.ts:62-75 (parseReferences)
///   src/components/PromptInput/PromptInput.tsx:541-579 (memberMentionHighlights)
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/screen/color.hpp>

export module cc.ui.prompt.combined_highlights;

import cc.utils.text_highlighting;
import cc.utils.parse_references;

export namespace cc::ui::prompt {

using ftxui::Color;
using cc::utils::TextHighlight;
namespace highlight_priority = cc::utils::highlight_priority;
using cc::utils::ReferenceMatch;
using cc::utils::parse_references;

// ============================================================
// Context struct — carries all inputs needed by the builder
// ============================================================

/// Input context for build_combined_highlights().
/// Mirrors the closure variables captured by the TS combinedHighlights useMemo.
struct CombinedHighlightContext {
    /// The full displayed text (may include history-nav overlay).
    /// TS REF: PromptInput.tsx:518 displayedValue
    std::string_view text;

    /// Flat character offset of the cursor in the full text buffer.
    /// TS REF: PromptInput.tsx cursorOffset
    std::size_t cursor_offset = 0;

    // ── Feature gates (mirror TS feature() / isEnabled() calls) ──────────

    /// When true, "ultrathink" keyword gets rainbow per-char highlighting.
    /// TS REF: thinking.ts:19 isUltrathinkEnabled()
    bool ultrathink_enabled = false;

    /// When true, "ultraplan" keyword gets rainbow per-char highlighting.
    /// TS REF: PromptInput.tsx:701 feature('ULTRAPLAN')
    bool ultraplan_enabled = false;

    /// When true, "ultrareview" keyword gets rainbow per-char highlighting.
    /// TS REF: PromptInput.tsx:716 (always checked, no feature gate in TS)
    bool ultrareview_enabled = true;

    /// When true, "/buddy" keyword gets rainbow per-char highlighting.
    /// TS REF: useBuddyNotification.tsx:83 feature('BUDDY')
    bool buddy_enabled = false;

    /// When true, "+500k" token budget shorthand gets highlighted.
    /// TS REF: PromptInput.tsx:534 feature('TOKEN_BUDGET')
    bool token_budget_enabled = false;

    // ── History search state ──────────────────────────────────────────────

    /// True when the user is navigating history (up/down arrow or ctrl+r).
    /// TS REF: PromptInput.tsx isSearchingHistory
    bool is_searching_history = false;

    /// The history search query string (for highlighting matched terms).
    /// TS REF: PromptInput.tsx historyQuery
    std::string_view history_query;

    /// True when a history match was found (highlight the matched range).
    /// TS REF: PromptInput.tsx historyMatch
    bool history_match_found = false;

    /// True when history search failed (no match — don't highlight).
    /// TS REF: PromptInput.tsx historyFailedMatch
    bool history_failed_match = false;

    // ── Voice interim ─────────────────────────────────────────────────────

    /// Range of voice interim text (start, end) in the text buffer.
    /// When set, the range is rendered dimmed.
    /// TS REF: PromptInput.tsx:675-683 voiceInterimRange
    std::optional<std::pair<std::size_t, std::size_t>> voice_interim_range;

    // ── Member mentions ───────────────────────────────────────────────────

    /// List of known team member names for @mention highlighting.
    /// Each entry: {name, color} — color is the FTXUI Color to use.
    /// TS REF: PromptInput.tsx:541-579 memberMentionHighlights
    std::vector<std::pair<std::string, Color>> team_members;

    /// When true, @mention highlighting is active (agent swarms enabled).
    /// TS REF: PromptInput.tsx:546 isAgentSwarmsEnabled()
    bool agent_swarms_enabled = false;
};

// ============================================================
// Rainbow color palette — mirror TS RAINBOW_COLORS
// ============================================================

/// 7-color rainbow cycle for per-character shimmer highlighting.
/// TS REF: src/utils/thinking.ts:60-68 RAINBOW_COLORS
///   ['rainbow_red', 'rainbow_orange', 'rainbow_yellow',
///    'rainbow_green', 'rainbow_blue', 'rainbow_indigo', 'rainbow_violet']
///
/// We map these theme tokens to the closest FTXUI palette colors.
/// FTXUI v5 has no named Orange, so we use RGB for orange/indigo.
inline const std::array<Color, 7> RAINBOW_COLORS = {
    Color::Red,                          // rainbow_red
    Color::RGB(255, 165, 0),             // rainbow_orange
    Color::Yellow,                       // rainbow_yellow
    Color::Green,                        // rainbow_green
    Color::Blue,                         // rainbow_blue
    Color::RGB(75, 0, 130),              // rainbow_indigo (indigo approx)
    Color::Magenta,                      // rainbow_violet
};

/// Shimmer variant of the rainbow colors — slightly brighter for the
/// animated sweep highlight.  TS REF: thinking.ts:70-78 RAINBOW_SHIMMER_COLORS
inline const std::array<Color, 7> RAINBOW_SHIMMER_COLORS = {
    Color::RedLight,
    Color::RGB(255, 200, 100),           // shimmer orange
    Color::YellowLight,
    Color::GreenLight,
    Color::BlueLight,
    Color::RGB(130, 100, 200),           // shimmer indigo
    Color::MagentaLight,
};

/// Get the rainbow color for a given character index within a trigger word.
/// @param char_index  Position within the trigger (0 = first char).
/// @param shimmer     If true, return the brighter shimmer variant.
/// TS REF: src/utils/thinking.ts:80-86 getRainbowColor()
[[nodiscard]] inline Color get_rainbow_color(
    std::size_t char_index,
    bool shimmer = false) {
    const auto& palette = shimmer ? RAINBOW_SHIMMER_COLORS : RAINBOW_COLORS;
    return palette[char_index % palette.size()];
}

// ============================================================
// Trigger detection helpers — mirror TS find*TriggerPositions
// ============================================================

/// A detected trigger position in the text.
/// TS REF: thinking.ts:36-58 findThinkingTriggerPositions return type
struct TriggerPosition {
    std::size_t start = 0;
    std::size_t end = 0;
};

/// Find all occurrences of a word-boundary keyword in the text.
/// Mirrors TS findThinkingTriggerPositions / findBtwTriggerPositions.
/// TS REF: thinking.ts:36-58, sideQuestion.ts:22-41
[[nodiscard]] inline std::vector<TriggerPosition> find_keyword_triggers(
    std::string_view text,
    const std::string& keyword_pattern,
    bool case_insensitive = true) {

    std::vector<TriggerPosition> positions;
    if (text.empty() || keyword_pattern.empty()) return positions;

    try {
        auto flags = std::regex::ECMAScript;
        if (case_insensitive) flags |= std::regex::icase;
        // Build word-boundary pattern: \bkeyword\b
        std::string pattern_str = "\\b" + keyword_pattern + "\\b";
        std::regex re(pattern_str, flags);

        std::string s(text);
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            positions.push_back(TriggerPosition{
                static_cast<std::size_t>(m.position()),
                static_cast<std::size_t>(m.position() + m.length())
            });
        }
    } catch (const std::regex_error&) {
        // Invalid regex — return empty (defensive)
    }
    return positions;
}

/// Find "/btw" at the start of the text (case-insensitive, word boundary).
/// TS REF: sideQuestion.ts:16 BTW_PATTERN = /^\/btw\b/gi
[[nodiscard]] inline std::vector<TriggerPosition> find_btw_triggers(
    std::string_view text) {
    // TS pattern: /^\/btw\b/gi — anchored to start, case-insensitive
    std::vector<TriggerPosition> positions;
    if (text.empty()) return positions;

    try {
        std::regex re(R"(^\/btw\b)",
            std::regex::ECMAScript | std::regex::icase);
        std::string s(text);
        std::smatch m;
        if (std::regex_search(s, m, re)) {
            positions.push_back(TriggerPosition{
                static_cast<std::size_t>(m.position()),
                static_cast<std::size_t>(m.position() + m.length())
            });
        }
    } catch (const std::regex_error&) {}
    return positions;
}

/// Find /command patterns in the text: /word at start or after whitespace.
/// TS REF: commandSuggestions.ts:552-567 findSlashCommandPositions
///   regex: /(^|[\s])(\/[a-zA-Z][a-zA-Z0-9:\-_]*)/g
[[nodiscard]] inline std::vector<TriggerPosition> find_slash_command_triggers(
    std::string_view text) {
    std::vector<TriggerPosition> positions;
    if (text.empty()) return positions;

    try {
        std::regex re(R"((^|[\s])(\/[a-zA-Z][a-zA-Z0-9:\-_]*))",
            std::regex::ECMAScript);
        std::string s(text);
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            // m[2] = the /command part
            std::size_t start = static_cast<std::size_t>(
                m.position(2));
            positions.push_back(TriggerPosition{
                start,
                start + static_cast<std::size_t>(m.length(2))
            });
        }
    } catch (const std::regex_error&) {}
    return positions;
}

/// Find token budget shorthand patterns: +500k, +2m, +1b at start or end.
/// TS REF: tokenBudget.ts:1-8 (SHORTHAND_START_RE, SHORTHAND_END_RE)
///   /^\s*\+(\d+(?:\.\d+)?)\s*(k|m|b)\b/i
///   /\s\+(\d+(?:\.\d+)?)\s*(k|m|b)\s*[.!?]?\s*$/i
[[nodiscard]] inline std::vector<TriggerPosition> find_token_budget_triggers(
    std::string_view text) {
    std::vector<TriggerPosition> positions;
    if (text.empty()) return positions;

    try {
        // Start-of-line shorthand: +500k, +2m
        std::regex start_re(R"(^\s*\+(\d+(?:\.\d+)?)\s*(k|m|b)\b)",
            std::regex::ECMAScript | std::regex::icase);
        std::string s(text);
        std::smatch m;
        if (std::regex_search(s, m, start_re)) {
            // Offset to skip leading whitespace (match the +Nxx part)
            std::size_t full_start = static_cast<std::size_t>(m.position());
            std::size_t full_end = static_cast<std::size_t>(
                m.position() + m.length());
            // Trim leading whitespace from the highlight
            std::size_t trimmed_start = full_start;
            while (trimmed_start < full_end &&
                   (s[trimmed_start] == ' ' || s[trimmed_start] == '\t')) {
                ++trimmed_start;
            }
            positions.push_back(TriggerPosition{trimmed_start, full_end});
        }

        // End-of-line shorthand: " +500k"
        std::regex end_re(R"(\s\+(\d+(?:\.\d+)?)\s*(k|m|b)\s*[.!?]?\s*$)",
            std::regex::ECMAScript | std::regex::icase);
        if (std::regex_search(s, m, end_re)) {
            // +1: regex includes leading \s, skip it to get the +Nxx part
            std::size_t start = static_cast<std::size_t>(m.position()) + 1;
            std::size_t end = static_cast<std::size_t>(
                m.position() + m.length());
            // Avoid double-counting if start and end match overlap
            bool already_covered = false;
            for (const auto& existing : positions) {
                if (start >= existing.start && start < existing.end) {
                    already_covered = true;
                    break;
                }
            }
            if (!already_covered) {
                positions.push_back(TriggerPosition{start, end});
            }
        }
    } catch (const std::regex_error&) {}
    return positions;
}

/// Find @name mentions matching known team members.
/// TS REF: PromptInput.tsx:557 regex /(^|\s)@([\w-]+)/g
/// Returns vector of {start, end, color}.
struct MentionHighlight {
    std::size_t start;
    std::size_t end;
    Color color;
};

[[nodiscard]] inline std::vector<MentionHighlight> find_member_mentions(
    std::string_view text,
    const std::vector<std::pair<std::string, Color>>& team_members) {

    std::vector<MentionHighlight> highlights;
    if (text.empty() || team_members.empty()) return highlights;

    try {
        std::regex re(R"((^|\s)@([\w-]+))", std::regex::ECMAScript);
        std::string s(text);
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            std::string name = m[2].str();
            // Find matching team member
            for (const auto& [member_name, member_color] : team_members) {
                if (member_name == name) {
                    std::size_t leading_space = m.length(1);
                    std::size_t name_start = static_cast<std::size_t>(
                        m.position()) + leading_space;
                    std::size_t name_end = name_start +
                        static_cast<std::size_t>(m.length(0) - leading_space);
                    highlights.push_back(
                        MentionHighlight{name_start, name_end, member_color});
                    break;
                }
            }
        }
    } catch (const std::regex_error&) {}
    return highlights;
}

// ============================================================
// Rainbow per-character highlights
// ============================================================

/// Build per-character rainbow highlights for a trigger word range.
/// Each character gets its own TextHighlight with cycling rainbow colors.
/// TS REF: PromptInput.tsx:686-698 (thinkTriggers loop), 700-713 (ultraplan),
///         715-726 (ultrareview), 728-739 (buddy)
inline void add_rainbow_shimmer_highlights(
    std::vector<TextHighlight>& highlights,
    const std::vector<TriggerPosition>& triggers) {

    for (const auto& trigger : triggers) {
        for (std::size_t i = trigger.start; i < trigger.end; ++i) {
            std::size_t char_offset = i - trigger.start;
            highlights.push_back(TextHighlight{
                .start = i,
                .end = i + 1,
                .color = get_rainbow_color(char_offset, false),
                .dim = false,
                .inverse = false,
                .shimmer_color = get_rainbow_color(char_offset, true),
                .priority = highlight_priority::RainbowShimmer
            });
        }
    }
}

// ============================================================
// Main builder: build_combined_highlights()
// ============================================================

/// Assemble the full combined highlights vector from all 8+ sources.
///
/// Priority resolution is handled downstream by segment_text_by_highlights(),
/// which sorts by (start, priority desc) and drops overlaps.  Here we simply
/// push all detected highlights into the vector in source-priority order
/// (matching TS push order is not semantically required since the segmenter
/// re-sorts, but we keep the same order for auditability).
///
/// TS REF: src/components/PromptInput/PromptInput.tsx:601-741
[[nodiscard]] inline std::vector<TextHighlight> build_combined_highlights(
    const CombinedHighlightContext& ctx) {

    std::vector<TextHighlight> highlights;
    highlights.reserve(32);  // pre-allocate for typical input

    auto refs = parse_references(ctx.text);

    // ── 1. Image chip highlights (inverse when cursor at chip.start) ────
    // TS REF: PromptInput.tsx:606-616
    // Invert the [Image #N] chip when the cursor is at chip.start (the
    // "selected" state) so backspace-to-delete is visually obvious.
    for (const auto& ref : refs) {
        // Only Image refs get the chip treatment
        if (ref.match.find("[Image") != 0) continue;

        std::size_t ref_start = ref.index;
        std::size_t ref_end = ref.index + ref.match.size();

        if (ctx.cursor_offset == ref_start) {
            highlights.push_back(TextHighlight{
                .start = ref_start,
                .end = ref_end,
                .color = std::nullopt,
                .dim = false,
                .inverse = true,
                .shimmer_color = std::nullopt,
                .priority = highlight_priority::ImageChip
            });
        }
    }

    // ── 2. History search highlights ────────────────────────────────────
    // TS REF: PromptInput.tsx:617-624
    // When navigating history and a match is found, highlight the query
    // portion of the displayed text.
    if (ctx.is_searching_history && ctx.history_match_found &&
        !ctx.history_failed_match && !ctx.history_query.empty()) {
        std::size_t query_len = ctx.history_query.size();
        highlights.push_back(TextHighlight{
            .start = ctx.cursor_offset,
            .end = ctx.cursor_offset + query_len,
            .color = Color::Yellow,   // TS 'warning' = yellow
            .dim = false,
            .inverse = false,
            .shimmer_color = std::nullopt,
            .priority = highlight_priority::HistorySearch
        });
    }

    // ── 3. Btw trigger highlights (solid yellow) ────────────────────────
    // TS REF: PromptInput.tsx:627-634
    auto btw_triggers = find_btw_triggers(ctx.text);
    for (const auto& trigger : btw_triggers) {
        highlights.push_back(TextHighlight{
            .start = trigger.start,
            .end = trigger.end,
            .color = Color::Yellow,   // TS 'warning' = yellow
            .dim = false,
            .inverse = false,
            .shimmer_color = std::nullopt,
            .priority = highlight_priority::BtwTrigger
        });
    }

    // ── 4. Slash command highlights (blue) ──────────────────────────────
    // TS REF: PromptInput.tsx:637-644
    auto slash_triggers = find_slash_command_triggers(ctx.text);
    for (const auto& trigger : slash_triggers) {
        highlights.push_back(TextHighlight{
            .start = trigger.start,
            .end = trigger.end,
            .color = Color::Blue,     // TS 'suggestion' = blue
            .dim = false,
            .inverse = false,
            .shimmer_color = std::nullopt,
            .priority = highlight_priority::SlashCommand
        });
    }

    // ── 5. Token budget highlights (blue) ───────────────────────────────
    // TS REF: PromptInput.tsx:647-654
    if (ctx.token_budget_enabled) {
        auto budget_triggers = find_token_budget_triggers(ctx.text);
        for (const auto& trigger : budget_triggers) {
            highlights.push_back(TextHighlight{
                .start = trigger.start,
                .end = trigger.end,
                .color = Color::Blue,   // TS 'suggestion' = blue
                .dim = false,
                .inverse = false,
                .shimmer_color = std::nullopt,
                .priority = highlight_priority::TokenBudget
            });
        }
    }

    // ── 6. Member mention highlights (team member color) ─────────────────
    // TS REF: PromptInput.tsx:665-672
    if (ctx.agent_swarms_enabled && !ctx.team_members.empty()) {
        auto mentions = find_member_mentions(ctx.text, ctx.team_members);
        for (const auto& mention : mentions) {
            highlights.push_back(TextHighlight{
                .start = mention.start,
                .end = mention.end,
                .color = mention.color,
                .dim = false,
                .inverse = false,
                .shimmer_color = std::nullopt,
                .priority = highlight_priority::Mention
            });
        }
    }

    // ── 7. Voice interim highlights (dim) ───────────────────────────────
    // TS REF: PromptInput.tsx:675-683
    if (ctx.voice_interim_range) {
        auto [vstart, vend] = *ctx.voice_interim_range;
        highlights.push_back(TextHighlight{
            .start = vstart,
            .end = vend,
            .color = std::nullopt,
            .dim = true,
            .inverse = false,
            .shimmer_color = std::nullopt,
            .priority = highlight_priority::VoiceInterim
        });
    }

    // ── 8. Rainbow shimmer highlights for ultrathink ────────────────────
    // TS REF: PromptInput.tsx:686-698
    if (ctx.ultrathink_enabled) {
        auto think_triggers = find_keyword_triggers(ctx.text, "ultrathink");
        add_rainbow_shimmer_highlights(highlights, think_triggers);
    }

    // ── 9. Rainbow shimmer highlights for ultraplan ─────────────────────
    // TS REF: PromptInput.tsx:700-713
    if (ctx.ultraplan_enabled) {
        auto ultraplan_triggers = find_keyword_triggers(ctx.text, "ultraplan");
        add_rainbow_shimmer_highlights(highlights, ultraplan_triggers);
    }

    // ── 10. Rainbow shimmer highlights for ultrareview ──────────────────
    // TS REF: PromptInput.tsx:715-726
    if (ctx.ultrareview_enabled) {
        auto ultrareview_triggers = find_keyword_triggers(ctx.text, "ultrareview");
        add_rainbow_shimmer_highlights(highlights, ultrareview_triggers);
    }

    // ── 11. Rainbow shimmer highlights for /buddy ───────────────────────
    // TS REF: PromptInput.tsx:728-739
    if (ctx.buddy_enabled) {
        // TS pattern: /\/buddy\b/g
        try {
            std::regex re(R"(\/buddy\b)", std::regex::ECMAScript);
            std::string s(ctx.text);
            auto begin = std::sregex_iterator(s.begin(), s.end(), re);
            auto end = std::sregex_iterator();
            std::vector<TriggerPosition> buddy_triggers;
            for (auto it = begin; it != end; ++it) {
                const auto& m = *it;
                buddy_triggers.push_back(TriggerPosition{
                    static_cast<std::size_t>(m.position()),
                    static_cast<std::size_t>(m.position() + m.length())
                });
            }
            add_rainbow_shimmer_highlights(highlights, buddy_triggers);
        } catch (const std::regex_error&) {}
    }

    return highlights;
}

} // namespace cc::ui::prompt
