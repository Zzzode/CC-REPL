/// @file schema.cppm
/// @brief Keybinding schema and types.
/// Migrated from src/keybindings/schema.ts, parser.ts, match.ts
module;

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>
#include <variant>
#include <algorithm>

export module cc.keybindings.schema;

export namespace cc::keybindings {

/// Modifier keys
struct Modifiers {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;  // Cmd on macOS
    
    [[nodiscard]] bool operator==(const Modifiers&) const = default;
    [[nodiscard]] bool any() const noexcept {
        return ctrl || alt || shift || meta;
    }
};

/// A parsed key chord (e.g., "Ctrl+Shift+K")
struct KeyChord {
    std::string key;  // The base key name (lowercase)
    Modifiers modifiers;
    
    [[nodiscard]] bool operator==(const KeyChord&) const = default;
};

/// A keybinding definition
struct Keybinding {
    std::string id;             // Unique binding ID (e.g., "editor.save")
    std::vector<KeyChord> keys; // Key sequence (usually 1 chord)
    std::string command;        // Command to execute
    std::optional<std::string> when;  // Condition expression
    std::optional<std::string> args;  // Arguments to pass
};

/// Parse a key string like "ctrl+shift+k" into a KeyChord
[[nodiscard]] inline KeyChord parse_key_chord(std::string_view input) {
    KeyChord chord;
    std::string remaining(input);
    
    // Normalize to lowercase
    std::transform(remaining.begin(), remaining.end(), remaining.begin(), ::tolower);
    
    // Split on '+' and extract modifiers
    std::size_t pos = 0;
    while ((pos = remaining.find('+')) != std::string::npos) {
        auto part = remaining.substr(0, pos);
        if (part == "ctrl" || part == "control") chord.modifiers.ctrl = true;
        else if (part == "alt" || part == "option") chord.modifiers.alt = true;
        else if (part == "shift") chord.modifiers.shift = true;
        else if (part == "meta" || part == "cmd" || part == "command") chord.modifiers.meta = true;
        else break;  // Not a modifier, must be the key
        remaining = remaining.substr(pos + 1);
    }
    
    chord.key = remaining;
    return chord;
}

/// Check if a key event matches a keybinding chord
[[nodiscard]] inline bool matches_chord(const KeyChord& binding, const KeyChord& event) {
    return binding.key == event.key && binding.modifiers == event.modifiers;
}

} // namespace cc::keybindings
