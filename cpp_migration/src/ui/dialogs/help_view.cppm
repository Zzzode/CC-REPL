/// @file help_view.cppm
/// @brief Faithful Help dialog with 3 tabs:
///        general, commands, custom-commands.
///
/// MODULE:   cc.ui.dialogs.help_view
/// LICENCE:  Exported.  Imported by repl_screen (HelpView panel mode) and
///           dialog_default_renderers (queue-based rendering).
///
/// TS REFERENCE:
///   Upstream help dialog, general tab, and commands tab components.
///
/// VISUAL STRUCTURE (faithful to TS):
///   ┌─ Claude Code v0.0.0 ─────────────────────────────┐
///   │  general │ commands │ custom-commands             │
///   ├──────────────────────────────────────────────────┤
///   │                                                  │
///   │  Tab content (General / Commands list / Custom)  │
///   │                                                  │
///   ├──────────────────────────────────────────────────┤
///   │  For more help: https://code.claude.com/docs      │
///   │  esc to cancel                                    │
///   └──────────────────────────────────────────────────┘
///
/// KEYBOARD:
///   Tab / Shift+Tab  — switch tabs
///   Esc              — close dialog
///   j/k / ↑/↓        — navigate command list
///   Enter            — run selected command
module;

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.help_view;

import cc.ui.dialogs.frame;
import cc.ui.design.theme;
import cc.ui.design.tokens;

export namespace cc::ui::dialogs::help_view {

using namespace ftxui;
namespace dframe = cc::ui::dialogs::frame;
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;

// ============================================================
// Types
// ============================================================

/// Help dialog tab identifiers (1:1 with TS tabs).
enum class HelpTab : std::uint8_t {
    General = 0,
    Commands,
    CustomCommands,
};

/// A single command entry in the commands tab.
struct HelpCommandEntry {
    std::string name;         // e.g. "help", "compact"
    std::string description;  // one-line description
    bool is_builtin = true;   // true = builtin, false = custom
};

/// Help dialog properties.
struct HelpViewProps {
    std::string app_version = "0.0.0";
    std::vector<HelpCommandEntry> builtin_commands;
    std::vector<HelpCommandEntry> custom_commands;
    /// Called when user selects a command to run (Enter on command list).
    std::function<void(std::string_view command_name)> on_run_command;
    /// Called when dialog is dismissed (Esc / close).
    std::function<void()> on_close;
};

/// Mutable state for the interactive help dialog.
struct HelpViewState {
    HelpTab active_tab = HelpTab::General;
    int command_list_index = 0;  // index into active command list
    int scroll_offset = 0;
};

// ============================================================
// Data helpers — default content
// ============================================================

/// Get default built-in commands (matching TS builtin filter).
[[nodiscard]] inline std::vector<HelpCommandEntry> default_builtin_commands() {
    return {
        {"help",       "Show this help screen"},
        {"compact",    "Summarize and compact the conversation"},
        {"clear",      "Start a new conversation"},
        {"resume",     "Resume a previous session"},
        {"history",    "Show conversation history"},
        {"config",     "Open configuration editor"},
        {"model",      "Switch the current model"},
        {"mode",       "Switch input mode"},
        {"doctor",     "Run system diagnostics"},
        {"commit",     "Create a git commit with a generated message"},
        {"review",     "Run a code review on the current changes"},
        {"mcp",        "Manage MCP servers"},
        {"vim",        "Toggle vim mode"},
    };
}

// ============================================================
// Tab renderers
// ============================================================

namespace detail {

/// Render the tab bar (general / commands / custom-commands).
[[nodiscard]] inline Element RenderTabBar(HelpTab active, const Theme& theme) {
    auto tab = [&](std::string_view label, HelpTab t) -> Element {
        bool is_active = active == t;
        auto el = text(std::string{label});
        if (is_active) {
            el = el | bold | color(theme.color_for(Role::Info));
        } else {
            el = el | dim;
        }
        // Add underline for active tab
        if (is_active) {
            el = hbox({
                text(" "),
                el,
                text(" "),
            }) | underlined | color(theme.color_for(Role::Info));
        } else {
            el = hbox({ text(" "), el, text(" ") });
        }
        return el;
    };

    return hbox({
        tab("general", HelpTab::General),
        tab("commands", HelpTab::Commands),
        tab("custom-commands", HelpTab::CustomCommands),
        filler(),
    }) | color(theme.color_for(Role::Chrome));
}

/// Render the General tab content.
/// Faithful to TS help general tab: intro text + Shortcuts list.
[[nodiscard]] inline Element RenderGeneralTab(const Theme& theme) {
    Elements shortcuts;
    shortcuts.push_back(text("Shortcuts") | bold);

    // Format: shortcut — description (two-column layout)
    auto shortcut_row = [&](std::string_view key, std::string_view desc) {
        return hbox({
            text(std::string{key}) | color(theme.color_for(Role::Info)) | bold,
            text("  "),
            text(std::string{desc}) | dim,
        });
    };

    shortcuts.push_back(text(""));
    shortcuts.push_back(shortcut_row("↑/↓",         "Navigate history"));
    shortcuts.push_back(shortcut_row("Ctrl+C",      "Cancel current operation"));
    shortcuts.push_back(shortcut_row("Ctrl+D",      "Exit Claude Code"));
    shortcuts.push_back(shortcut_row("Ctrl+L",      "Clear screen"));
    shortcuts.push_back(shortcut_row("Tab",         "Autocomplete"));
    shortcuts.push_back(shortcut_row("Shift+Tab",   "Previous suggestion"));
    shortcuts.push_back(shortcut_row("Enter",       "Submit message"));
    shortcuts.push_back(shortcut_row("Shift+Enter", "New line"));
    shortcuts.push_back(shortcut_row("Esc",         "Dismiss dialog"));

    return vbox({
        paragraph(
            "Claude understands your codebase, makes edits with your "
            "permission, and executes commands — right from your terminal."
        ),
        text(""),
        vbox(shortcuts),
    });
}

/// Render the Commands / Custom Commands tab content.
/// Faithful to TS help commands tab: selectable list with descriptions.
[[nodiscard]] inline Element RenderCommandsTab(
    const std::vector<HelpCommandEntry>& commands,
    int selected_index,
    int scroll_offset,
    int max_visible,
    const Theme& theme)
{
    if (commands.empty()) {
        return vbox({
            text(""),
            text("No commands found") | dim | center,
        });
    }

    int count = static_cast<int>(commands.size());
    int visible = std::min(max_visible, count);

    // Clamp scroll
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset + visible > count) scroll_offset = std::max(0, count - visible);
    if (scroll_offset > count) scroll_offset = std::max(0, count - visible);

    Elements rows;
    for (int i = 0; i < visible; ++i) {
        int idx = scroll_offset + i;
        if (idx < 0 || idx >= count) continue;

        const auto& cmd = commands[idx];
        bool is_selected = idx == selected_index;

        auto cmd_el = hbox({
            text("/" + cmd.name) | color(theme.color_for(Role::Info)),
            filler(),
            text(cmd.description) | dim,
        });

        if (is_selected) {
            cmd_el = cmd_el | inverted | focus;
        }

        rows.push_back(cmd_el);
    }

    return vbox(rows);
}

/// Render the footer (docs link + dismiss hint).
[[nodiscard]] inline Element RenderFooter(const Theme& theme,
                                           bool exit_pending = false,
                                           std::string_view exit_key = "") {
    Elements rows;

    // Docs link row
    rows.push_back(hbox({
        text("For more help: ") | dim,
        text("https://code.claude.com/docs/en/overview") | color(theme.color_for(Role::Info)),
    }));

    // Dismiss hint
    if (exit_pending && !exit_key.empty()) {
        rows.push_back(hbox({
            text("Press "),
            text(std::string{exit_key}) | bold,
            text(" again to exit"),
        }) | dim);
    } else {
        rows.push_back(hbox({
            text("esc"),
            text(" to cancel") | dim,
        }) | dim);
    }

    return vbox(rows);
}

} // namespace detail

// ============================================================
// Main HelpView renderer (pure Element version)
// ============================================================

/// Render the help dialog (read-only version — no interaction).
/// Faithful to TS help visual structure.
[[nodiscard]] inline Element RenderHelpView(
    const HelpViewProps& props,
    const HelpViewState& state,
    const Theme& theme,
    int max_visible_commands = 10)
{
    using namespace detail;

    // Build tab bar
    auto tab_bar = RenderTabBar(state.active_tab, theme);

    // Build content based on active tab
    Element content = text("");
    switch (state.active_tab) {
        case HelpTab::General:
            content = RenderGeneralTab(theme);
            break;
        case HelpTab::Commands:
            content = RenderCommandsTab(
                props.builtin_commands,
                state.command_list_index,
                state.scroll_offset,
                max_visible_commands,
                theme);
            break;
        case HelpTab::CustomCommands:
            content = RenderCommandsTab(
                props.custom_commands,
                state.command_list_index,
                state.scroll_offset,
                max_visible_commands,
                theme);
            break;
    }

    // Footer
    auto footer = RenderFooter(theme);

    // Assemble body content (tab bar + content + footer)
    auto body = vbox({
        tab_bar,
        text(""),  // spacer
        content,
        text(""),  // spacer
        footer,
    });

    // Wrap in DialogFrame
    dframe::DialogFrameProps frame_props;
    frame_props.title = "Claude Code v" + props.app_version;
    frame_props.subtitle = "Help";
    frame_props.style = dframe::FrameStyle::Info;
    frame_props.content = body;
    frame_props.full_border = true;
    frame_props.inner_padding_x = 1;
    frame_props.inner_padding_y = 0;

    return dframe::DialogFrame(frame_props, theme);
}

// ============================================================
// Interactive HelpView Component
// ============================================================

/// Create an interactive HelpView component (with keyboard navigation).
/// Faithful to TS help interactive behavior.
[[nodiscard]] inline Component MakeHelpView(
    std::shared_ptr<HelpViewState> state,
    HelpViewProps props,
    const Theme& theme)
{
    using namespace detail;

    auto renderer = Renderer([state, props = std::move(props), &theme]() -> Element {
        return RenderHelpView(props, *state, theme);
    });

    auto with_events = CatchEvent([state, props = std::move(props)](Event event) -> bool {
        // Tab navigation
        if (event == Event::Tab || event == Event::ArrowRight) {
            auto t = static_cast<int>(state->active_tab);
            state->active_tab = static_cast<HelpTab>((t + 1) % 3);
            state->command_list_index = 0;
            state->scroll_offset = 0;
            return true;
        }
        if (event == Event::TabReverse || event == Event::ArrowLeft) {
            auto t = static_cast<int>(state->active_tab);
            state->active_tab = static_cast<HelpTab>((t + 2) % 3);
            state->command_list_index = 0;
            state->scroll_offset = 0;
            return true;
        }

        // Command list navigation (only for commands/custom tabs)
        const auto* list = [&]() -> const std::vector<HelpCommandEntry>* {
            switch (state->active_tab) {
                case HelpTab::Commands: return &props.builtin_commands;
                case HelpTab::CustomCommands: return &props.custom_commands;
                default: return nullptr;
            }
        }();

        if (list) {
            int count = static_cast<int>(list->size());
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                if (count > 0) {
                    state->command_list_index =
                        (state->command_list_index - 1 + count) % count;
                    // Update scroll
                    if (state->command_list_index < state->scroll_offset) {
                        state->scroll_offset = state->command_list_index;
                    }
                }
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                if (count > 0) {
                    state->command_list_index =
                        (state->command_list_index + 1) % count;
                    // Update scroll (assuming ~10 visible)
                    int visible = 10;
                    if (state->command_list_index >= state->scroll_offset + visible) {
                        state->scroll_offset = state->command_list_index - visible + 1;
                    }
                }
                return true;
            }
            if (event == Event::Return) {
                if (count > 0 && props.on_run_command) {
                    auto idx = state->command_list_index;
                    if (idx >= 0 && idx < count) {
                        props.on_run_command((*list)[idx].name);
                    }
                }
                return true;
            }
        }

        // Close
        if (event == Event::Escape) {
            if (props.on_close) props.on_close();
            return true;
        }

        return false;
    });

    return renderer | with_events;
}

} // namespace cc::ui::dialogs::help_view
