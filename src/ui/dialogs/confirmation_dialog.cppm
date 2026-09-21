/// @file confirmation_dialog.cppm
/// @brief Faithful Confirmation dialog — reusable yes/no/cancel dialog
///        using DialogFrame.  Port of TS ConfirmDialog pattern.
///
/// MODULE:   cc.ui.dialogs.confirmation
/// LICENCE:  Exported.  Imported by any code that needs a confirmation.
///
/// TS REFERENCE:
///   src/components/ConfirmDialog/ConfirmDialog.tsx
///   src/utils/confirm.tsx (confirm() helper)
///
/// VISUAL STRUCTURE (faithful to TS):
///   +- Confirm Action ---------------------------------+
///   |  Are you sure you want to do this?               |
///   |                                                  |
///   |  This action cannot be undone.                   |
///   |                                                  |
///   |       [ Yes ]   [ No ]   [ Cancel ]              |
///   |                                                  |
///   +--------------------------------------------------+
///
/// KEYBOARD:
///   Enter / y    — confirm (Yes)
///   n            — No
///   Esc / c      — Cancel
///   Tab / Arrow  — move focus between buttons
module;

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

export module cc.ui.dialogs.confirmation;

import cc.ui.dialogs.frame;
import cc.ui.design.theme;
import cc.ui.design.tokens;

export namespace cc::ui::dialogs::confirmation {

using namespace ftxui;
namespace dframe = cc::ui::dialogs::frame;
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;

// ============================================================
// Types
// ============================================================

/// Confirmation dialog result.
enum class ConfirmResult : std::uint8_t {
    Yes,     ///< User confirmed (Yes / Enter / y)
    No,      ///< User declined (No / n)
    Cancel,  ///< User cancelled (Esc / c)
};

/// Confirmation dialog style variant.
enum class ConfirmStyle : std::uint8_t {
    Info,     ///< Neutral permission/default style (default — maps to FrameStyle::Permission)
    Warning,  ///< Warning / attention style
    Danger,   ///< Destructive / high-risk style
    Success,  ///< Positive / confirm-success style
    Error,    ///< Alias of Danger, matches TS `color="error"` (e.g. Sandbox Bypass)
};

/// Button labels for the confirmation dialog.
struct ConfirmButtons {
    std::string yes = "Yes";
    std::string no = "No";
    std::optional<std::string> cancel = "Cancel";
};

/// Confirmation dialog properties.
struct ConfirmDialogProps {
    std::string title = "Confirm";
    std::string message;
    std::optional<std::string> detail;  ///< additional detail text (dim)
    ConfirmStyle style = ConfirmStyle::Info;
    ConfirmButtons buttons;
    /// Default button index: 0=Yes, 1=No, 2=Cancel
    int default_button = 0;
    /// Callback invoked when user makes a choice.
    std::function<void(ConfirmResult)> on_result;
};

/// Mutable state for the interactive confirmation dialog.
struct ConfirmDialogState {
    int focused_button = 0;  // 0=Yes, 1=No, 2=Cancel
};

// ============================================================
// Helpers
// ============================================================

namespace detail {

/// Map ConfirmStyle to FrameStyle.
[[nodiscard]] inline dframe::FrameStyle to_frame_style(ConfirmStyle style) {
    switch (style) {
        case ConfirmStyle::Info:    return dframe::FrameStyle::Permission;
        case ConfirmStyle::Warning: return dframe::FrameStyle::Warning;
        case ConfirmStyle::Danger:  return dframe::FrameStyle::Danger;
        case ConfirmStyle::Success: return dframe::FrameStyle::Success;
        case ConfirmStyle::Error:   return dframe::FrameStyle::Error;
    }
    return dframe::FrameStyle::Permission;
}

/// Get the accent color for a style.
[[nodiscard]] inline Color accent_color(ConfirmStyle style, const Theme& theme) {
    switch (style) {
        case ConfirmStyle::Info:    return theme.color_for(Role::Info);
        case ConfirmStyle::Warning: return theme.color_for(Role::Warning);
        case ConfirmStyle::Danger:  return theme.color_for(Role::Danger);
        case ConfirmStyle::Success: return theme.color_for(Role::Success);
        case ConfirmStyle::Error:   return theme.color_for(Role::Danger);
    }
    return theme.color_for(Role::Info);
}

/// Render a single button element.
[[nodiscard]] inline Element RenderButton(const std::string& label,
                                            bool is_focused,
                                            bool is_default,
                                            Color accent,
                                            bool is_destructive = false) {
    auto btn = hbox({
        text(" "),
        text(label) | bold,
        text(" "),
    });

    if (is_focused) {
        btn = btn | inverted | focus;
    }

    if (is_default && !is_focused) {
        btn = btn | borderStyled(accent) | color(accent);
    } else if (!is_focused) {
        Color border_col = is_destructive ? Color::Red : Color::GrayDark;
        btn = btn | borderStyled(border_col);
    }

    return btn;
}

/// Render the button row.
[[nodiscard]] inline Element RenderButtonRow(
    const ConfirmButtons& buttons,
    int focused_index,
    int default_button,
    ConfirmStyle style,
    const Theme& theme)
{
    auto accent = accent_color(style, theme);
    bool has_cancel = buttons.cancel.has_value();

    Elements btn_els;

    // Yes button (index 0)
    btn_els.push_back(RenderButton(buttons.yes, focused_index == 0,
                                     default_button == 0, accent, false));

    btn_els.push_back(text("  "));

    // No button (index 1)
    btn_els.push_back(RenderButton(buttons.no, focused_index == 1,
                                     default_button == 1, accent, true));

    if (has_cancel) {
        btn_els.push_back(text("  "));
        // Cancel button (index 2)
        btn_els.push_back(RenderButton(*buttons.cancel, focused_index == 2,
                                         default_button == 2, accent, false));
    }

    return hbox(std::move(btn_els)) | center;
}

/// Count visible buttons.
[[nodiscard]] inline int button_count(const ConfirmButtons& buttons) {
    return buttons.cancel.has_value() ? 3 : 2;
}

} // namespace detail

// ============================================================
// Render functions
// ============================================================

/// Render the confirmation dialog (pure Element version).
/// Faithful to TS ConfirmDialog visual structure.
[[nodiscard]] inline Element RenderConfirmDialog(
    const ConfirmDialogProps& props,
    const ConfirmDialogState& state,
    const Theme& theme)
{
    using namespace detail;

    auto frame_style = to_frame_style(props.style);

    // Build content
    Elements content_els;

    // Main message
    content_els.push_back(text(props.message) | center);

    // Detail text (optional)
    if (props.detail && !props.detail->empty()) {
        content_els.push_back(text(""));
        content_els.push_back(text(*props.detail) | dim | center);
    }

    content_els.push_back(text(""));

    // Button row
    content_els.push_back(RenderButtonRow(
        props.buttons, state.focused_button, props.default_button,
        props.style, theme));

    // Keyboard hint
    content_els.push_back(text(""));
    content_els.push_back(
        text("Enter = confirm, Tab = move, Esc = cancel") | dim | center);

    auto content = vbox(std::move(content_els));

    // Wrap in DialogFrame
    dframe::DialogFrameProps frame_props;
    frame_props.title = props.title;
    frame_props.style = frame_style;
    frame_props.content = content;
    frame_props.full_border = true;
    frame_props.inner_padding_x = 2;
    frame_props.inner_padding_y = 1;
    frame_props.pane_variant = dframe::PaneVariant::ModalMinimal;

    return dframe::DialogFrame(frame_props, theme);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive ConfirmDialog component.
/// Faithful to TS ConfirmDialog behavior.
[[nodiscard]] inline Component MakeConfirmDialog(
    std::shared_ptr<ConfirmDialogState> state,
    ConfirmDialogProps props,
    const Theme& theme)
{
    using namespace detail;

    int num_buttons = button_count(props.buttons);

    auto renderer = Renderer([state, props = std::move(props), &theme]() -> Element {
        return RenderConfirmDialog(props, *state, theme);
    });

    auto with_events = CatchEvent([state, props = std::move(props),
                                     num_buttons](Event event) -> bool {
        // Navigation
        if (event == Event::Tab || event == Event::ArrowRight) {
            state->focused_button = (state->focused_button + 1) % num_buttons;
            return true;
        }
        if (event == Event::TabReverse || event == Event::ArrowLeft) {
            state->focused_button =
                (state->focused_button - 1 + num_buttons) % num_buttons;
            return true;
        }

        // Confirm (Enter / Space)
        if (event == Event::Return || event == Event::Character(' ')) {
            if (props.on_result) {
                ConfirmResult result = ConfirmResult::Yes;
                if (state->focused_button == 1) result = ConfirmResult::No;
                else if (state->focused_button == 2) result = ConfirmResult::Cancel;
                props.on_result(result);
            }
            return true;
        }

        // Shortcut: Y = Yes
        if (event == Event::Character('y') || event == Event::Character('Y')) {
            if (props.on_result) props.on_result(ConfirmResult::Yes);
            return true;
        }

        // Shortcut: N = No
        if (event == Event::Character('n') || event == Event::Character('N')) {
            if (props.on_result) props.on_result(ConfirmResult::No);
            return true;
        }

        // Shortcut: C / Esc = Cancel (if cancel button exists)
        if (event == Event::Escape) {
            if (props.on_result) props.on_result(ConfirmResult::Cancel);
            return true;
        }
        if (event == Event::Character('c') || event == Event::Character('C')) {
            if (props.buttons.cancel && props.on_result) {
                props.on_result(ConfirmResult::Cancel);
            }
            return true;
        }

        return false;
    });

    return renderer | with_events;
}

// ============================================================
// Convenience builders
// ============================================================

/// Show a simple yes/no confirmation dialog (blocking, standalone).
/// Returns the user's choice.
inline ConfirmResult show_confirm_dialog(
    ScreenInteractive& screen,
    std::string_view title,
    std::string_view message,
    ConfirmStyle style = ConfirmStyle::Info,
    bool show_cancel = true)
{
    auto state = std::make_shared<ConfirmDialogState>();
    ConfirmResult result = ConfirmResult::Cancel;

    ConfirmDialogProps props;
    props.title = std::string{title};
    props.message = std::string{message};
    props.style = style;
    if (!show_cancel) props.buttons.cancel = std::nullopt;
    props.on_result = [&](ConfirmResult r) {
        result = r;
        screen.ExitLoopClosure()();
    };

    auto theme = cc::ui::design::theme::current_theme();
    auto component = MakeConfirmDialog(std::move(state), std::move(props), theme);
    screen.Loop(component);

    return result;
}

/// Show a destructive confirmation (red style, No as default).
inline ConfirmResult show_danger_confirm(
    ScreenInteractive& screen,
    std::string_view title,
    std::string_view message,
    std::optional<std::string_view> detail = std::nullopt)
{
    auto state = std::make_shared<ConfirmDialogState>();
    state->focused_button = 1;  // No is default for destructive actions

    ConfirmResult result = ConfirmResult::Cancel;

    ConfirmDialogProps props;
    props.title = std::string{title};
    props.message = std::string{message};
    if (detail) props.detail = std::string{*detail};
    props.style = ConfirmStyle::Danger;
    props.default_button = 1;  // No is default
    props.on_result = [&](ConfirmResult r) {
        result = r;
        screen.ExitLoopClosure()();
    };

    auto theme = cc::ui::design::theme::current_theme();
    auto component = MakeConfirmDialog(std::move(state), std::move(props), theme);
    screen.Loop(component);

    return result;
}

} // namespace cc::ui::dialogs::confirmation
