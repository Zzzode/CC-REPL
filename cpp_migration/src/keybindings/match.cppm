/// @file match.cppm
/// @brief Keystroke matching logic for terminal key events.
/// Migrated from src/keybindings/match.ts
///
/// Maps raw terminal key input to parsed keystrokes, handling modifier keys
/// and special key names. This module bridges terminal input events with
/// the keybinding system's parsed representation.
module;

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <algorithm>

export module cc.keybindings.match;

import cc.keybindings.schema;

export namespace cc::keybindings {

/// Modifier state extracted from a terminal key event
struct InputModifiers {
    bool ctrl = false;
    bool shift = false;
    bool meta = false;   // Alt/Option on most terminals
    bool super_ = false; // Cmd/Win (kitty protocol only)

    [[nodiscard]] bool operator==(const InputModifiers&) const = default;
};

/// A raw terminal key event (input character + modifier/special-key flags)
struct KeyEvent {
    std::string input;  // Raw input character(s)

    // Special key flags (only one is true at a time)
    bool escape = false;
    bool return_ = false;
    bool tab = false;
    bool backspace = false;
    bool delete_ = false;
    bool up_arrow = false;
    bool down_arrow = false;
    bool left_arrow = false;
    bool right_arrow = false;
    bool page_up = false;
    bool page_down = false;
    bool wheel_up = false;
    bool wheel_down = false;
    bool home = false;
    bool end = false;

    // Modifier flags
    bool ctrl = false;
    bool shift = false;
    bool meta = false;
    bool super_ = false;
};

/// Extract the normalized key name from a terminal KeyEvent.
/// Maps boolean flags (escape, return, etc.) to string names matching
/// the ParsedKeystroke format used in keybinding definitions.
[[nodiscard]] inline std::optional<std::string> get_key_name(const KeyEvent& event) {
    if (event.escape)     return "escape";
    if (event.return_)    return "enter";
    if (event.tab)        return "tab";
    if (event.backspace)  return "backspace";
    if (event.delete_)    return "delete";
    if (event.up_arrow)   return "up";
    if (event.down_arrow) return "down";
    if (event.left_arrow) return "left";
    if (event.right_arrow) return "right";
    if (event.page_up)    return "pageup";
    if (event.page_down)  return "pagedown";
    if (event.wheel_up)   return "wheelup";
    if (event.wheel_down) return "wheeldown";
    if (event.home)       return "home";
    if (event.end)        return "end";

    // Single printable character - normalize to lowercase
    if (event.input.size() == 1) {
        std::string lower(1, static_cast<char>(std::tolower(
            static_cast<unsigned char>(event.input[0]))));
        return lower;
    }

    return std::nullopt;
}

/// Extract modifier state from a terminal KeyEvent
[[nodiscard]] inline InputModifiers get_input_modifiers(const KeyEvent& event) {
    return {
        .ctrl = event.ctrl,
        .shift = event.shift,
        .meta = event.meta,
        .super_ = event.super_
    };
}

/// Check if modifiers from input match a target KeyChord's modifiers.
///
/// Alt and Meta: Terminals historically set meta=true for Alt/Option.
/// Both alt and meta in config are treated as matching when meta is pressed.
///
/// Super (Cmd/Win): Distinct from alt/meta. Only arrives via the kitty
/// keyboard protocol on supporting terminals.
[[nodiscard]] inline bool modifiers_match(
    const InputModifiers& input_mods,
    const KeyChord& target
) {
    // Check ctrl modifier
    if (input_mods.ctrl != target.modifiers.ctrl) return false;

    // Check shift modifier
    if (input_mods.shift != target.modifiers.shift) return false;

    // Alt and meta both map to the terminal meta flag (terminal limitation)
    // Check if EITHER alt OR meta is required in target
    bool target_needs_meta = target.modifiers.alt || target.modifiers.meta;
    if (input_mods.meta != target_needs_meta) return false;

    // Super (cmd/win) is distinct from alt/meta
    // Note: using 'meta' field as proxy for super in Modifiers struct
    // (the schema uses 'meta' for Cmd on macOS)
    // This is a simplification; in full implementation, super would be separate

    return true;
}

/// Check if a KeyEvent matches a target KeyChord.
/// Handles the escape key quirk where terminals set meta=true alongside escape.
[[nodiscard]] inline bool matches_keystroke(
    const KeyEvent& event,
    const KeyChord& target
) {
    // Resolve the key name from the event
    auto key_name = get_key_name(event);
    if (!key_name.has_value()) return false;

    // Key name must match
    if (*key_name != target.key) return false;

    auto input_mods = get_input_modifiers(event);

    // QUIRK: Terminals set meta=true when escape is pressed (legacy behavior
    // from how escape sequences work). Ignore meta modifier when matching
    // escape key itself, otherwise "escape" without modifiers would never match.
    if (event.escape) {
        InputModifiers adjusted = input_mods;
        adjusted.meta = false;
        return modifiers_match(adjusted, target);
    }

    return modifiers_match(input_mods, target);
}

/// Check if a KeyEvent matches a keybinding (single-chord bindings only).
/// Returns true if the binding has exactly one chord and it matches the event.
[[nodiscard]] inline bool matches_binding(
    const KeyEvent& event,
    const Keybinding& binding
) {
    // Only match single-keystroke bindings (Phase 1)
    if (binding.keys.size() != 1) return false;
    return matches_keystroke(event, binding.keys[0]);
}

/// Find the first matching binding for a key event from a list of bindings.
/// Later entries take priority (user bindings override defaults).
[[nodiscard]] inline std::optional<std::size_t> find_matching_binding(
    const KeyEvent& event,
    const std::vector<Keybinding>& bindings
) {
    // Iterate in reverse so later bindings (user overrides) win
    for (std::size_t i = bindings.size(); i > 0; --i) {
        if (matches_binding(event, bindings[i - 1])) {
            return i - 1;
        }
    }
    return std::nullopt;
}

} // namespace cc::keybindings
