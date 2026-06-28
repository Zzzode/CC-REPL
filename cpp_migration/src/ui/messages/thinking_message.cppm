/// @file thinking_message.cppm
/// @brief Thinking process message rendering - covers both plaintext
/// chain-of-thought (with keyword highlighting, expandable truncation) and
/// redacted/encrypted reasoning (🔐 badge, byte-count display).
///
/// Supersedes the smaller message_redacted_thinking.cppm by exposing both
/// mode (a) plain and mode (b) redacted through a single data model.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <cctype>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.thinking_message;

import cc.types.types;

export namespace cc::ui::messages::thinking_message {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// State of the thinking process
enum class ThinkingState : std::uint8_t {
    Active,     // Currently thinking (streaming)
    Paused,     // Paused/waiting for tool result
    Complete,   // Thinking finished
};

/// Rendering mode for the thinking block
enum class ThinkingMode : std::uint8_t {
    Plain,      ///< Normal chain-of-thought text
    Redacted,   ///< Redacted / encrypted reasoning with lock badge
};

/// A section within the thinking content (plain mode)
struct ThinkingSection {
    std::string title;          // Optional section heading
    std::string content;        // The thinking text
    bool is_key_insight = false; // Highlight as important
};

/// Data for a thinking message
struct ThinkingMessageData {
    ThinkingMode mode{ThinkingMode::Plain};
    ThinkingState state{ThinkingState::Complete};

    // --- Plain mode fields ---
    std::vector<ThinkingSection> sections;
    std::string raw_text;                   // Full raw thinking text

    // --- Redacted mode fields ---
    std::string redacted_reason;            // Why redacted (policy / safety / etc.)
    std::optional<std::size_t> encrypted_bytes; // Encrypted payload size
    bool redacted_show_placeholder{true};   // Show "filtered" placeholder body

    // --- Shared ---
    std::chrono::milliseconds duration{0};  // Time spent thinking
    int token_count = 0;                    // Tokens used for thinking
    bool is_collapsed = true;               // Default collapsed
    int budget_remaining_pct = 100;         // Extended thinking budget
    std::size_t total_chars = 0;            // Total chars (used for size badge)
};

/// Options for the thinking message component
struct ThinkingMessageOptions {
    ThinkingMessageData data;
    int max_visible_lines = 15;             // N-line truncation
    bool keyword_highlight = true;          // Numbers / TODO / etc. simple color
    bool show_token_count = true;
    bool animate_streaming = true;
    std::function<void()> on_toggle;
    std::function<void()> on_copy_text;     // 'c' copies full plaintext
};

// ============================================================
// Keyword Highlight (simple — no NLP)
// ============================================================

namespace detail {

/// Return true if c is part of an identifier boundary
inline bool is_id_boundary(char c) {
    return std::isspace(static_cast<unsigned char>(c)) ||
           std::ispunct(static_cast<unsigned char>(c)) || c == 0;
}

/// Scan a single line into Elements with simple keyword/token coloring.
inline Elements highlight_line_tokens(const std::string& line) {
    Elements out;
    std::size_t i = 0;
    const auto n = line.size();
    std::string buf;

    auto flush_plain = [&] {
        if (!buf.empty()) {
            out.push_back(text(buf));
            buf.clear();
        }
    };

    while (i < n) {
        char c = line[i];

        // --- Number literal ---
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && i + 1 < n &&
             std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            std::size_t j = i;
            if (c == '-') ++j;
            while (j < n && (std::isdigit(static_cast<unsigned char>(line[j])) ||
                             line[j] == '.' || line[j] == '_' ||
                             line[j] == 'x' || line[j] == 'X' ||
                             std::isxdigit(static_cast<unsigned char>(line[j])))) {
                ++j;
            }
            // Require digit boundary to avoid eating into identifiers.
            if (j < n && !is_id_boundary(line[j])) {
                buf.push_back(c);
                ++i;
                continue;
            }
            flush_plain();
            out.push_back(text(line.substr(i, j - i)) | color(Color::Blue));
            i = j;
            continue;
        }

        // --- Uppercase / CONSTANT like tokens ---
        if (std::isalpha(static_cast<unsigned char>(c)) &&
            std::isupper(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            bool has_lower = false;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) ||
                             line[j] == '_')) {
                if (std::islower(static_cast<unsigned char>(line[j]))) has_lower = true;
                ++j;
            }
            std::string word = line.substr(i, j - i);
            // Only treat as constant if entirely upper/digit/underscore and len>=3
            if (!has_lower && word.size() >= 3 && is_id_boundary(j < n ? line[j] : '\0')) {
                flush_plain();
                out.push_back(text(word) | color(Color::Magenta) | bold);
                i = j;
                continue;
            }
            // TODO / FIXME / NOTE / XXX keyword family (any case-insensitive prefix)
            std::string upper;
            upper.reserve(word.size());
            for (char w : word) upper.push_back(static_cast<char>(
                std::toupper(static_cast<unsigned char>(w))));
            if (upper == "TODO" || upper == "FIXME" || upper == "NOTE" ||
                upper == "XXX"  || upper == "HACK"  || upper == "BUG") {
                flush_plain();
                out.push_back(text(word) | color(Color::Yellow) | bold);
                i = j;
                continue;
            }
        }

        // Plain char buffer
        buf.push_back(c);
        ++i;
    }
    flush_plain();
    return out;
}

/// Wrap highlighted line tokens with style — indented, dim, graylight.
inline Element render_thinking_line(const std::string& line, bool highlight,
                                    bool is_key_insight) {
    if (line.empty()) return text("");
    Elements line_parts;
    if (highlight) {
        line_parts = highlight_line_tokens(line);
    } else {
        line_parts = {text(line)};
    }
    auto el = hbox(line_parts) | dim;
    if (is_key_insight) {
        el = el | color(Color::Yellow);
    } else {
        el = el | color(Color::GrayLight);
    }
    return el;
}

inline std::size_t count_lines(const std::string& s) {
    if (s.empty()) return 0;
    std::size_t n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

} // namespace detail

// ============================================================
// Rendering — header (shared)
// ============================================================

/// Spinner frames for active thinking
[[nodiscard]] inline std::string thinking_spinner(int frame) {
    static constexpr const char* frames[] = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"
    };
    return frames[((frame % 8) + 8) % 8];
}

/// Byte formatting
[[nodiscard]] inline std::string format_bytes(std::size_t bytes) {
    if (bytes >= 1024ULL * 1024) {
        return std::format("{:.1f}MB", bytes / (1024.0 * 1024.0));
    }
    if (bytes >= 1024) {
        return std::format("{:.1f}KB", bytes / 1024.0);
    }
    return std::format("{}B", bytes);
}

/// Render the thinking message header (works for both modes)
[[nodiscard]] inline Element RenderThinkingHeader(const ThinkingMessageData& data,
                                                   int frame) {
    Elements parts;

    if (data.mode == ThinkingMode::Redacted) {
        // --- Redacted header: lock icon + "Encrypted reasoning" ---
        parts.push_back(text("🔒 ") | color(Color::Yellow));
        parts.push_back(text("Encrypted reasoning") | color(Color::Yellow) | bold);

        if (data.encrypted_bytes) {
            parts.push_back(text(std::format(" · {}", format_bytes(*data.encrypted_bytes)))
                            | dim);
        }
        if (!data.redacted_reason.empty()) {
            parts.push_back(text(" — " + data.redacted_reason)
                            | color(Color::GrayDark) | dim);
        }
        // Trailing token/duration
        if (data.duration.count() > 0) {
            std::string dur = (data.duration.count() >= 1000)
                ? std::format(" ({:.1f}s)", data.duration.count() / 1000.0)
                : std::format(" ({}ms)", data.duration.count());
            parts.push_back(text(dur) | dim);
        }
        if (data.token_count > 0) {
            parts.push_back(filler());
            parts.push_back(text(std::format("{} tokens", data.token_count))
                            | color(Color::GrayDark) | dim);
        }
        return hbox(parts);
    }

    // --- Plain mode header ---
    switch (data.state) {
        case ThinkingState::Active:
            parts.push_back(text(thinking_spinner(frame) + " ") | color(Color::Cyan));
            parts.push_back(text("Thinking") | color(Color::Cyan) | bold);
            parts.push_back(text("...") | color(Color::Cyan) | dim | blink);
            break;
        case ThinkingState::Paused:
            parts.push_back(text("⏸ ") | color(Color::Yellow));
            parts.push_back(text("Thinking (paused)") | color(Color::Yellow));
            break;
        case ThinkingState::Complete:
            parts.push_back(text("💭 ") | dim);
            parts.push_back(text("Reasoning") | color(Color::GrayLight));
            break;
    }

    // Duration
    if (data.duration.count() > 0) {
        std::string dur = (data.duration.count() >= 1000)
            ? std::format(" ({:.1f}s)", data.duration.count() / 1000.0)
            : std::format(" ({}ms)", data.duration.count());
        parts.push_back(text(dur) | dim);
    }

    // Token count
    if (data.token_count > 0) {
        parts.push_back(filler());
        parts.push_back(text(std::format("{} tokens", data.token_count))
                        | color(Color::GrayDark) | dim);
    }

    return hbox(parts);
}

// ============================================================
// Rendering — body: plain mode with keyword highlight + N-line truncation
// ============================================================

/// Render the thinking content body (plain mode)
[[nodiscard]] inline Element RenderThinkingBody(
    const ThinkingMessageData& data, int max_lines, bool highlight) {

    if (data.is_collapsed) {
        // Show summary only
        std::string summary;
        if (!data.raw_text.empty()) {
            summary = data.raw_text.substr(0, 80);
            if (data.raw_text.size() > 80) summary += "...";
        } else {
            summary = "(thinking content hidden)";
        }
        return hbox({
            text("  ▸ ") | color(Color::GrayDark),
            text(summary) | dim | color(Color::GrayLight),
        });
    }

    // Expanded view
    Elements elements;
    int lines_shown = 0;
    bool truncated = false;
    int remaining_total = 0;

    auto split_and_emit = [&](const std::string& content, int indent,
                              bool is_key_insight, int limit) -> int {
        // returns remaining lines not rendered
        int rendered = 0;
        int total = 0;
        // First pass: total lines
        size_t p = 0;
        while (p < content.size()) {
            auto nl = content.find('\n', p);
            ++total;
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        p = 0;
        while (p < content.size() && rendered < limit) {
            auto nl = content.find('\n', p);
            std::string line;
            if (nl == std::string::npos) {
                line = content.substr(p);
                p = content.size();
            } else {
                line = content.substr(p, nl - p);
                p = nl + 1;
            }
            std::string prefix(indent, ' ');
            auto line_el = detail::render_thinking_line(line, highlight, is_key_insight);
            elements.push_back(hbox({text(prefix), line_el}));
            ++rendered;
        }
        return total - rendered;
    };

    if (!data.sections.empty()) {
        // Render structured sections
        for (const auto& section : data.sections) {
            if (!section.title.empty()) {
                auto title_el = text("  ■ " + section.title)
                    | bold | color(section.is_key_insight ? Color::Yellow : Color::White);
                elements.push_back(title_el);
            }
            int limit = std::max(1, max_lines - lines_shown);
            if (limit <= 0) { truncated = true; break; }
            int remaining = split_and_emit(section.content, 4,
                                           section.is_key_insight, limit);
            lines_shown += (limit - remaining);
            if (remaining > 0) {
                truncated = true;
                remaining_total += remaining;
            }
            elements.push_back(text(""));
        }
    } else if (!data.raw_text.empty()) {
        int limit = max_lines;
        int remaining = split_and_emit(data.raw_text, 2, false, limit);
        lines_shown += (limit - remaining);
        if (remaining > 0) {
            truncated = true;
            remaining_total += remaining;
        }
    }

    if (truncated) {
        elements.push_back(
            text(std::format("  ... {} more lines (press e to expand fully)",
                             remaining_total))
            | dim | color(Color::GrayDark));
    }

    return vbox(elements);
}

// ============================================================
// Rendering — body: redacted mode
// ============================================================

/// Render the redacted-mode body
[[nodiscard]] inline Element RenderRedactedBody(const ThinkingMessageData& data) {
    if (!data.redacted_show_placeholder) {
        return text("");
    }
    Elements rows;
    rows.push_back(text("  This thinking content has been encrypted and cannot "
                        "be displayed.")
                   | dim | color(Color::GrayDark));

    std::string extra;
    if (data.encrypted_bytes) {
        extra = std::format("Payload size: {} · ", format_bytes(*data.encrypted_bytes));
    }
    if (!data.redacted_reason.empty()) {
        extra += "Reason: " + data.redacted_reason;
    } else {
        extra += "Reason: policy filter";
    }
    rows.push_back(text("  " + extra) | dim | color(Color::GrayDark));

    // Static "🔒 Encrypted reasoning" badge
    auto badge = hbox({
        text("  [ ") | dim,
        text("🔐 Encrypted reasoning") | color(Color::Yellow) | bold,
        text(" ]") | dim,
    });
    rows.insert(rows.begin(), badge);
    return vbox(rows);
}

// ============================================================
// Top-level renderer
// ============================================================

/// Render a complete thinking message (plain or redacted mode)
[[nodiscard]] inline Element RenderThinkingMessage(
    const ThinkingMessageOptions& opts, int frame = 0) {

    const auto& data = opts.data;
    auto header = RenderThinkingHeader(data, frame);

    Elements content;
    content.push_back(header);

    // Budget indicator for extended thinking (plain/active only)
    if (data.mode == ThinkingMode::Plain &&
        data.state == ThinkingState::Active &&
        data.budget_remaining_pct < 100) {
        double progress = (100 - data.budget_remaining_pct) / 100.0;
        content.push_back(hbox({
            text("  Budget: ") | dim,
            gauge(progress) | color(Color::Cyan) | flex,
            text(std::format(" {}%", data.budget_remaining_pct)) | dim,
        }));
    }

    Element body = (data.mode == ThinkingMode::Redacted)
        ? RenderRedactedBody(data)
        : RenderThinkingBody(data, opts.max_visible_lines, opts.keyword_highlight);

    content.push_back(body);

    // Toggle hint (complete plain mode only)
    if (data.mode == ThinkingMode::Plain &&
        data.state == ThinkingState::Complete) {
        auto hint_text = data.is_collapsed ? "▸ Expand [enter]" : "▾ Collapse [enter]";
        Elements hints = {
            text(std::string("  ") + hint_text) | dim | color(Color::GrayDark),
            filler(),
            text("[c] copy ") | dim | color(Color::GrayDark),
            text("[e] expand all ") | dim | color(Color::GrayDark),
        };
        content.push_back(hbox(hints));
    }

    Color border_color = (data.mode == ThinkingMode::Redacted)
        ? Color::Yellow
        : Color::GrayDark;
    return vbox(content) | borderLight | color(border_color);
}

// ============================================================
// M4: Faithful TS renderer (AssistantThinkingMessage.tsx)
// ============================================================
//
// TS renders a MINIMAL thinking block (not the bordered/collapsing panel
// above).  Two states:
//
//   collapsed (not transcript, not verbose):
//     <Box marginTop={addMargin?1:0}>
//       <Text dimColor italic>∴ Thinking <CtrlOToExpand/></Text>
//     </Box>
//
//   expanded (transcript or verbose):
//     <Box flexDirection="column" gap={1} marginTop={addMargin?1:0} width="100%">
//       <Text dimColor italic>∴ Thinking…</Text>
//       <Box paddingLeft={2}><Markdown dimColor>{thinking}</Markdown></Box>
//     </Box>
//
// Label glyph is U+2234 "∴" (THEREFORE).  No header decoration, no spinner,
// no token count, no border, no budget bar, no toggle hints — all of those
// belong to the richer divergent panel above (kept for the interactive UI).

/// The TS thinking label glyph (U+2234 "∴" THEREFORE).
inline constexpr std::string_view kThinkingLabel = "\xE2\x88\xB4";  // ∴

/// CtrlOToExpand hint text rendered after the collapsed label.
inline constexpr std::string_view kCtrlOHint = " (ctrl+o to expand)";

/// Faithful collapsed-state render:  `∴ Thinking (ctrl+o to expand)` dim italic.
/// Matches TS exactly — there is NO inline preview of the thinking content in
/// collapsed mode; the body only appears in expanded (transcript/verbose) mode.
[[nodiscard]] inline Element RenderThinkingMessageCollapsed(
    std::string_view /*thinking*/, bool add_margin) {
    Elements line_parts;
    line_parts.push_back(text(std::string(kThinkingLabel)));
    line_parts.push_back(text(" Thinking"));
    line_parts.push_back(text(std::string(kCtrlOHint)));
    Element label = hbox(std::move(line_parts))
        | dim | color(Color::GrayLight);
    // FTXUI has no true italic; dim+gray approximates the dimColor+italic look.
    if (add_margin) return vbox({text(""), std::move(label)});
    return label;
}

/// Faithful expanded-state render:  `∴ Thinking…` label + indented dim body.
/// `body` is the caller-supplied rendered thinking content (M5 wires Markdown;
/// M4 passes plain dim text).  Indented paddingLeft=2 per TS.
[[nodiscard]] inline Element RenderThinkingMessageExpanded(
    const std::string& thinking, bool add_margin) {
    Element label = hbox({
        text(std::string(kThinkingLabel)),
        text(" Thinking…"),
    }) | dim | color(Color::GrayLight);

    // Body: indented 2, dim.  Plain-text fallback (M5 swaps in Markdown).
    Elements bl;
    {
        std::size_t s = 0;
        while (s < thinking.size()) {
            auto nl = thinking.find('\n', s);
            std::string line = (nl == std::string::npos) ? thinking.substr(s)
                                                          : thinking.substr(s, nl - s);
            bl.push_back(text(std::move(line)) | dim | color(Color::GrayLight));
            if (nl == std::string::npos) break;
            s = nl + 1;
        }
    }
    Element body = vbox(std::move(bl));

    Element inner = vbox({
        std::move(label),
        hbox({text("  "), std::move(body)}),
    });
    if (add_margin) return vbox({text(""), std::move(inner)});
    return inner;
}

/// Top-level faithful dispatcher mirroring AssistantThinkingMessage:
/// shouldShowFullThinking = isTranscriptMode || verbose  → expanded; else
/// collapsed.  Empty thinking → empty element (TS returns null).
[[nodiscard]] inline Element RenderThinkingMessageFaithful(
    const ThinkingMessageData& data, bool is_transcript_mode, bool verbose,
    bool add_margin = true) {
    if (data.raw_text.empty() && data.sections.empty()) {
        return text("");
    }
    // Build full thinking string for both branches.
    std::string thinking = data.raw_text;
    if (thinking.empty() && !data.sections.empty()) {
        for (const auto& s : data.sections) {
            if (!thinking.empty()) thinking.push_back('\n');
            thinking += s.content;
        }
    }
    const bool show_full = is_transcript_mode || verbose;
    if (!show_full) {
        return RenderThinkingMessageCollapsed(thinking, add_margin);
    }
    return RenderThinkingMessageExpanded(thinking, add_margin);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive thinking message component
[[nodiscard]] inline Component ThinkingMessage(ThinkingMessageOptions options) {
    struct State {
        ThinkingMessageOptions opts;
        int frame = 0;
        bool full_expand = false;    // e key: ignore N-line truncation
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(options);

    return Renderer([state] {
        auto& o = state->opts;
        if (state->full_expand && !o.data.is_collapsed) {
            // Use a huge effective line limit
            auto copy = o;
            copy.max_visible_lines = 1'000'000;
            if (o.data.state == ThinkingState::Active) state->frame++;
            return RenderThinkingMessage(copy, state->frame);
        }
        if (o.data.state == ThinkingState::Active) state->frame++;
        return RenderThinkingMessage(o, state->frame);
    }) | CatchEvent([state](Event event) -> bool {
        auto& d = state->opts.data;
        auto& o = state->opts;

        // Toggle collapsed (Enter / space)
        if (event == Event::Return || event == Event::Character(' ')) {
            d.is_collapsed = !d.is_collapsed;
            if (!d.is_collapsed) state->full_expand = false;
            if (o.on_toggle) o.on_toggle();
            return true;
        }

        // 'e' — expand fully (ignore line limit)
        if (event == Event::Character('e') || event == Event::Character('E')) {
            if (!d.is_collapsed) {
                state->full_expand = !state->full_expand;
                return true;
            }
        }

        // 'c' — copy full text (fires callback; clipboard copy is host job)
        if (event == Event::Character('c') || event == Event::Character('C')) {
            if (d.mode == ThinkingMode::Plain && !d.raw_text.empty()) {
                if (o.on_copy_text) o.on_copy_text();
                return true;
            }
        }

        return false;
    });
}

} // namespace cc::ui::messages::thinking_message
