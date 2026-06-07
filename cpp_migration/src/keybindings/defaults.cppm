/// @file defaults.cppm
/// @brief Default keybinding definitions and reserved shortcuts.
/// Migrated from src/keybindings/defaultBindings.ts, reservedShortcuts.ts
module;

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

export module cc.keybindings.defaults;

import cc.keybindings.schema;

export namespace cc::keybindings {

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
        {.id = "navigate.up", .keys = {parse_key_chord("up")}, .command = "navigate_up"},
        {.id = "navigate.down", .keys = {parse_key_chord("down")}, .command = "navigate_down"},
        {.id = "navigate.page_up", .keys = {parse_key_chord("pageup")}, .command = "page_up"},
        {.id = "navigate.page_down", .keys = {parse_key_chord("pagedown")}, .command = "page_down"},
        
        // Editing
        {.id = "edit.submit", .keys = {parse_key_chord("enter")}, .command = "submit", .when = "inputFocused"},
        {.id = "edit.newline", .keys = {parse_key_chord("shift+enter")}, .command = "insert_newline", .when = "inputFocused"},
        {.id = "edit.cancel", .keys = {parse_key_chord("escape")}, .command = "cancel"},
        
        // Task management
        {.id = "task.background", .keys = {parse_key_chord("ctrl+b")}, .command = "background_task"},
        {.id = "task.interrupt", .keys = {parse_key_chord("ctrl+c")}, .command = "interrupt"},
        
        // Overlays
        {.id = "overlay.help", .keys = {parse_key_chord("ctrl+/")}, .command = "show_help"},
        {.id = "overlay.tasks", .keys = {parse_key_chord("ctrl+t")}, .command = "show_tasks"},
        {.id = "overlay.command_palette", .keys = {parse_key_chord("ctrl+k")}, .command = "command_palette"},
    };
}

/// Check if a shortcut string is reserved
[[nodiscard]] inline bool is_reserved(std::string_view shortcut) {
    std::string lower(shortcut);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return reserved_shortcuts().contains(lower);
}

} // namespace cc::keybindings
