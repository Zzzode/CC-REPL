/// @file ide_dialogs.cppm
/// @brief IDE integration dialogs — auto-connect prompt, onboarding, status indicator.
/// Migrated from IdeAutoConnectDialog.tsx, IdeOnboardingDialog.tsx, IdeStatusIndicator.tsx.
module;

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <expected>
#include <algorithm>
#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.ide_dialogs;

export namespace cc::ui::ide_dialogs {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// IDE connection state
enum class IdeConnectionState {
    disconnected,
    connecting,
    connected,
    error,
};

/// Supported IDE types
enum class IdeType {
    vscode,
    jetbrains,
    vim_neovim,
    unknown,
};

/// Convert IDE type to display name
[[nodiscard]] inline std::string ide_name(IdeType ide) {
    switch (ide) {
        case IdeType::vscode: return "VS Code";
        case IdeType::jetbrains: return "JetBrains";
        case IdeType::vim_neovim: return "Vim/Neovim";
        case IdeType::unknown: return "Unknown IDE";
    }
    return "?";
}

/// Auto-connect dialog decision
enum class AutoConnectDecision {
    yes,
    no,
};

/// Props for the IDE auto-connect dialog
struct IdeAutoConnectDialogProps {
    std::function<void()> on_complete;
    IdeType detected_ide = IdeType::unknown;
};

/// Props for the IDE onboarding dialog
struct IdeOnboardingDialogProps {
    std::function<void()> on_complete;
    IdeType ide = IdeType::unknown;
    std::string terminal_name;
};

/// Props for the IDE status indicator
struct IdeStatusIndicatorProps {
    IdeConnectionState state = IdeConnectionState::disconnected;
    IdeType ide = IdeType::unknown;
    std::optional<std::string> error_message;
};

// ============================================================
// Element Rendering
// ============================================================

/// Render connection state indicator dot
[[nodiscard]] inline Element RenderIdeStateIndicator(IdeConnectionState state) {
    switch (state) {
        case IdeConnectionState::connected:
            return text("\u25CF") | color(Color::Green);
        case IdeConnectionState::connecting:
            return text("\u25CB") | color(Color::Yellow);
        case IdeConnectionState::disconnected:
            return text("\u25CB") | color(Color::GrayDark);
        case IdeConnectionState::error:
            return text("\u25CF") | color(Color::Red);
    }
    return text("?");
}

/// Render the IDE status indicator (inline for status bar)
[[nodiscard]] inline Element RenderIdeStatusIndicator(
    const IdeStatusIndicatorProps& props) {

    auto indicator = RenderIdeStateIndicator(props.state);
    auto label = text(" IDE");

    Elements parts = {indicator, label};

    if (props.state == IdeConnectionState::connected) {
        parts.push_back(text(": " + ide_name(props.ide)) | dim);
    } else if (props.state == IdeConnectionState::error && props.error_message) {
        parts.push_back(text(" \u2717") | color(Color::Red));
    }

    return hbox(parts);
}

/// Render the auto-connect dialog element
[[nodiscard]] inline Element RenderAutoConnectDialog(
    IdeType detected_ide, int selected) {

    auto make_option = [](std::string_view label, bool is_selected) {
        auto prefix = is_selected
            ? text("\u276F ") | color(Color::Cyan)
            : text("  ");
        auto el = text(std::string{label});
        if (is_selected) el = el | bold;
        return hbox({prefix, el});
    };

    auto content = vbox({
        paragraph("We detected a supported IDE terminal. Would you like "
                  "Claude Code to automatically connect to your IDE for "
                  "enhanced features?"),
        text(""),
        hbox({
            text("  Detected: "),
            text(ide_name(detected_ide)) | bold | color(Color::Cyan),
        }),
        text(""),
        make_option("Yes", selected == 0),
        make_option("No", selected == 1),
        text(""),
        text("You can also configure this in /config or with the --ide flag") | dim,
    });

    return window(
        text(" IDE Auto-Connect ") | bold | color(Color::Blue),
        content
    ) | color(Color::Blue) | size(WIDTH, LESS_THAN, 70);
}

/// Render the IDE onboarding dialog element
[[nodiscard]] inline Element RenderOnboardingDialog(
    const IdeOnboardingDialogProps& props, int selected) {

    auto make_option = [](std::string_view label, bool is_selected) {
        auto prefix = is_selected
            ? text("\u276F ") | color(Color::Cyan)
            : text("  ");
        auto el = text(std::string{label});
        if (is_selected) el = el | bold;
        return hbox({prefix, el});
    };

    Elements content_items = {
        text("IDE Integration Setup") | bold,
        text(""),
        paragraph("Claude Code can integrate with your IDE to provide "
                  "enhanced features like jump-to-definition, apply edits "
                  "directly, and synchronized file navigation."),
        text(""),
    };

    if (!props.terminal_name.empty()) {
        content_items.push_back(hbox({
            text("  Terminal: "),
            text(props.terminal_name) | color(Color::Cyan),
        }));
        content_items.push_back(text(""));
    }

    content_items.push_back(text("Would you like to enable IDE integration?"));
    content_items.push_back(text(""));
    content_items.push_back(make_option("Yes, enable IDE integration", selected == 0));
    content_items.push_back(make_option("No, skip for now", selected == 1));
    content_items.push_back(text(""));
    content_items.push_back(
        text("You can change this later with /config") | dim);

    return window(
        text(" IDE Onboarding ") | bold | color(Color::Blue),
        vbox(content_items)
    ) | color(Color::Blue) | size(WIDTH, LESS_THAN, 70);
}

// ============================================================
// Interactive Components
// ============================================================

/// Create the IDE auto-connect dialog component
[[nodiscard]] inline Component IdeAutoConnectDialog(
    IdeAutoConnectDialogProps props) {

    struct State {
        IdeAutoConnectDialogProps props;
        int selected = 0;
    };
    constexpr int kAutoConnectOptionCount = 2;

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderAutoConnectDialog(
            state->props.detected_ide, state->selected);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = std::max(0, state->selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected = std::min(kAutoConnectOptionCount - 1,
                                       state->selected + 1);
            return true;
        }
        if (event == Event::Return) {
            // Both selections complete the dialog
            // Caller reads selected to determine auto_connect preference
            if (state->props.on_complete) state->props.on_complete();
            return true;
        }
        if (event == Event::Escape) {
            // Treat escape as "No"
            state->selected = 1;
            if (state->props.on_complete) state->props.on_complete();
            return true;
        }
        return false;
    });
}

/// Create the IDE onboarding dialog component
[[nodiscard]] inline Component IdeOnboardingDialog(
    IdeOnboardingDialogProps props) {

    struct State {
        IdeOnboardingDialogProps props;
        int selected = 0;
    };
    constexpr int kOnboardingOptionCount = 2;

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderOnboardingDialog(state->props, state->selected);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = std::max(0, state->selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected = std::min(kOnboardingOptionCount - 1,
                                       state->selected + 1);
            return true;
        }
        if (event == Event::Return) {
            if (state->props.on_complete) state->props.on_complete();
            return true;
        }
        if (event == Event::Escape) {
            state->selected = 1;
            if (state->props.on_complete) state->props.on_complete();
            return true;
        }
        return false;
    });
}

/// Create a standalone IDE status indicator (non-interactive element renderer)
[[nodiscard]] inline Component IdeStatusIndicator(
    IdeStatusIndicatorProps props) {

    auto shared_props = std::make_shared<IdeStatusIndicatorProps>(std::move(props));
    return Renderer([shared_props] {
        return RenderIdeStatusIndicator(*shared_props);
    });
}

} // namespace cc::ui::ide_dialogs
