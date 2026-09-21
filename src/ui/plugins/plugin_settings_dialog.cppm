/// @file plugin_settings_dialog.cppm
/// @brief Per-plugin settings: global auto-update toggle, per-plugin
///        enable/disable, per-plugin options, and keybindings editor
///        (name + hotkey input with add/remove).
///
/// Data comes from C1: plugin_ui_data::InstalledCellData and
/// plugin_ui_data::ConfigStep — no direct engine reads here.
module;

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.plugins.plugin_settings_dialog;

import cc.types.types;
import cc.commands.plugin_ui_data;

export namespace cc::ui::plugins::plugin_settings_dialog {
using namespace ftxui;

namespace ui = cc::commands::plugin_ui;

using ui::InstalledCellData;
using ui::ConfigStep;

// =========================================================================
// Keybinding entry (name → hotkey string)
// =========================================================================

struct KeybindingEntry {
    std::string name;        // e.g. "toggle-format", "send-chunk"
    std::string hotkey;      // e.g. "Ctrl+Alt+F", "⌘K"
    std::string scope;       // "global" / "plugin-local"
    bool        enabled = true;
};

// =========================================================================
// Per-plugin options row (schema name → current value)
// =========================================================================

struct PluginOption {
    std::string name;
    std::string current_value;
    std::string type_hint;   // "string" / "bool" / "number" / "select"
    bool        required = false;
    bool        is_secret = false;
    std::vector<std::string> allowed_values;  // for "select" type
};

struct PerPluginSettings {
    std::string plugin_id;
    bool        enabled = true;
    bool        auto_update = true;
    std::vector<PluginOption> options;
    std::vector<KeybindingEntry> keybindings;
    std::vector<ConfigStep> pending_steps;   // from C1::build_config_steps
};

// =========================================================================
// Global settings
// =========================================================================

struct GlobalPluginSettings {
    bool auto_update_all = true;
    bool allow_unverified = false;
    bool allow_unsafe_permissions = false;
    bool show_trust_warning_on_install = true;
    std::string default_install_scope = "user";   // user / project / local
};

// =========================================================================
// Dialog inputs (all from C1 prep)
// =========================================================================

struct SettingsDialogInputs {
    GlobalPluginSettings          globals;
    std::vector<PerPluginSettings> per_plugin;   // one per installed plugin

    // Routed focus hint
    std::optional<std::string> focus_plugin_id;

    // Callbacks
    std::function<void(const GlobalPluginSettings&)> on_save_globals;
    std::function<void(std::string_view plugin_id,
                       const PerPluginSettings&)>     on_save_plugin;
    std::function<void(std::string_view plugin_id,
                       const KeybindingEntry& kb)>    on_update_keybinding;
    std::function<void(std::string_view plugin_id,
                       std::string_view kb_name)>      on_remove_keybinding;
    std::function<void()>                              on_close;
};

// =========================================================================
// Sub-tabs inside the settings dialog
// =========================================================================

enum class SettingsTab : unsigned char {
    Global,      // global auto-update + trust settings
    Plugins,     // per-plugin list (enable / auto-update toggles)
    Options,     // per-plugin options (schema values)
    Keybindings, // hotkey editor
};

inline constexpr const char* k_settings_tab_names[] = {
    "Global", "Plugins", "Options", "Keybindings",
};

// =========================================================================
// Rendering helpers
// =========================================================================

namespace detail {

// Render a boolean toggle row.
[[nodiscard]] inline Element BoolToggleRow(
    std::string_view label, std::string_view hint,
    bool value, bool selected, Color accent = Color::Cyan)
{
    auto val = text(value ? " [✓ ON] " : " [  OFF] ")
        | color(value ? Color::Green : Color::GrayDark) | bold;
    auto row = hbox({
        text(selected ? "› " : "  ") | color(accent),
        text(std::string{label}) | bold
             | color(selected ? Color::White : Color::GrayLight),
        filler(),
        std::move(val),
    });
    if (selected) row = row | bgcolor(Color::RGB(20, 30, 50));
    Elements out{std::move(row)};
    if (!hint.empty()) {
        out.push_back(paragraph("   " + std::string{hint}) | dim);
    }
    return vbox(std::move(out));
}

// Render the 4-tab header
[[nodiscard]] inline Element RenderSettingsTabBar(SettingsTab active) {
    Elements bits;
    for (unsigned i = 0; i < 4; ++i) {
        const bool sel = (i == (unsigned)active);
        bits.push_back(text(" " + std::string{k_settings_tab_names[i]} + " ")
            | (sel ? (bold | color(Color::Blue) | bgcolor(Color::RGB(20, 30, 55)))
                   : dim));
        if (i + 1 < 4) bits.push_back(text("│") | dim);
    }
    return hbox(std::move(bits));
}

// ---- Global tab ----

[[nodiscard]] inline Element RenderGlobalTab(
    const GlobalPluginSettings& g, int selected_row)
{
    // Rows: auto-update-all, default-scope, allow-unverified,
    //       allow-unsafe, show-trust-warning
    return vbox({
        hbox({
            text(" 🌐  Global Plugin Settings ") | bold | color(Color::Blue),
            filler(),
            text("Applies to all plugins") | dim,
        }),
        separator(),
        text(""),
        BoolToggleRow("Auto-update all plugins",
            "When enabled, periodically checks for updates and installs them.",
            g.auto_update_all, selected_row == 0, Color::Green),
        separator() | dim,
        // Default install scope (pseudo-toggle: cycle through user/project/local)
        hbox({
            text(selected_row == 1 ? "› " : "  ") | color(Color::Yellow),
            text("Default install scope") | bold
                 | color(selected_row == 1 ? Color::White : Color::GrayLight),
            filler(),
            text(" [" + g.default_install_scope + "] ") | bold | color(Color::Yellow),
        }) | (selected_row == 1 ? bgcolor(Color::RGB(40, 35, 10)) : nothing),
        paragraph("   New plugin installations use this scope unless overridden.") | dim,
        separator() | dim,
        BoolToggleRow("Allow unverified plugins",
            "Install plugins that are not signed or verified by a trusted marketplace.",
            g.allow_unverified, selected_row == 2, Color::Orange1),
        separator() | dim,
        BoolToggleRow("Allow elevated permissions",
            "Permit plugins to request 'unsafe' permissions (bash, exec, file write).",
            g.allow_unsafe_permissions, selected_row == 3, Color::Red),
        separator() | dim,
        BoolToggleRow("Show trust warning on install",
            "Display the security trust dialog before every plugin install/update.",
            g.show_trust_warning_on_install, selected_row == 4, Color::Cyan),
        text(""),
        separator(),
        hbox({
            text(" ↑↓/j/k") | bold | color(Color::Cyan), text(" navigate  "),
            text("Space") | bold | color(Color::Cyan), text(" toggle  "),
            text("⏎") | bold | color(Color::Cyan), text(" save  "),
            text("Esc") | bold | color(Color::Cyan), text(" close"),
            filler(),
            text("[Ctrl+S] save all") | dim,
        }) | dim,
    });
}

// ---- Plugins tab (per-plugin enable/auto-update) ----

[[nodiscard]] inline Element RenderPerPluginRow(
    const PerPluginSettings& p, bool selected, bool is_focus_target)
{
    auto enabled_dot = text(p.enabled ? "● " : "○ ")
        | color(p.enabled ? Color::Green : Color::GrayDark);
    auto au_dot = text(p.auto_update ? "⬆ " : "  ")
        | color(p.auto_update ? Color::Yellow : Color::GrayDark);

    auto row = hbox({
        text(is_focus_target ? "★ " : (selected ? "› " : "  "))
            | color(is_focus_target ? Color::Yellow : Color::Cyan),
        enabled_dot,
        text(p.plugin_id) | bold
             | color(selected ? Color::White : Color::GrayLight),
        filler(),
        au_dot,
        text(p.auto_update ? "auto-update" : "manual-only") | dim,
        text("  "),
        text("[" + std::to_string(p.options.size()) + " opt]") | dim,
        text(" "),
        text("[" + std::to_string(p.keybindings.size()) + " key]") | dim,
    });
    if (selected) row = row | bgcolor(Color::RGB(20, 35, 50));
    return row;
}

[[nodiscard]] inline Element RenderPluginsTab(
    const std::vector<PerPluginSettings>& all,
    int selected,
    std::optional<std::string> focus_id)
{
    Elements rows;
    rows.push_back(hbox({
        text(" 🧩  Per-Plugin Toggles ") | bold | color(Color::Magenta),
        filler(),
        text(std::format("{} plugin{} installed",
                         all.size(), all.size() == 1 ? "" : "s")) | dim,
    }));
    rows.push_back(separator());
    rows.push_back(hbox({
        text(" ") | dim,
        text("Status") | dim | size(WIDTH, EQUAL, 8),
        text("Plugin") | dim,
        filler(),
        text("Updates") | dim | size(WIDTH, EQUAL, 14),
        text("Opts") | dim | size(WIDTH, EQUAL, 6),
        text("Keys") | dim | size(WIDTH, EQUAL, 6),
    }));
    rows.push_back(separator() | dim);

    if (all.empty()) {
        rows.push_back(text("  No plugins installed yet.") | dim | center);
    } else {
        for (int i = 0; i < (int)all.size(); ++i) {
            const bool is_target =
                focus_id.has_value() && all[i].plugin_id == *focus_id;
            rows.push_back(RenderPerPluginRow(all[i], i == selected, is_target));
            if (i + 1 < (int)all.size()) rows.push_back(separator() | dim);
        }
    }

    rows.push_back(separator());
    rows.push_back(hbox({
        text(" ↑↓/j/k") | bold | color(Color::Cyan), text(" navigate  "),
        text("Space") | bold | color(Color::Cyan), text(" toggle enable  "),
        text("U") | bold | color(Color::Cyan), text(" toggle auto-update  "),
        text("→") | bold | color(Color::Cyan), text(" go to options/keys"),
    }) | dim);
    return vbox(std::move(rows));
}

// ---- Options tab (schema values for the focused plugin) ----

[[nodiscard]] inline Element RenderOptionRow(
    const PluginOption& opt, bool selected)
{
    std::string display_value;
    if (opt.is_secret && !opt.current_value.empty()) {
        display_value.assign(opt.current_value.size(), '*');
    } else {
        display_value = opt.current_value.empty() ? "(unset)" : opt.current_value;
    }
    auto row = hbox({
        text(selected ? "› " : "  ") | color(Color::Green),
        text(opt.name) | bold
             | color(selected ? Color::White : Color::GrayLight),
        opt.required ? text(" *") | color(Color::Red) : text(""),
        text(" [" + opt.type_hint + "]") | dim,
        filler(),
        text(display_value) | color(selected ? Color::White : Color::CyanLight),
    });
    if (selected) row = row | bgcolor(Color::RGB(10, 35, 25));
    return row;
}

[[nodiscard]] inline Element RenderOptionsTab(
    const PerPluginSettings* focused, int selected_opt)
{
    Elements rows;
    rows.push_back(hbox({
        text(" ⚙  Plugin Options ") | bold | color(Color::Green),
        filler(),
        focused ? text(focused->plugin_id) | color(Color::Cyan) | underlined
                : text("(select a plugin first)") | dim,
    }));
    rows.push_back(separator());

    if (!focused || focused->options.empty()) {
        rows.push_back(text("  No options to configure.") | dim | center);
    } else {
        // Pending config steps banner
        if (!focused->pending_steps.empty()) {
            rows.push_back(vbox({
                hbox({
                    text(" 🔔 ") | color(Color::Yellow),
                    text(std::format("{} configuration step{} required before first use",
                                     focused->pending_steps.size(),
                                     focused->pending_steps.size() == 1 ? "" : "s"))
                        | bold | color(Color::Yellow),
                }),
                paragraph("   These options are required.  Fill them in to enable the plugin.") | dim,
            }) | borderLight | color(Color::Yellow));
            rows.push_back(text(""));
        }

        for (int i = 0; i < (int)focused->options.size(); ++i) {
            rows.push_back(RenderOptionRow(focused->options[i], i == selected_opt));
            if (i + 1 < (int)focused->options.size())
                rows.push_back(separator() | dim);
        }
    }

    rows.push_back(separator());
    rows.push_back(hbox({
        text(" ↑↓/j/k") | bold | color(Color::Cyan), text(" navigate  "),
        text("⏎") | bold | color(Color::Cyan), text(" edit value  "),
        text("←") | bold | color(Color::Cyan), text(" back to plugins list"),
    }) | dim);
    return vbox(std::move(rows));
}

// ---- Keybindings tab ----

[[nodiscard]] inline Element RenderKeybindingRow(
    const KeybindingEntry& kb, bool selected)
{
    auto scope_c = kb.scope == "global" ? Color::Cyan : Color::Magenta;
    auto row = hbox({
        text(selected ? "› " : "  ") | color(Color::Yellow),
        text(kb.enabled ? "● " : "○ ")
            | color(kb.enabled ? Color::Green : Color::GrayDark),
        text(kb.name) | bold
             | color(selected ? Color::White : Color::GrayLight),
        filler(),
        text("[" + kb.hotkey + "]") | bold | color(Color::Yellow),
        text(" "),
        text(kb.scope) | dim | color(scope_c),
    });
    if (selected) row = row | bgcolor(Color::RGB(35, 30, 10));
    return row;
}

[[nodiscard]] inline Element RenderKeybindingsTab(
    const PerPluginSettings* focused, int selected_kb,
    bool editing_new, std::string_view new_name, std::string_view new_hotkey)
{
    Elements rows;
    rows.push_back(hbox({
        text(" ⌨  Keybindings Editor ") | bold | color(Color::Yellow),
        filler(),
        focused ? text(focused->plugin_id) | color(Color::Cyan) | underlined
                : text("(select a plugin first)") | dim,
    }));
    rows.push_back(separator());

    if (!focused || focused->keybindings.empty()) {
        rows.push_back(text("  No keybindings yet.  Press [A] to add one.")
                        | dim | center);
    } else {
        for (int i = 0; i < (int)focused->keybindings.size(); ++i) {
            rows.push_back(RenderKeybindingRow(focused->keybindings[i],
                                               i == selected_kb));
            if (i + 1 < (int)focused->keybindings.size())
                rows.push_back(separator() | dim);
        }
    }

    // Add-new inline editor
    if (editing_new) {
        rows.push_back(separator());
        rows.push_back(text(" New keybinding:") | bold | color(Color::Yellow));
        rows.push_back(hbox({
            text("  name: ") | dim,
            text(std::string{new_name.empty() ? "(type name)" : std::string{new_name}})
                 | color(new_name.empty() ? Color::GrayDark : Color::White),
            text("│") | blink | color(Color::Yellow),
            filler(),
            text("hotkey: ") | dim,
            text(std::string{new_hotkey.empty() ? "(press key combo)"
                                               : std::string{new_hotkey}})
                 | color(new_hotkey.empty() ? Color::GrayDark : Color::Yellow) | bold,
        }) | borderLight | color(Color::Yellow));
    }

    rows.push_back(separator());
    rows.push_back(hbox({
        text(" ↑↓/j/k") | bold | color(Color::Cyan), text(" navigate  "),
        text("A") | bold | color(Color::Cyan), text("dd  "),
        text("R") | bold | color(Color::Cyan), text("emove  "),
        text("Space") | bold | color(Color::Cyan), text(" enable/disable  "),
        text("⏎") | bold | color(Color::Cyan), text(" edit hotkey"),
    }) | dim);
    return vbox(std::move(rows));
}

} // namespace detail

// =========================================================================
// Interactive component
// =========================================================================

struct SettingsState {
    SettingsDialogInputs inputs;
    SettingsTab          active_tab = SettingsTab::Global;

    // Per-tab selection indices
    int global_selected = 0;         // 0..4 rows
    int plugin_selected = 0;
    int option_selected = 0;
    int keybinding_selected = 0;

    // Keybindings editor state
    bool        kb_adding = false;
    int         kb_edit_field = 0;    // 0 = name, 1 = hotkey
    std::string kb_new_name;
    std::string kb_new_hotkey;

    // Currently-focused plugin pointer (for Options/Keybindings tabs)
    std::optional<std::size_t> focused_plugin_index;
};

[[nodiscard]] inline Component MakePluginSettingsDialog(
    SettingsDialogInputs inputs)
{
    auto state = std::make_shared<SettingsState>();
    state->inputs = std::move(inputs);

    // Auto-focus a plugin if routed
    if (state->inputs.focus_plugin_id) {
        for (std::size_t i = 0; i < state->inputs.per_plugin.size(); ++i) {
            if (state->inputs.per_plugin[i].plugin_id == *state->inputs.focus_plugin_id) {
                state->focused_plugin_index = i;
                state->plugin_selected = (int)i;
                state->active_tab = SettingsTab::Plugins;
                break;
            }
        }
    }

    // Build an embedded input component for option/keybind editing
    auto option_input = std::make_shared<std::string>();
    Component option_input_widget = Input(option_input.get(), "Enter value…");

    return Renderer([state] {
        Element body;
        switch (state->active_tab) {
            case SettingsTab::Global:
                body = detail::RenderGlobalTab(state->inputs.globals,
                                               state->global_selected);
                break;
            case SettingsTab::Plugins:
                body = detail::RenderPluginsTab(
                    state->inputs.per_plugin, state->plugin_selected,
                    state->inputs.focus_plugin_id);
                break;
            case SettingsTab::Options: {
                const PerPluginSettings* p = nullptr;
                if (state->focused_plugin_index &&
                    *state->focused_plugin_index < state->inputs.per_plugin.size())
                    p = &state->inputs.per_plugin[*state->focused_plugin_index];
                body = detail::RenderOptionsTab(p, state->option_selected);
                break;
            }
            case SettingsTab::Keybindings: {
                const PerPluginSettings* p = nullptr;
                if (state->focused_plugin_index &&
                    *state->focused_plugin_index < state->inputs.per_plugin.size())
                    p = &state->inputs.per_plugin[*state->focused_plugin_index];
                body = detail::RenderKeybindingsTab(
                    p, state->keybinding_selected,
                    state->kb_adding, state->kb_new_name, state->kb_new_hotkey);
                break;
            }
        }

        return vbox({
            hbox({
                text(" ⚙  Plugin Settings ") | bold | color(Color::Blue),
                filler(),
                detail::RenderSettingsTabBar(state->active_tab),
            }),
            separator() | color(Color::Blue),
            std::move(body) | flex,
        }) | borderDouble | bgcolor(Color::RGB(10, 15, 25));

    }) | CatchEvent([state, option_input](Event event) -> bool {
        // Tab switching: 1/2/3/4 digits OR Ctrl+[ Left / Right arrows
        if (event == Event::Tab) {
            state->active_tab = (SettingsTab)(((unsigned)state->active_tab + 1) % 4);
            return true;
        }
        if (event.is_character()) {
            const char ch = event.character()[0];
            if (ch >= '1' && ch <= '4') {
                state->active_tab = (SettingsTab)(unsigned)(ch - '1');
                return true;
            }
        }

        // Global close
        if (event == Event::Escape) {
            if (state->kb_adding) { state->kb_adding = false; return true; }
            if (state->inputs.on_close) state->inputs.on_close();
            return true;
        }

        // Ctrl+S → save all globals + per-plugin
        if (event.input() == "ctrl+s") {
            if (state->inputs.on_save_globals)
                state->inputs.on_save_globals(state->inputs.globals);
            for (const auto& p : state->inputs.per_plugin) {
                if (state->inputs.on_save_plugin)
                    state->inputs.on_save_plugin(p.plugin_id, p);
            }
            return true;
        }

        // --- Per-tab handlers ------------------------------------------------
        switch (state->active_tab) {
            // --- Global tab ---
            case SettingsTab::Global: {
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    state->global_selected = std::max(0, state->global_selected - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    state->global_selected = std::min(4, state->global_selected + 1);
                    return true;
                }
                if (event == Event::Character(' ')) {
                    auto& g = state->inputs.globals;
                    switch (state->global_selected) {
                        case 0: g.auto_update_all = !g.auto_update_all; break;
                        case 1: {
                            // Cycle default scope
                            if (g.default_install_scope == "user")    g.default_install_scope = "project";
                            else if (g.default_install_scope == "project") g.default_install_scope = "local";
                            else g.default_install_scope = "user";
                            break;
                        }
                        case 2: g.allow_unverified = !g.allow_unverified; break;
                        case 3: g.allow_unsafe_permissions = !g.allow_unsafe_permissions; break;
                        case 4: g.show_trust_warning_on_install = !g.show_trust_warning_on_install; break;
                    }
                    return true;
                }
                if (event == Event::Return) {
                    if (state->inputs.on_save_globals)
                        state->inputs.on_save_globals(state->inputs.globals);
                    return true;
                }
                return false;
            }

            // --- Plugins tab ---
            case SettingsTab::Plugins: {
                const int n = (int)state->inputs.per_plugin.size();
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    state->plugin_selected = std::max(0, state->plugin_selected - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    state->plugin_selected = std::min(std::max(0, n - 1),
                                                      state->plugin_selected + 1);
                    return true;
                }
                if (n == 0) return false;
                auto& p = state->inputs.per_plugin[state->plugin_selected];
                state->focused_plugin_index = (std::size_t)state->plugin_selected;

                if (event == Event::Character(' ')) {
                    p.enabled = !p.enabled;
                    if (state->inputs.on_save_plugin)
                        state->inputs.on_save_plugin(p.plugin_id, p);
                    return true;
                }
                if (event == Event::Character('u') || event == Event::Character('U')) {
                    p.auto_update = !p.auto_update;
                    return true;
                }
                if (event == Event::ArrowRight) {
                    state->active_tab = SettingsTab::Options;
                    return true;
                }
                return false;
            }

            // --- Options tab ---
            case SettingsTab::Options: {
                if (event == Event::ArrowLeft) {
                    state->active_tab = SettingsTab::Plugins;
                    return true;
                }
                if (!state->focused_plugin_index) return false;
                auto& p = state->inputs.per_plugin[*state->focused_plugin_index];
                const int n = (int)p.options.size();
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    state->option_selected = std::max(0, state->option_selected - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    state->option_selected = std::min(std::max(0, n - 1),
                                                      state->option_selected + 1);
                    return true;
                }
                return false;
            }

            // --- Keybindings tab ---
            case SettingsTab::Keybindings: {
                if (event == Event::ArrowLeft) {
                    state->active_tab = SettingsTab::Plugins;
                    return true;
                }
                if (!state->focused_plugin_index) return false;
                auto& p = state->inputs.per_plugin[*state->focused_plugin_index];

                // Adding new keybinding
                if (state->kb_adding) {
                    if (event == Event::Return) {
                        if (!state->kb_new_name.empty() && !state->kb_new_hotkey.empty()) {
                            KeybindingEntry kb{
                                .name = state->kb_new_name,
                                .hotkey = state->kb_new_hotkey,
                                .scope = "global",
                                .enabled = true,
                            };
                            p.keybindings.push_back(kb);
                            if (state->inputs.on_update_keybinding)
                                state->inputs.on_update_keybinding(p.plugin_id, kb);
                            state->kb_adding = false;
                            state->kb_new_name.clear();
                            state->kb_new_hotkey.clear();
                        }
                        return true;
                    }
                    // Switch field: Tab
                    if (event == Event::Tab) {
                        state->kb_edit_field = 1 - state->kb_edit_field;
                        return true;
                    }
                    // Type into the active field
                    if (event.is_character()) {
                        if (state->kb_edit_field == 0)
                            state->kb_new_name.push_back(event.character()[0]);
                        else
                            state->kb_new_hotkey.push_back(event.character()[0]);
                        return true;
                    }
                    if (event == Event::Backspace) {
                        if (state->kb_edit_field == 0 && !state->kb_new_name.empty())
                            state->kb_new_name.pop_back();
                        else if (state->kb_edit_field == 1 && !state->kb_new_hotkey.empty())
                            state->kb_new_hotkey.pop_back();
                        return true;
                    }
                    return false;
                }

                // Navigate existing list
                const int n = (int)p.keybindings.size();
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    state->keybinding_selected =
                        std::max(0, state->keybinding_selected - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    state->keybinding_selected =
                        std::min(std::max(0, n - 1),
                                 state->keybinding_selected + 1);
                    return true;
                }
                // Add
                if (event == Event::Character('a') || event == Event::Character('A')) {
                    state->kb_adding = true;
                    state->kb_edit_field = 0;
                    state->kb_new_name.clear();
                    state->kb_new_hotkey.clear();
                    return true;
                }
                if (n == 0) return false;
                auto& kb = p.keybindings[state->keybinding_selected];
                // Toggle enabled
                if (event == Event::Character(' ')) {
                    kb.enabled = !kb.enabled;
                    if (state->inputs.on_update_keybinding)
                        state->inputs.on_update_keybinding(p.plugin_id, kb);
                    return true;
                }
                // Remove
                if (event == Event::Character('r') || event == Event::Character('R')) {
                    const std::string name = kb.name;
                    p.keybindings.erase(p.keybindings.begin() + state->keybinding_selected);
                    state->keybinding_selected = std::max(
                        0, state->keybinding_selected - 1);
                    if (state->inputs.on_remove_keybinding)
                        state->inputs.on_remove_keybinding(p.plugin_id, name);
                    return true;
                }
                return false;
            }
        }
        return false;
    });
}

} // namespace cc::ui::plugins::plugin_settings_dialog
