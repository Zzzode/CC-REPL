/// @file wizard_dialog.cppm
/// @brief Multi-step wizard framework — provider, layout, navigation.
/// Migrated from components/wizard/ (WizardProvider.tsx, WizardDialogLayout.tsx,
/// WizardNavigationFooter.tsx).
module;

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <expected>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <any>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.wizard_dialog;

export namespace cc::ui::wizard_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// A single wizard step definition
struct WizardStep {
    std::string id;
    std::string title;
    std::string description;
    /// The component factory for this step's content.
    /// Takes the wizard context, returns a renderable component.
    std::function<Component()> create_content;
};

/// Navigation direction
enum class WizardNavDirection {
    next,
    back,
    skip,
    cancel,
    complete,
};

/// Wizard state accessible to step components
struct WizardState {
    int current_step_index = 0;
    int total_steps = 0;
    std::string title;
    bool show_step_counter = true;
    std::vector<int> navigation_history;
    bool is_completed = false;
};

/// Theme color identifier for the wizard dialog
enum class WizardColor {
    suggestion,
    permission,
    warning,
    info,
};

/// Convert wizard color to FTXUI color
[[nodiscard]] inline Color to_ftxui_color(WizardColor c) {
    switch (c) {
        case WizardColor::suggestion: return Color::Cyan;
        case WizardColor::permission: return Color::Magenta;
        case WizardColor::warning: return Color::Yellow;
        case WizardColor::info: return Color::Blue;
    }
    return Color::White;
}

/// Props for the wizard provider/container
struct WizardProviderProps {
    std::vector<WizardStep> steps;
    std::string title = "Wizard";
    bool show_step_counter = true;
    std::function<void()> on_complete;
    std::function<void()> on_cancel;
};

/// Props for the wizard dialog layout
struct WizardDialogLayoutProps {
    std::optional<std::string> title_override;
    WizardColor color = WizardColor::suggestion;
    std::optional<std::string> subtitle;
    std::optional<std::string> footer_text;
};

/// Navigation footer display options
struct WizardNavigationFooterProps {
    bool can_go_back = false;
    bool can_go_next = true;
    bool can_skip = false;
    std::optional<std::string> next_label;
    std::optional<std::string> back_label;
    std::optional<std::string> instructions;
};

// ============================================================
// Element Rendering
// ============================================================

/// Render the navigation footer
[[nodiscard]] inline Element RenderWizardNavigationFooter(
    const WizardNavigationFooterProps& props) {

    Elements hints;

    if (props.can_go_back) {
        auto label = props.back_label.value_or("Back");
        hints.push_back(hbox({
            text("Esc") | color(Color::Cyan) | bold,
            text(": " + label + "  "),
        }));
    }

    if (props.can_go_next) {
        auto label = props.next_label.value_or("Next");
        hints.push_back(hbox({
            text("Enter") | color(Color::Cyan) | bold,
            text(": " + label + "  "),
        }));
    }

    if (props.can_skip) {
        hints.push_back(hbox({
            text("Tab") | color(Color::Cyan) | bold,
            text(": Skip  "),
        }));
    }

    Elements result;
    if (props.instructions) {
        result.push_back(text(*props.instructions) | dim);
        result.push_back(text("  "));
    }
    result.push_back(hbox(hints) | dim);

    return hbox(result);
}

/// Render the step counter label
[[nodiscard]] inline std::string step_counter_label(int current, int total) {
    return std::format(" ({}/{})", current + 1, total);
}

/// Render the wizard dialog layout wrapping step content
[[nodiscard]] inline Element RenderWizardDialogLayout(
    const WizardDialogLayoutProps& layout_props,
    const WizardState& wizard_state,
    Element step_content,
    const WizardNavigationFooterProps& nav_props) {

    // Build title with optional step counter
    std::string title = layout_props.title_override.value_or(wizard_state.title);
    if (title.empty()) title = "Wizard";
    if (wizard_state.show_step_counter) {
        title += step_counter_label(
            wizard_state.current_step_index, wizard_state.total_steps);
    }

    auto title_color = to_ftxui_color(layout_props.color);

    // Header
    Elements header_items;
    header_items.push_back(text(" " + title + " ") | bold | color(title_color));
    if (layout_props.subtitle) {
        header_items.push_back(text("  " + *layout_props.subtitle) | dim);
    }

    auto footer = RenderWizardNavigationFooter(nav_props);

    auto body = vbox({
        hbox(header_items),
        separator() | color(title_color),
        step_content | flex,
        separator() | dim,
        footer,
    });

    return body | border | color(title_color);
}

/// Render a placeholder step (for steps without content)
[[nodiscard]] inline Element RenderPlaceholderStep(
    const WizardStep& step) {
    return vbox({
        text("  " + step.title) | bold,
        text(""),
        text("  " + step.description) | dim,
    });
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the wizard provider component that manages multi-step navigation
[[nodiscard]] inline Component WizardComponent(WizardProviderProps props) {
    struct InternalState {
        WizardProviderProps props;
        WizardState wizard_state;
        WizardDialogLayoutProps layout;
        WizardNavigationFooterProps nav;
    };

    auto state = std::make_shared<InternalState>();
    state->props = std::move(props);
    state->wizard_state.total_steps = static_cast<int>(state->props.steps.size());
    state->wizard_state.title = state->props.title;
    state->wizard_state.show_step_counter = state->props.show_step_counter;

    // Update navigation hints based on current position
    auto update_nav = [](InternalState& s) {
        s.nav.can_go_back = (s.wizard_state.current_step_index > 0);
        s.nav.can_go_next = (s.wizard_state.current_step_index
                             < s.wizard_state.total_steps - 1);
        if (s.wizard_state.current_step_index == s.wizard_state.total_steps - 1) {
            s.nav.next_label = "Complete";
            s.nav.can_go_next = true;
        } else {
            s.nav.next_label = "Next";
        }
    };
    update_nav(*state);

    return Renderer([state] {
        Element step_content;
        if (state->props.steps.empty()) {
            step_content = text("  No steps defined") | dim;
        } else {
            auto idx = std::min(
                state->wizard_state.current_step_index,
                static_cast<int>(state->props.steps.size()) - 1);
            const auto& step = state->props.steps[idx];
            if (step.create_content) {
                // In a real implementation, this would render the component
                step_content = RenderPlaceholderStep(step);
            } else {
                step_content = RenderPlaceholderStep(step);
            }
        }

        return RenderWizardDialogLayout(
            state->layout, state->wizard_state, step_content, state->nav);
    }) | CatchEvent([state, update_nav](Event event) -> bool {
        // Next / Complete
        if (event == Event::Return) {
            auto& ws = state->wizard_state;
            if (ws.current_step_index >= ws.total_steps - 1) {
                // Complete
                ws.is_completed = true;
                if (state->props.on_complete) state->props.on_complete();
            } else {
                ws.navigation_history.push_back(ws.current_step_index);
                ws.current_step_index++;
                update_nav(*state);
            }
            return true;
        }

        // Back
        if (event == Event::Escape) {
            auto& ws = state->wizard_state;
            if (!ws.navigation_history.empty()) {
                ws.current_step_index = ws.navigation_history.back();
                ws.navigation_history.pop_back();
                update_nav(*state);
            } else {
                // At first step, cancel wizard
                if (state->props.on_cancel) state->props.on_cancel();
            }
            return true;
        }

        // Skip (Tab)
        if (event == Event::Tab) {
            auto& ws = state->wizard_state;
            if (ws.current_step_index < ws.total_steps - 1) {
                ws.navigation_history.push_back(ws.current_step_index);
                ws.current_step_index++;
                update_nav(*state);
            }
            return true;
        }

        return false;
    });
}

/// Create a simple wizard with step titles only (overload)
[[nodiscard]] inline Component WizardComponent(
    std::vector<std::string> step_titles,
    std::function<void()> on_complete,
    std::function<void()> on_cancel) {

    WizardProviderProps props;
    props.on_complete = std::move(on_complete);
    props.on_cancel = std::move(on_cancel);

    for (auto& title : step_titles) {
        WizardStep step;
        step.id = title;
        step.title = std::move(title);
        props.steps.push_back(std::move(step));
    }

    return WizardComponent(std::move(props));
}

/// Create a wizard with a title (overload)
[[nodiscard]] inline Component WizardComponent(
    std::string title,
    std::vector<WizardStep> steps,
    std::function<void()> on_complete,
    std::function<void()> on_cancel) {

    WizardProviderProps props;
    props.title = std::move(title);
    props.steps = std::move(steps);
    props.on_complete = std::move(on_complete);
    props.on_cancel = std::move(on_cancel);
    return WizardComponent(std::move(props));
}

} // namespace cc::ui::wizard_dialog
