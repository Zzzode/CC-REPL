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

        // Main body
        auto lines = SplitLines(data_.content);
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
    body.push_back(text(data.content));
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

/// The figures.pointer glyph used as the user-message prefix (matches TS).
inline constexpr std::string_view kFiguresPointer = "\xE2\x9D\xAF";  // ❯ U+276F

/// Faithful render of a user prompt message (UserPromptMessage.tsx ->
/// HighlightedThinkingText non-brief path).  Full-width, left-aligned, with a
/// `❯` (subtle) prefix followed by the prompt text.  is_selected swaps the
/// prefix + bg color to the TS `messageActionsBackground` / `suggestion` tokens.
[[nodiscard]] inline Element RenderUserPromptMessage(const UserTextMessageData& data,
                                                     bool is_selected = false) {
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

    Elements row;
    // Prefix "❯ " — subtle unless selected (suggestion/lavender).
    const Color prefix_color  = is_selected ? kSuggestion : kSubtle;
    const Decorator prefix_style =
        data.is_transcript_mode ? (dim | color(prefix_color))
                                : color(prefix_color);
    row.push_back(text(std::string(kFiguresPointer)) | prefix_style);
    row.push_back(text(" ") | prefix_style);
    // Body text — "text" color (EXPLICIT: FTXUI does not auto-inherit
    // theme.text from Ink; wrapping a hbox in bgcolor() leaves default
    // terminal fg which some terminals render as a washed-out blue/grey
    // instead of the pure white TS uses).
    Decorator body_style = data.is_transcript_mode
        ? (dim | color(kText))
        : color(kText);
    row.push_back(text(data.content) | body_style);
    // paddingRight={1} → trailing space column, painted with bg so the tint
    // reaches the right edge.
    row.push_back(text(" ") | color(kText));

    Element inner = hbox(std::move(row));
    if (data.is_transcript_mode) {
        return vbox({text(""), hbox({inner, filler()})});
    }
    const Color bg = is_selected ? kUserBgSel : kUserBg;
    return vbox({
        text(""),
        hbox({inner | bgcolor(bg) | flex, text(" ") | bgcolor(bg)}),
    });
}

/// Faithful render of a slash-command user message (UserCommandMessage.tsx):
///   <Box marginTop={addMargin?1:0} backgroundColor="userMessageBackground" paddingRight={1}>
///     <Text><Text color="subtle">{figures.pointer} </Text>
///           <Text color="text">/{command args}</Text></Text>
///   </Box>
[[nodiscard]] inline Element RenderUserCommandMessage(const UserTextMessageData& data,
                                                      bool is_selected = false) {
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
        text(std::string(kFiguresPointer)) | color(prefix_color),
        text(" ") | color(prefix_color),
        text(body) | color(kText),
        text(" ") | color(kText),  // paddingRight=1
    });
    if (data.is_transcript_mode) return vbox({text(""), row});
    // Wrap row in a single bgcolor cell that is exactly row-width (no flex).
    Element chip = hbox({row}) | bgcolor(kUserBg);
    return vbox({
        text(""),
        hbox({chip, filler()}),  // left-align the chip, filler to end of row
    });
}

}  // namespace cc::ui::messages
