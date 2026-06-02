/// @file sandbox_dialog.cppm
/// @brief Sandbox settings dialog — mode selection, overrides, config, dependencies.
/// Migrated from SandboxSettings.tsx.
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

export module cc.ui.sandbox_dialog;

export namespace cc::ui::sandbox_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Sandbox execution mode
enum class SandboxMode {
    auto_allow,  // Sandbox BashTool, with auto-allow
    regular,     // Sandbox BashTool, with regular permissions
    disabled,    // No Sandbox
};

/// Convert sandbox mode to display label
[[nodiscard]] inline std::string mode_label(SandboxMode mode) {
    switch (mode) {
        case SandboxMode::auto_allow:
            return "Sandbox BashTool, with auto-allow";
        case SandboxMode::regular:
            return "Sandbox BashTool, with regular permissions";
        case SandboxMode::disabled:
            return "No Sandbox";
    }
    return "?";
}

/// Available tabs in sandbox settings
enum class SandboxTab {
    mode,
    overrides,
    config,
    dependencies,
};

/// Convert tab to display string
[[nodiscard]] inline std::string tab_label(SandboxTab tab) {
    switch (tab) {
        case SandboxTab::mode: return "Mode";
        case SandboxTab::overrides: return "Overrides";
        case SandboxTab::config: return "Config";
        case SandboxTab::dependencies: return "Dependencies";
    }
    return "?";
}

/// Dependency check severity
enum class DependencyCheckSeverity {
    info,
    warning,
    error,
};

/// A single dependency check result
struct DependencyCheckEntry {
    std::string message;
    DependencyCheckSeverity severity = DependencyCheckSeverity::info;
};

/// Full dependency check state (mirrors SandboxDependencyCheck)
struct SandboxDependencyCheck {
    std::vector<DependencyCheckEntry> warnings;
    std::vector<DependencyCheckEntry> errors;
};

/// Command result display mode
enum class CommandResultDisplay {
    normal,
    system,
    skip,
};

/// Props for the sandbox settings dialog
struct SandboxDialogProps {
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_complete;
    SandboxDependencyCheck dep_check;
    SandboxMode current_mode = SandboxMode::regular;
};

/// Override entry for bash tool
struct SandboxOverride {
    std::string pattern;
    bool allow = true;
    std::string description;
};

// ============================================================
// Element Rendering
// ============================================================

/// Render a mode option item
[[nodiscard]] inline Element RenderModeOption(
    SandboxMode mode, SandboxMode current, bool selected) {

    auto label = mode_label(mode);
    if (mode == current) {
        label += " (current)";
    }

    auto prefix = selected
        ? text("\u276F ") | color(Color::Cyan)
        : text("  ");
    auto label_el = text(label);
    if (selected) label_el = label_el | bold;
    if (mode == current) {
        auto suffix = text(" (current)") | color(Color::Green);
        return hbox({prefix, text(mode_label(mode)), suffix});
    }
    return hbox({prefix, label_el});
}

/// Render the mode selection tab
[[nodiscard]] inline Element RenderModeTab(
    SandboxMode current_mode, int selected, bool show_socket_warning) {

    Elements items;

    if (show_socket_warning) {
        items.push_back(
            text("Cannot block unix domain sockets (see Dependencies tab)")
            | color(Color::Yellow));
        items.push_back(text(""));
    }

    items.push_back(text("Configure Mode:") | bold);
    items.push_back(text(""));

    const std::array<SandboxMode, 3> modes = {
        SandboxMode::auto_allow,
        SandboxMode::regular,
        SandboxMode::disabled,
    };

    for (int i = 0; i < 3; ++i) {
        items.push_back(RenderModeOption(modes[i], current_mode, i == selected));
    }

    items.push_back(text(""));
    items.push_back(vbox({
        hbox({
            text("Auto-allow mode: ") | bold | dim,
            paragraph("Commands will try to run in the sandbox automatically, "
                     "and attempts to run outside of the sandbox fallback to "
                     "regular permissions. Explicit ask/deny rules are always "
                     "respected.") | dim,
        }),
        text(""),
        hbox({
            text("Learn more: ") | dim,
            text("code.claude.com/docs/en/sandboxing") | color(Color::Cyan) | underlined,
        }),
    }));

    return vbox(items);
}

/// Render the overrides tab
[[nodiscard]] inline Element RenderOverridesTab(
    const std::vector<SandboxOverride>& overrides) {

    if (overrides.empty()) {
        return vbox({
            text("  No overrides configured") | dim,
            text(""),
            text("  Overrides allow you to explicitly allow or deny") | dim,
            text("  specific command patterns.") | dim,
        });
    }

    Elements items;
    items.push_back(text(" Command Overrides:") | bold);
    items.push_back(separator());

    for (const auto& ovr : overrides) {
        auto indicator = ovr.allow
            ? text(" \u2713 ") | color(Color::Green)
            : text(" \u2717 ") | color(Color::Red);
        items.push_back(hbox({
            indicator,
            text(ovr.pattern) | bold,
            text(" — "),
            text(ovr.description) | dim,
        }));
    }
    return vbox(items);
}

/// Render the config tab (shows current sandbox JSON config)
[[nodiscard]] inline Element RenderConfigTab() {
    return vbox({
        text("  Sandbox Configuration") | bold,
        text(""),
        text("  Configuration is stored in:") | dim,
        text("  ~/.claude/settings.json") | color(Color::Cyan),
        text(""),
        text("  Edit manually or use overrides tab.") | dim,
    });
}

/// Render the dependencies tab
[[nodiscard]] inline Element RenderDependenciesTab(
    const SandboxDependencyCheck& dep_check) {

    Elements items;

    if (!dep_check.errors.empty()) {
        items.push_back(text(" Errors:") | bold | color(Color::Red));
        for (const auto& err : dep_check.errors) {
            items.push_back(hbox({
                text("  \u2717 ") | color(Color::Red),
                text(err.message),
            }));
        }
        items.push_back(text(""));
    }

    if (!dep_check.warnings.empty()) {
        items.push_back(text(" Warnings:") | bold | color(Color::Yellow));
        for (const auto& warn : dep_check.warnings) {
            items.push_back(hbox({
                text("  \u26A0 ") | color(Color::Yellow),
                text(warn.message),
            }));
        }
    }

    if (dep_check.errors.empty() && dep_check.warnings.empty()) {
        items.push_back(hbox({
            text(" \u2713 ") | color(Color::Green),
            text("All dependencies satisfied") | bold,
        }));
    }

    return vbox(items);
}

/// Render tab header for sandbox dialog
[[nodiscard]] inline Element RenderSandboxTabHeader(
    SandboxTab selected, bool has_errors, bool has_warnings) {

    std::vector<SandboxTab> tabs;
    if (has_errors) {
        // Only show dependencies tab when there are errors
        tabs.push_back(SandboxTab::dependencies);
    } else {
        tabs.push_back(SandboxTab::mode);
        if (has_warnings) tabs.push_back(SandboxTab::dependencies);
        tabs.push_back(SandboxTab::overrides);
        tabs.push_back(SandboxTab::config);
    }

    Elements elements;
    for (const auto& tab : tabs) {
        auto label = tab_label(tab);
        auto el = text(" " + label + " ");
        if (tab == selected) {
            el = el | bold | inverted;
        } else {
            el = el | dim;
        }
        elements.push_back(el);
    }
    return hbox(elements);
}

/// Render the full sandbox dialog
[[nodiscard]] inline Element RenderSandboxDialog(
    const SandboxDialogProps& props,
    SandboxTab active_tab,
    int mode_selected) {

    bool has_errors = !props.dep_check.errors.empty();
    bool has_warnings = !props.dep_check.warnings.empty();
    bool show_socket_warning = has_warnings; // simplified

    auto header = RenderSandboxTabHeader(active_tab, has_errors, has_warnings);

    Element content;
    switch (active_tab) {
        case SandboxTab::mode:
            content = RenderModeTab(props.current_mode, mode_selected, show_socket_warning);
            break;
        case SandboxTab::overrides:
            content = RenderOverridesTab({});
            break;
        case SandboxTab::config:
            content = RenderConfigTab();
            break;
        case SandboxTab::dependencies:
            content = RenderDependenciesTab(props.dep_check);
            break;
    }

    auto body = vbox({
        header,
        separator(),
        content | flex,
    });

    return window(
        text(" Sandbox: ") | bold | color(Color::Magenta),
        body
    ) | color(Color::Magenta);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the sandbox settings dialog component
[[nodiscard]] inline Component SandboxDialog(SandboxDialogProps props) {
    struct State {
        SandboxDialogProps props;
        SandboxTab active_tab = SandboxTab::mode;
        int mode_selected = 0;
    };
    constexpr int kModeCount = 3;

    auto state = std::make_shared<State>();
    // Start on dependencies tab if there are blocking errors
    if (!props.dep_check.errors.empty()) {
        state->active_tab = SandboxTab::dependencies;
    }
    state->props = std::move(props);

    return Renderer([state] {
        return RenderSandboxDialog(
            state->props, state->active_tab, state->mode_selected);
    }) | CatchEvent([state](Event event) -> bool {
        // Tab navigation
        if (event == Event::ArrowLeft) {
            auto idx = static_cast<int>(state->active_tab);
            if (idx > 0) state->active_tab = static_cast<SandboxTab>(idx - 1);
            return true;
        }
        if (event == Event::ArrowRight) {
            auto idx = static_cast<int>(state->active_tab);
            if (idx < 3) state->active_tab = static_cast<SandboxTab>(idx + 1);
            return true;
        }

        // Mode tab item selection
        if (state->active_tab == SandboxTab::mode) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->mode_selected = std::max(0, state->mode_selected - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->mode_selected = std::min(
                    kModeCount - 1, state->mode_selected + 1);
                return true;
            }
            if (event == Event::Return) {
                // Apply mode selection
                std::array<SandboxMode, 3> modes = {
                    SandboxMode::auto_allow,
                    SandboxMode::regular,
                    SandboxMode::disabled,
                };
                auto selected_mode = modes[state->mode_selected];
                std::string msg;
                switch (selected_mode) {
                    case SandboxMode::auto_allow:
                        msg = "\u2713 Sandbox enabled with auto-allow for bash commands";
                        break;
                    case SandboxMode::regular:
                        msg = "\u2713 Sandbox enabled with regular bash permissions";
                        break;
                    case SandboxMode::disabled:
                        msg = "\u25CB Sandbox disabled";
                        break;
                }
                if (state->props.on_complete) {
                    state->props.on_complete(msg, CommandResultDisplay::normal);
                }
                return true;
            }
        }

        // Escape to dismiss
        if (event == Event::Escape) {
            if (state->props.on_complete) {
                state->props.on_complete(std::nullopt, CommandResultDisplay::skip);
            }
            return true;
        }

        return false;
    });
}

/// Create sandbox dialog with dependency check (overload)
[[nodiscard]] inline Component SandboxDialog(
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_complete,
    SandboxDependencyCheck dep_check) {

    SandboxDialogProps props;
    props.on_complete = std::move(on_complete);
    props.dep_check = std::move(dep_check);
    return SandboxDialog(std::move(props));
}

} // namespace cc::ui::sandbox_dialog
