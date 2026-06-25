/// @file defaults.cppm
/// @brief Default keybinding definitions and reserved shortcuts.
/// Migrated from src/keybindings/defaultBindings.ts, reservedShortcuts.ts
module;

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

export module cc.keybindings.defaults;

import cc.keybindings.schema;

export namespace cc::keybindings {

[[nodiscard]] inline Keybinding default_binding(
    std::string_view id,
    std::string_view key,
    std::string_view command,
    std::string_view when = {})
{
    Keybinding binding;
    binding.id = std::string(id);
    binding.keys = {parse_key_chord(key)};
    binding.command = std::string(command);
    if (!when.empty()) binding.when = std::string(when);
    return binding;
}

/// Reserved shortcuts that cannot be overridden by user bindings
[[nodiscard]] inline const std::unordered_set<std::string>& reserved_shortcuts() {
    static const std::unordered_set<std::string> shortcuts = {
        "ctrl+c",     // Interrupt/cancel
        "ctrl+d",     // EOF/exit
        "ctrl+z",     // Suspend (Unix)
        "ctrl+\\",    // Quit
        "ctrl+l",     // Clear (terminal)
        "enter",      // Submit
        "escape",     // Cancel/dismiss
        "tab",        // Autocomplete
    };
    return shortcuts;
}

/// Get the default keybindings
[[nodiscard]] inline std::vector<Keybinding> get_default_bindings() {
    return {
        // Navigation
        default_binding("navigate.up", "up", "navigate_up"),
        default_binding("navigate.down", "down", "navigate_down"),
        default_binding("navigate.page_up", "pageup", "page_up"),
        default_binding("navigate.page_down", "pagedown", "page_down"),
        
        // Editing
        default_binding("edit.submit", "enter", "submit", "inputFocused"),
        default_binding("edit.newline", "shift+enter", "insert_newline", "inputFocused"),
        default_binding("edit.cancel", "escape", "cancel"),
        
        // Task management
        default_binding("task.background", "ctrl+b", "background_task"),
        default_binding("task.interrupt", "ctrl+c", "interrupt"),
        
        // Overlays
        default_binding("overlay.help", "ctrl+/", "show_help"),
        default_binding("overlay.tasks", "ctrl+t", "show_tasks"),
        default_binding("overlay.command_palette", "ctrl+k", "command_palette"),
    };
}

/// Check if a shortcut string is reserved
[[nodiscard]] inline bool is_reserved(std::string_view shortcut) {
    std::string lower(shortcut);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return reserved_shortcuts().contains(lower);
}

} // namespace cc::keybindings
