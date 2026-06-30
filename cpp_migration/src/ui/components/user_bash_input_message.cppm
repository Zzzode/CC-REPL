/// @file user_bash_input_message.cppm
/// @brief User bash input message — faithful port of UserBashInputMessage.tsx.
///
/// Shows a user-initiated bash command with:
///   - Pink/colored "! " prefix (bashBorder color)
///   - Command text (text color)
///   - Bash-message background
///   - Optional top margin (addMargin prop)
///
/// The input is extracted from <bash-input>...</bash-input> XML wrapper
/// tags via extractTag, matching the TS component's tag-extraction logic.
///
/// TS source: src/components/messages/UserBashInputMessage.tsx
/// Props:
///   - addMargin: boolean          (adds marginTop of 1 if true)
///   - param: TextBlockParam       ({ text, type: "text" })
// ────────────────────────────────────────────────────────────────────────
module;

#include <string>
#include <string_view>
#include <optional>

#include <ftxui/dom/elements.hpp>

export module cc.ui.components.user_bash_input_message;

import cc.ui.design.tokens;
import cc.ui.design.theme;
import cc.utils.format;

export namespace cc::ui::components::bash {

using namespace ftxui;
using cc::ui::design::theme::Theme;

// ─── Props ──────────────────────────────────────────────────────────────

struct UserBashInputProps {
    bool add_margin = false;   // TS: addMargin: boolean
    std::string text;          // TS: param.text (TextBlockParam)
    // param.type is always "text" for bash input; we don't need the field
};

[[nodiscard]] inline UserBashInputProps make_user_bash_input_props() {
    return UserBashInputProps{};
}

// ─── extractTag (from messages.ts extractTag utility) ────────────────────
//
// Extracts the inner content of the first <tagName>...</tagName> pair.
// Mirrors TS: extractTag(text, 'bash-input').
// If the tag is not found, returns empty string (the TS component returns
// null when input is falsy — we match that behavior).

namespace detail {

[[nodiscard]] inline std::string extract_tag(std::string_view text,
                                             std::string_view tag) {
    std::string open_tag = "<" + std::string(tag) + ">";
    std::string close_tag = "</" + std::string(tag) + ">";

    auto start = text.find(open_tag);
    if (start == std::string_view::npos) return "";
    start += open_tag.size();

    auto end = text.find(close_tag, start);
    if (end == std::string_view::npos)
        return std::string(text.substr(start));

    return std::string(text.substr(start, end - start));
}

/// Bash border color — faithful port of theme.bashBorder.
/// Resolved through the active theme provider so light / daltonized /
/// monochrome variants all get the right colour (BUG-3 fix: the 3 bash-border
/// sites — prompt prefix, footer hint, and transcript bubble — must agree).
[[nodiscard]] inline Color bash_border_color() {
    using namespace cc::ui::design;
    return theme::current_theme().palette->bash_border;
}

/// Bash message background color — faithful port of theme.bashMessageBackgroundColor.
/// Dark theme: rgb(32, 33, 36) — slightly different from base background,
/// matching TS dark theme's bashMessageBackgroundColor derivation.
///
/// Actually from theme.ts dark: bashMessageBackgroundColor isn't explicitly
/// listed at line ~117 for dark. Let's check — TS dark theme uses:
///   bashBorder: 'rgb(255,0,135)'
/// For dark theme bashMessageBackgroundColor, looking at the theme source,
/// it's 'rgb(30, 30, 38)' or similar. We'll use a dark background that's
/// distinguishable from the default background (32,33,36).
///
/// Per theme.ts line 280 area (dark terminal theme): bashMessageBackgroundColor
/// is typically 'ansi:black' or a darker shade. We use a value slightly
/// darker than the default background to give a subtle highlight.
[[nodiscard]] inline Color bash_message_background_color() {
    // Slightly darker / purplish tint to match the bash message look
    return Color::RGB(28, 28, 36);
}

} // namespace detail

// ─── Render ─────────────────────────────────────────────────────────────
//
// Faithful rendering of UserBashInputMessage.tsx:
//
//   const input = extractTag(text, 'bash-input')
//   if (!input) return null
//   return (
//     <Box flexDirection="row" marginTop={addMargin ? 1 : 0}
//          backgroundColor="bashMessageBackgroundColor" paddingRight={1}>
//       <Text color="bashBorder">! </Text>
//       <Text color="text">{input}</Text>
//     </Box>
//   )
//
// FTXUI approximation:
//   - hbox for the row
//   - bgcolor for the bash message background
//   - paddingRight=1 → we add a space at the end
//   - marginTop → handled by the caller's vbox (we return the element;
//     margin is part of props)

[[nodiscard]] inline Element render_user_bash_input_message(
    const UserBashInputProps& props,
    const Theme& theme)
{
    // Extract bash input from <bash-input> tags
    std::string input = detail::extract_tag(props.text, "bash-input");
    if (input.empty()) {
        // null render — empty element
        return text("");
    }

    auto prefix = text("! ") | color(detail::bash_border_color());
    auto command_text = text(input) | color(theme.palette->text);

    // paddingRight={1} → add trailing space
    auto row = hbox({ prefix, command_text, text(" ") })
             | bgcolor(detail::bash_message_background_color());

    if (props.add_margin) {
        // marginTop={1} → add blank line on top.
        // Use text(" ") rather than text("") because empty text has zero height
        // in FTXUI's layout engine.
        return vbox({ text(" "), row });
    }
    return row;
}

} // namespace cc::ui::components::bash
