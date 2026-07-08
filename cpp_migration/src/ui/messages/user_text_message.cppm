/// @file user_text_message.cppm
/// @brief FTXUI component for user text messages (UserTextMessage.tsx)
///
/// Visual layout:
///   ┌── Right-aligned (user side)
///   │  ┌──────────────────────────────┐
///   │  │ [content, selectable]   ⏱ HH:MM │
///   │  │ [quoted reply if any]     👤 You│
///   │  └──────────────────────────────┘
///   └──
///
/// Interactions:
///   - Click on message -> toggle selection highlight
///   - Ctrl+C / copy button -> copy content to clipboard (placeholder callback)
// ────────────────────────────────────────────────────────────────────────
module;

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.user_text_message;

import cc.ui.messages.message_timestamp;
// P0-1: Unified prompt / user-message glyph source (TS figures.pointer).
// Eliminates the local `kFiguresPointer` duplicate that diverged from the
// prompt prefix's UTF-8 byte sequence in CPP Round 1-6.
import cc.ui.design.figures;

export namespace cc::ui::messages {

using namespace ftxui;

// ─── Input data ────────────────────────────────────────────────────────

/// Plain data input, no globals.
struct UserTextMessageData {
    std::string content;
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();
    std::optional<std::string> quoted_reply;   ///< Text being replied to (>)
    bool is_transcript_mode = false;           ///< Dimmed presentation
    std::optional<std::string> command_name;   ///< "/commit" etc. shown as chip
};

// ─── User-prompt text truncation ────────────────────────────────────────
// TS REF: src/components/messages/UserPromptMessage.tsx lines 28-70.
// Hard-caps display text at 10_000 chars: head 2500 + separator + tail 2500.
// Critical for perf: pasting a 100 KB file into the prompt would otherwise
// force FTXUI to lay out thousands of lines on every keystroke.
namespace truncate_detail {
inline constexpr std::size_t kMaxDisplayChars = 10'000;
inline constexpr std::size_t kTruncateHeadChars = 2'500;
inline constexpr std::size_t kTruncateTailChars = 2'500;

/// Count occurrences of `ch` in `text` starting at byte position `start`.
/// TS REF: src/utils/stringUtils.ts countCharInString (lines 54-66).
[[nodiscard]] inline std::size_t CountCharFrom(std::string_view text,
                                                char ch,
                                                std::size_t start) {
    std::size_t count = 0;
    std::size_t pos = start;
    while ((pos = text.find(ch, pos)) != std::string_view::npos) {
        ++count;
        ++pos;
    }
    return count;
}
}  // namespace truncate_detail

/// Truncate long user prompt text for display.  Returns the original text
/// unchanged if <= 10_000 chars.  Otherwise returns head 2500 + separator +
/// tail 2500, where the separator shows the count of hidden lines:
///   "\n… +N lines …\n"
/// (U+2026 HORIZONTAL ELLIPSIS, UTF-8 \xe2\x80\xa6).
///
/// TS REF: UserPromptMessage.tsx lines 64-70 (useMemo displayText).
[[nodiscard]] inline std::string TruncateUserPromptText(std::string_view text) {
    using namespace truncate_detail;
    if (text.size() <= kMaxDisplayChars) return std::string(text);

    std::string_view head = text.substr(0, kTruncateHeadChars);
    std::string_view tail = text.substr(text.size() - kTruncateTailChars);

    // Newlines in the hidden region (from head end to text end), minus
    // newlines already visible in the tail slice.
    const std::size_t hidden_lines =
        truncate_detail::CountCharFrom(text, '\n', truncate_detail::kTruncateHeadChars) -
        truncate_detail::CountCharFrom(tail, '\n', 0);

    std::string result;
    result.reserve(truncate_detail::kTruncateHeadChars + truncate_detail::kTruncateTailChars + 40);
    result.append(head);
    result += "\n\xe2\x80\xa6 +";       // "\n… +"
    result += std::to_string(hidden_lines);
    result += " lines \xe2\x80\xa6\n";  // " lines …\n"
    result.append(tail);
    return result;
}

// ─── Text wrapping for FTXUI ───────────────────────────────────────────
// FTXUI's text() does NOT auto-wrap, and paragraphAlignLeft only wraps at
// word boundaries.  For long unbroken strings (base64, minified code, or
// the truncation head/tail of a paste with no spaces), we need manual
// character-level wrapping.
namespace wrap_detail {
inline constexpr std::size_t kPromptWrapWidth = 78;  // 80 - 2 for "❯ " prefix

/// Wrap `text` to at most `width` display columns, breaking mid-codepoint
/// if necessary (with UTF-8 safety).  Returns individual wrapped lines.
[[nodiscard]] inline std::vector<std::string> WrapToWidth(std::string_view text,
                                                           std::size_t width) {
    std::vector<std::string> lines;
    if (width == 0) width = 1;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = std::min(pos + width, text.size());
        // UTF-8 safety: don't split a multi-byte sequence.
        // Continuation bytes are 0x80..0xBF.
        while (end < text.size() &&
               (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80 &&
               end > pos) {
            --end;
        }
        lines.emplace_back(text.substr(pos, end - pos));
        pos = end;
    }
    if (lines.empty()) lines.emplace_back("");
    return lines;
}
}  // namespace wrap_detail

// ─── Component ─────────────────────────────────────────────────────────

class UserTextMessageComponent : public ComponentBase {
  public:
    using OnCopyFn = std::function<void(std::string_view)>;
    using OnClickFn = std::function<void()>;

    explicit UserTextMessageComponent(UserTextMessageData data,
                                      OnCopyFn on_copy = nullptr,
                                      OnClickFn on_click = nullptr)
        : data_(std::move(data)),
          on_copy_(std::move(on_copy)),
          on_click_(std::move(on_click)) {}

    Element Render() override {
        auto bubble_body = BuildContent();

        // Right-side header: timestamp + role tag
        auto header_right = hbox({
            text(" ") | size(WIDTH, EQUAL, 1),
            text(render_timestamp(data_.timestamp)) | dim | color(Color::GrayDark),
            text(" 👤 You") | color(Color::Blue),
        });

        // User message is right-aligned. We indent from the left so that
        // the content visually hugs the right edge.
        return vbox({
            hbox({
                filler(),
                header_right,
            }),
            hbox({
                filler(),
                vbox(std::move(bubble_body))
                    | (hovered_ ? bgcolor(Color::DarkBlue) : nothing)
                    | flex_shrink,
            }),
        });
    }

    bool OnEvent(Event event) override {
        if (event == Event::Return) {
            hovered_ = !hovered_;
            if (on_click_) on_click_();
            return true;
        }
        // Custom copy accelerator
        if (event == Event::Special({3})) {  // Ctrl+C convention
            if (on_copy_) on_copy_(data_.content);
            return true;
        }
        if (event.is_mouse() && event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Released) {
            hovered_ = !hovered_;
            if (on_click_) on_click_();
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    auto BuildContent() -> Elements {
        Elements out;

        // Quoted reply block (indented, dimmed)
        if (data_.quoted_reply && !data_.quoted_reply->empty()) {
            auto lines = SplitLines(*data_.quoted_reply);
            for (auto& line : lines) {
                out.push_back(hbox({
                    text("│ ") | color(Color::GrayDark),
                    text(std::move(line)) | dim,
                }));
            }
            out.push_back(separator() | dim);
        }

        // Command chip
        if (data_.command_name) {
            out.push_back(hbox({
                text(" $ ") | bold | color(Color::Green),
                text(*data_.command_name) | bold,
            }));
        }

        // Main body — truncated for long pastes (TS REF: UserPromptMessage.tsx
        // lines 64-70).  SplitLines handles both natural \n in user input and
        // the truncation separator "\n… +N lines …\n".
        std::string display_body = TruncateUserPromptText(data_.content);
        auto lines = SplitLines(display_body);
        for (auto& line : lines) {
            out.push_back(text(std::move(line))
                              | color(Color::White)
                              | (data_.is_transcript_mode ? dim : nothing));
        }

        return out;
    }

    static auto SplitLines(std::string_view text) -> std::vector<std::string> {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < text.size()) {
            auto nl = text.find('\n', start);
            if (nl == std::string_view::npos) {
                lines.emplace_back(text.substr(start));
                break;
            }
            lines.emplace_back(text.substr(start, nl - start));
            start = nl + 1;
        }
        if (lines.empty()) lines.emplace_back("");
        return lines;
    }

    UserTextMessageData data_;
    OnCopyFn on_copy_;
    OnClickFn on_click_;
    bool hovered_ = false;
};

/// Factory function.
[[nodiscard]] inline Component MakeUserTextMessage(UserTextMessageData data,
                                                   UserTextMessageComponent::OnCopyFn on_copy = nullptr,
                                                   UserTextMessageComponent::OnClickFn on_click = nullptr) {
    return Make<UserTextMessageComponent>(std::move(data), std::move(on_copy), std::move(on_click));
}

/// Convenience stateless element renderer (for transcript / no-interaction).
[[nodiscard]] inline Element RenderUserTextMessageBubble(const UserTextMessageData& data) {
    Elements body;
    body.push_back(text(TruncateUserPromptText(data.content)));
    return vbox({
        hbox({
            filler(),
            text(render_timestamp(data.timestamp)) | dim,
            text(" 👤") | color(Color::Blue),
        }),
        hbox({ filler(), vbox(std::move(body)) }),
    });
}

// ─── M4: Faithful TS renderers ─────────────────────────────────────────
// Mirrors UserPromptMessage.tsx + HighlightedThinkingText.tsx (the non-brief
// layout path).  TS renders:
//   <Box flexDirection="column" marginTop={addMargin?1:0}
//        backgroundColor={isSelected?messageActionsBackground:userMessageBackground}
//        paddingRight={1}>
//     <HighlightedThinkingText text={displayText} .../>
//   </Box>
// and HighlightedThinkingText (no triggers) emits a single <Text> row:
//   <Text color={subtle}>{figures.pointer} </Text><Text color="text">{text}</Text>
// figures.pointer is U+276F "❯".  No timestamp / role label in the non-brief
// path — that only appears in the brief/chat layout.
//
// NOTE: The glyph itself lives in cc.ui.design.figures::kPointer — the
// authoritative source used by the prompt prefix, user messages, and plugin
// manager.  Use `namespace figs = cc::ui::design::figures;` below.

/// Faithful render of a user prompt message (UserPromptMessage.tsx ->
/// HighlightedThinkingText non-brief path).  Full-width, left-aligned, with a
/// `❯` (subtle) prefix followed by the prompt text.  is_selected swaps the
/// prefix + bg color to the TS `messageActionsBackground` / `suggestion` tokens.
///
/// `add_margin` (TS REF: marginTop={addMargin ? 1 : 0}) controls the blank
/// separator line above the message.  In TS this is `!hasMetadata`; since
/// hasMetadata is always false for user messages (it requires type==="assistant"),
/// add_margin is effectively always true in normal REPL use.  We still thread
/// the parameter for faithfulness and transcript-mode correctness.
///
/// FTXUI NOTES:
///   - `text()` does NOT auto-wrap (unlike Ink <Text>).
///   - `paragraphAlignLeft()` only wraps at word boundaries; long unbroken
///     strings (base64, minified code) would overflow the screen.
///   - We use manual character-level wrapping via WrapToWidth() to guarantee
///     all content is visible within the terminal width.
[[nodiscard]] inline Element RenderUserPromptMessage(const UserTextMessageData& data,
                                                     bool is_selected = false,
                                                     bool add_margin = true) {
    // TS palette tokens (dark mode, from src/utils/theme.ts):
    //   subtle                      = rgb( 80, 80, 80)  — pointer glyph prefix
    //   suggestion                  = rgb(177,185,249)  — pointer glyph (selected)
    //   text                        = rgb(250,250,252)  — body foreground (default)
    //   userMessageBackground       = rgb( 55, 55, 55)  — bubble background
    //   messageActionsBackground    = rgb( 44, 50, 62)  — bubble bg (selected)
    const Color kText        = Color::RGB(250, 250, 252);
    const Color kUserBg      = Color::RGB( 55,  55,  55);
    const Color kUserBgSel   = Color::RGB( 44,  50,  62);
    const Color kSubtle      = Color::RGB( 80,  80,  80);
    const Color kSuggestion  = Color::RGB(177, 185, 249);

    const Color prefix_color  = is_selected ? kSuggestion : kSubtle;
    const Color bg            = is_selected ? kUserBgSel : kUserBg;
    const Decorator body_style =
        data.is_transcript_mode ? (dim | color(kText)) : color(kText);
    const Decorator prefix_style =
        data.is_transcript_mode ? (dim | color(prefix_color)) : color(prefix_color);

    // Truncate long text (TS REF: UserPromptMessage.tsx lines 64-70).
    // Result may contain \n separators from the truncation or natural user
    // input newlines.
    std::string display_text = TruncateUserPromptText(data.content);

    // Split by \n to get logical lines (handles truncation separator + any
    // natural newlines in user input).
    std::vector<std::string_view> logical_lines;
    {
        std::size_t start = 0;
        while (start < display_text.size()) {
            auto nl = display_text.find('\n', start);
            if (nl == std::string::npos) {
                logical_lines.emplace_back(display_text.data() + start,
                                           display_text.size() - start);
                break;
            }
            logical_lines.emplace_back(display_text.data() + start, nl - start);
            start = nl + 1;
        }
        if (logical_lines.empty()) logical_lines.emplace_back("");
    }

    // Wrap each logical line and build visual line elements.
    // Available width = terminal - prefix(2) - paddingRight(1) - 1 safety = 76.
    // But we use kPromptWrapWidth=78 and let the trailing space handle overflow.
    const std::size_t wrap_w = wrap_detail::kPromptWrapWidth - 2;  // -2 for "❯ "

    Elements visual_lines;
    bool first_line = true;
    for (auto logical_line : logical_lines) {
        auto wrapped = wrap_detail::WrapToWidth(logical_line, wrap_w);
        for (auto& wline : wrapped) {
            Elements row;
            if (first_line) {
                row.push_back(text(std::string(cc::ui::design::figures::kPointer))
                              | prefix_style);
                row.push_back(text(" ") | prefix_style);
            } else {
                // Indent continuation lines to align with the prefix column.
                row.push_back(text("  ") | prefix_style);
            }
            row.push_back(text(std::move(wline)) | body_style);
            // paddingRight={1} — trailing space so bg reaches the right edge.
            row.push_back(text(" ") | color(kText));

            Element line_el = hbox(std::move(row));
            if (!data.is_transcript_mode) {
                // Full-width background: flex on content + trailing bg spacer.
                line_el = hbox({line_el | bgcolor(bg) | flex,
                                text(" ") | bgcolor(bg)});
            }
            visual_lines.push_back(std::move(line_el));
            first_line = false;
        }
    }

    Element body = vbox(std::move(visual_lines));

    if (data.is_transcript_mode) {
        Element content = hbox({std::move(body), filler()});
        if (add_margin) return vbox({text(""), std::move(content)});
        return content;
    }

    if (add_margin) return vbox({text(""), std::move(body)});
    // TS PARITY: continuation within same user turn (e.g. after an image),
    // <MessageResponse> prepends "  ⎿  " (U+23BF connector).
    return hbox({
        text("  \xe2\x8e\xbf  ") | dim,
        std::move(body),
    });
}

/// Faithful render of a slash-command user message (UserCommandMessage.tsx):
///   <Box marginTop={addMargin?1:0} backgroundColor="userMessageBackground" paddingRight={1}>
///     <Text><Text color="subtle">{figures.pointer} </Text>
///           <Text color="text">/{command args}</Text></Text>
///   </Box>
///
/// `add_margin` (TS REF: marginTop={addMargin ? 1 : 0}) controls the blank
/// separator line above the message.  Threaded for faithfulness; always true
/// in normal REPL use for the same reason as RenderUserPromptMessage.
[[nodiscard]] inline Element RenderUserCommandMessage(const UserTextMessageData& data,
                                                      bool is_selected = false,
                                                      bool add_margin = true) {
    (void)is_selected;  // TS: selection never affects the command chip tint.
    // TS palette tokens (dark mode, src/utils/theme.ts):
    //   subtle                      = rgb( 80, 80, 80)  — pointer glyph prefix
    //   text                        = rgb(255,255,255)  — body foreground
    //   userMessageBackground       = rgb( 55, 55, 55)  — chip background
    // NOTE: TS B5 UserCommandMessage ALWAYS renders backgroundColor=
    // userMessageBackground — no bgcolor swap on selection. Selection affects
    // BLACK_CIRCLE color in assistant rows and the isSelected context, NOT
    // this chip's tint (unlike UserPromptMessage which swaps to messageAct-
    // ionsBackground). Hence bg = kUserBg regardless of is_selected.
    const Color kUserBg       = Color::RGB( 55,  55,  55);
    const Color kSubtle       = Color::RGB( 80,  80,  80);
    // TS UserCommandMessage.tsx B5: the ❯ prefix always uses the "subtle"
    // token (rgb(80,80,80)).  Selection only affects the BLACK_CIRCLE color
    // of the assistant row, never the user command chip tint — unlike
    // UserPromptMessage which swaps both the prefix and the background.
    const Color prefix_color = kSubtle;

    std::string body = data.command_name
        ? ("/" + *data.command_name)
        : data.content;
    // Faithful foregrounds (match theme.text + theme.subtle tokens exactly).
    // Explicit colors avoid FTXUI default-fg drift when wrapped in bgcolor().
    const Color kText = Color::RGB(250, 250, 252);
    // TS: paddingRight=1 only — no flex, so the chip collapses to content
    // width instead of stretching to terminal width (F8 compact chip).
    Element row = hbox({
        text(std::string(cc::ui::design::figures::kPointer)) | color(prefix_color),
        text(" ") | color(prefix_color),
        text(body) | color(kText),
        text(" ") | color(kText),  // paddingRight=1
    });
    if (data.is_transcript_mode) {
        if (add_margin) return vbox({text(""), row});
        return row;
    }
    // Wrap row in a single bgcolor cell that is exactly row-width (no flex).
    Element chip = hbox({row}) | bgcolor(kUserBg);
    Element content = hbox({chip, filler()});
    if (add_margin) return vbox({text(""), std::move(content)});
    return content;
}

}  // namespace cc::ui::messages
