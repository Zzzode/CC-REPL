/// @file desktop_upsell.cppm
/// @brief Desktop app upsell dialog — prompts users to upgrade to Claude Desktop.
/// Shows feature highlights and download/not-now action buttons.
module;
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.desktop_upsell;

export namespace cc::ui::dialogs {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// User's choice from the upsell dialog
enum class UpsellChoice {
    download,
    not_now,
};

/// Feature highlight item shown in the upsell body
struct UpsellFeature {
    std::string icon;
    std::string label;
    std::string description;
};

/// Props for the desktop upsell dialog component
struct DesktopUpsellProps {
    std::function<void(UpsellChoice)> on_select;
    std::vector<UpsellFeature> features;
    int dialog_width = 70;
};

// ============================================================
// Default Data
// ============================================================

/// Default feature list shown in the upsell dialog
[[nodiscard]] inline std::vector<UpsellFeature> default_upsell_features() {
    return {
        {"✨", "Native Experience",
         "Full desktop integration with system tray and notifications"},
        {"\U0001F680", "Faster Performance",
         "Optimized rendering with reduced latency"},
        {"\U0001F512", "Enhanced Security",
         "Secure credential storage via OS keychain"},
        {"\U0001F4CA", "Advanced Analytics",
         "Detailed usage dashboards and insights"},
    };
}

// ============================================================
// Element Rendering
// ============================================================

/// Render the feature highlight list
[[nodiscard]] inline Element RenderFeatureList(
    const std::vector<UpsellFeature>& features) {

    Elements items;
    for (const auto& feat : features) {
        items.push_back(hbox({
            text("  " + feat.icon + " "),
            text(feat.label) | bold,
            text(" — "),
            text(feat.description) | dim,
        }));
    }
    return vbox(items);
}

/// Render the action buttons bar
[[nodiscard]] inline Element RenderButtonBar(int focused) {
    auto download_label = text(" Download ");
    auto not_now_label  = text(" Not Now ");

    auto download_btn = (focused == 0)
        ? download_label | bold | inverted | color(Color::Green)
        : download_label | color(Color::Green);

    auto not_now_btn = (focused == 1)
        ? not_now_label | bold | inverted
        : not_now_label | dim;

    return hbox({
        text("  "),
        download_btn | size(WIDTH, GREATER_THAN, 14),
        text("  "),
        not_now_btn | size(WIDTH, GREATER_THAN, 12),
        filler(),
        text("Enter") | bold | dim,
        text(" confirm · ") | dim,
        text("Tab") | bold | dim,
        text(" switch · ") | dim,
        text("Esc") | bold | dim,
        text(" dismiss") | dim,
    });
}

/// Render the full desktop upsell dialog
[[nodiscard]] inline Element RenderDesktopUpsellDialog(
    const std::vector<UpsellFeature>& features,
    int focused_button,
    int width) {

    auto body = vbox({
        hbox({
            text(" \U0001F4BB ") | color(Color::Cyan),
            text("Get the full Claude experience with the desktop app. "
                 "Features include:") | dim,
        }),
        text(""),
        RenderFeatureList(features),
        text(""),
        separator(),
        RenderButtonBar(focused_button),
    });

    return window(
        text(" Upgrade to Claude Desktop ") | bold | color(Color::Cyan),
        body | size(WIDTH, LESS_THAN, width)
    ) | color(Color::Cyan);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the desktop upsell dialog component
[[nodiscard]] inline Component DesktopUpsellDialog(DesktopUpsellProps props) {
    constexpr int k_button_count = 2;

    struct State {
        DesktopUpsellProps props;
        int focused_button = 0;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    // Populate default features if none provided
    if (state->props.features.empty()) {
        state->props.features = default_upsell_features();
    }

    return Renderer([state] {
        return RenderDesktopUpsellDialog(
            state->props.features,
            state->focused_button,
            state->props.dialog_width);
    }) | CatchEvent([state](Event event) -> bool {
        // Tab / Arrow to switch between buttons
        if (event == Event::Tab || event == Event::ArrowRight
            || event == Event::ArrowLeft) {
            state->focused_button =
                (state->focused_button + 1) % k_button_count;
            return true;
        }

        // Enter to confirm selection
        if (event == Event::Return) {
            if (state->props.on_select) {
                auto choice = (state->focused_button == 0)
                    ? UpsellChoice::download
                    : UpsellChoice::not_now;
                state->props.on_select(choice);
            }
            return true;
        }

        // Escape to dismiss (treat as "not now")
        if (event == Event::Escape) {
            if (state->props.on_select) {
                state->props.on_select(UpsellChoice::not_now);
            }
            return true;
        }

        return false;
    });
}

/// Create desktop upsell dialog with a simple callback (overload)
[[nodiscard]] inline Component DesktopUpsellDialog(
    std::function<void(UpsellChoice)> on_select) {

    DesktopUpsellProps props;
    props.on_select = std::move(on_select);
    return DesktopUpsellDialog(std::move(props));
}

} // namespace cc::ui::dialogs
