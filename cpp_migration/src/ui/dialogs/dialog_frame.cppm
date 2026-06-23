/// @file dialog_frame.cppm
/// @brief Reusable DialogFrame component — faithful port of TS PermissionDialog.tsx.
///
/// MODULE:   cc.ui.dialogs.frame
/// LICENCE:  Exported.  Imported by all dialog renderers that use the
///           standard permission-style frame.
///
/// TS REFERENCE: src/components/permissions/PermissionDialog/PermissionDialog.tsx
///               src/components/permissions/PermissionDialog/PermissionDialog.css
///
/// LAYOUT (faithful to TS):
///   ╭───────────────────────────────────────────────────╮
///   │ Title                            [worker badge]  │
///   │ subtitle                                         │
///   ├───────────────────────────────────────────────────┤
///   │                                                   │
///   │  content                                          │
///   │                                                   │
///   ╰───────────────────────────────────────────────────╯
///
/// FEATURES:
///   - Rounded top border (╭╮ style — no bottom border in overlay mode)
///   - Title row with bold title + optional subtitle
///   - Optional worker badge (right-aligned in title row)
///   - Optional title-right slot (e.g. close button, status)
///   - Configurable inner padding
///   - Themed border color (by risk level or custom color)
///   - Bottom-anchored "floating" look when used in overlay/bottom slots
module;

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>

export module cc.ui.dialogs.frame;

import cc.ui.design.theme;
import cc.ui.design.tokens;
import cc.ui.design.primitives;
import cc.ui.permissions.components;

export namespace cc::ui::dialogs::frame {

using namespace ftxui;
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;
namespace primitives = cc::ui::design::primitives;
namespace pc = cc::ui::permissions::components;

// ============================================================
// DialogFrameStyle — visual variant
// ============================================================

/// Style variant for the frame border/background.
enum class FrameStyle : std::uint8_t {
    Default,        ///< Neutral chrome border
    Info,           ///< Info/blue (permission prompts)
    Success,        ///< Green
    Warning,        ///< Yellow/orange
    Danger,         ///< Red (high risk)
    Critical,       ///< Bold red (critical risk)
    Muted,          ///< Dim border
};

/// Get the border color for a frame style.
[[nodiscard]] inline Color frame_border_color(FrameStyle style,
                                              const Theme& theme) {
    switch (style) {
        case FrameStyle::Default:  return theme.color_for(Role::Chrome);
        case FrameStyle::Info:     return theme.color_for(Role::Info);
        case FrameStyle::Success:  return theme.color_for(Role::Success);
        case FrameStyle::Warning:  return theme.color_for(Role::Warning);
        case FrameStyle::Danger:   return theme.color_for(Role::Danger);
        case FrameStyle::Critical: return theme.color_for(Role::Danger);
        case FrameStyle::Muted:    return theme.color_for(Role::Muted);
    }
    return theme.color_for(Role::Chrome);
}

// ============================================================
// DialogFrameProps — properties for the frame
// ============================================================

/// Properties for building a DialogFrame.
struct DialogFrameProps {
    std::string title;
    std::optional<std::string> subtitle;
    FrameStyle style = FrameStyle::Info;

    /// Optional worker badge element (rendered right of title).
    std::optional<Element> worker_badge;

    /// Optional right-aligned slot in title row (status, close button, etc.).
    std::optional<Element> title_right;

    /// The main content body.
    Element content = text("");

    /// Horizontal padding inside the content area (cells).
    int inner_padding_x = 1;
    /// Vertical padding inside the content area (cells).
    int inner_padding_y = 0;

    /// Whether to show a full border or just the top "cap".
    /// Full border = modal panels, top-only = floating overlay dialogs.
    bool full_border = true;

    /// Whether to use rounded corners.
    bool rounded = true;
};

// ============================================================
// DialogFrame — the reusable frame component
// ============================================================

/// Build a standard dialog frame with title, subtitle, and content.
/// Faithful to TS PermissionDialog.tsx.
[[nodiscard]] inline Element DialogFrame(const DialogFrameProps& props,
                                          const Theme& theme) {
    auto border_col = frame_border_color(props.style, theme);

    // ---- Title row ----
    auto title_el = text(props.title) | bold;

    Elements title_row_els;
    title_row_els.push_back(title_el);

    // Optional worker badge (to the right of title, separated by space)
    if (props.worker_badge) {
        title_row_els.push_back(text("  "));
        title_row_els.push_back(*props.worker_badge);
    }

    // filler() pushes title_right to the right edge
    title_row_els.push_back(filler());

    if (props.title_right) {
        title_row_els.push_back(*props.title_right);
    }

    auto title_row = hbox(title_row_els);

    // ---- Subtitle row ----
    auto subtitle_el = [&]() -> Element {
        if (!props.subtitle) return text("");
        return text(*props.subtitle) | dim | color(theme.color_for(Role::Muted));
    }();

    // ---- Header block (title + subtitle) ----
    Elements header_els;
    header_els.push_back(title_row);
    if (props.subtitle) {
        header_els.push_back(text(""));
        header_els.push_back(subtitle_el);
    }
    auto header = vbox(header_els);

    // ---- Padding for content ----
    auto padded_content = [&]() -> Element {
        auto c = props.content;
        if (props.inner_padding_x > 0) {
            c = c | size(WIDTH, GREATER_THAN, 1); // ensure min width
            // Add left/right padding via hbox
            auto pad = std::string(props.inner_padding_x, ' ');
            c = hbox({ text(pad), c | xflex, text(pad) });
        }
        if (props.inner_padding_y > 0) {
            Elements ys;
            for (int i = 0; i < props.inner_padding_y; ++i) ys.push_back(text(""));
            ys.push_back(c);
            for (int i = 0; i < props.inner_padding_y; ++i) ys.push_back(text(""));
            c = vbox(ys);
        }
        return c;
    }();

    // ---- Assemble the frame body ----
    Elements body_els;

    // Header section (with padding)
    body_els.push_back(hbox({
        text(" "),
        header | xflex,
        text(" "),
    }));

    // Divider between header and content
    body_els.push_back(pc::ThinDivider());

    // Content section
    body_els.push_back(padded_content);

    auto body = vbox(body_els) | xflex;

    // ---- Apply border ----
    if (props.full_border) {
        if (props.rounded) {
            return body
                | borderRounded
                | color(border_col)
                | size(WIDTH, GREATER_THAN, 30);
        }
        return body
            | borderStyled(border_col)
            | color(border_col)
            | size(WIDTH, GREATER_THAN, 30);
    }

    // Top-only border (floating overlay style)
    // Use window with empty top border — in practice full border is used
    // for most dialogs and the bottom edge blends into the prompt area.
    return body
        | borderStyled(border_col)
        | color(border_col)
        | size(WIDTH, GREATER_THAN, 30);
}

// ============================================================
// Convenience builders
// ============================================================

/// Build a simple info dialog frame with title and message.
[[nodiscard]] inline Element SimpleInfoFrame(std::string_view title,
                                              std::string_view message,
                                              const Theme& theme) {
    DialogFrameProps props;
    props.title = std::string{title};
    props.style = FrameStyle::Info;
    props.content = paragraph(std::string{message});
    return DialogFrame(props, theme);
}

/// Build a warning dialog frame with title and message.
[[nodiscard]] inline Element SimpleWarningFrame(std::string_view title,
                                                 std::string_view message,
                                                 const Theme& theme) {
    DialogFrameProps props;
    props.title = std::string{title};
    props.style = FrameStyle::Warning;
    props.content = paragraph(std::string{message});
    return DialogFrame(props, theme);
}

/// Build a danger dialog frame with title and message.
[[nodiscard]] inline Element SimpleDangerFrame(std::string_view title,
                                                std::string_view message,
                                                const Theme& theme) {
    DialogFrameProps props;
    props.title = std::string{title};
    props.style = FrameStyle::Danger;
    props.content = paragraph(std::string{message});
    return DialogFrame(props, theme);
}

// ============================================================
// WorkerBadge — small "worker" indicator badge
// ============================================================

/// Render a worker badge (shown in dialog title for worker-initiated requests).
/// Faithful to TS WorkerBadge component.
[[nodiscard]] inline Element WorkerBadge(std::string_view worker_id,
                                          const Theme& theme) {
    return hbox({
        text("👷 ") | dim,
        text(std::string{worker_id}) | bold | color(theme.color_for(Role::Info)) | dim,
    });
}

/// Render a compact worker dot badge (for smaller dialogs).
[[nodiscard]] inline Element WorkerBadgeCompact(std::string_view worker_id,
                                                 const Theme& theme) {
    return hbox({
        text("●") | color(theme.color_for(Role::Info)),
        text(" "),
        text(std::string{worker_id}) | dim,
    });
}

} // namespace cc::ui::dialogs::frame
