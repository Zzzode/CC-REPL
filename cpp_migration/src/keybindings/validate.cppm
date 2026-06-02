/// @file validate.cppm
/// @brief Keybinding configuration validation.
/// Migrated from src/keybindings/validate.ts - validates keybinding configurations
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <span>

export module cc.keybindings.validate;

export namespace cc::keybindings::validate {

// ============================================================
// Types
// ============================================================

/// An error found during validation
struct ValidationError {
    std::string key_combo;
    std::string message;
    enum Level : std::uint8_t { Warning, Error } level;
};

/// Result of keybinding validation
struct ValidationResult {
    bool valid{true};
    std::vector<ValidationError> errors;
};

// ============================================================
// Functions
// ============================================================

/// Get the list of valid modifier names
[[nodiscard]] inline std::vector<std::string_view> get_valid_modifier_names() {
    return {"ctrl", "alt", "shift", "meta", "control", "option", "cmd", "command", "super"};
}

/// Check if a key name is valid
[[nodiscard]] inline bool is_valid_key_name(std::string_view key) {
    if (key.empty()) return false;

    // Single character keys
    if (key.size() == 1) {
        char c = key[0];
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    }

    // Named keys
    static const std::vector<std::string_view> valid_names = {
        "escape", "enter", "tab", "space", "backspace", "delete", "insert",
        "home", "end", "pageup", "pagedown",
        "up", "down", "left", "right",
        "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
        "capslock", "numlock", "scrolllock", "printscreen", "pause",
        "minus", "equal", "bracketleft", "bracketright", "backslash",
        "semicolon", "quote", "comma", "period", "slash", "backquote",
    };

    // Normalize to lowercase for comparison
    std::string normalized(key);
    for (auto& c : normalized) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (const auto& name : valid_names) {
        if (normalized == name) return true;
    }

    // Check if it's a modifier (also valid as a key on its own)
    for (const auto& mod : get_valid_modifier_names()) {
        if (normalized == mod) return true;
    }

    return false;
}

/// Validate a single keybinding expression
[[nodiscard]] inline ValidationResult validate_keybinding(std::string_view key_combo) {
    ValidationResult result;

    if (key_combo.empty()) {
        result.valid = false;
        result.errors.push_back({
            std::string(key_combo), "Key combination is empty", ValidationError::Error});
        return result;
    }

    // Split on '+' and validate each part
    std::string combo(key_combo);
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while ((pos = combo.find('+')) != std::string::npos) {
        parts.push_back(combo.substr(0, pos));
        combo = combo.substr(pos + 1);
    }
    if (!combo.empty()) {
        parts.push_back(combo);
    }

    if (parts.empty()) {
        result.valid = false;
        result.errors.push_back({
            std::string(key_combo), "No key parts found", ValidationError::Error});
        return result;
    }

    // Last part should be the key, others should be modifiers
    auto& key_part = parts.back();
    std::string normalized_key(key_part);
    for (auto& c : normalized_key) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (!is_valid_key_name(normalized_key)) {
        result.valid = false;
        result.errors.push_back({
            std::string(key_combo),
            "Invalid key name: " + key_part,
            ValidationError::Error,
        });
    }

    // Check modifiers
    auto valid_mods = get_valid_modifier_names();
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string mod(parts[i]);
        for (auto& c : mod) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        bool found = false;
        for (const auto& valid : valid_mods) {
            if (mod == valid) { found = true; break; }
        }
        if (!found) {
            result.valid = false;
            result.errors.push_back({
                std::string(key_combo),
                "Invalid modifier: " + parts[i],
                ValidationError::Error,
            });
        }
    }

    return result;
}

/// Validate a set of keybinding configurations
[[nodiscard]] inline ValidationResult validate_keybinding_config(
    std::span<const std::pair<std::string, std::string>> bindings)
{
    ValidationResult result;

    for (const auto& [combo, command] : bindings) {
        auto single_result = validate_keybinding(combo);
        if (!single_result.valid) {
            result.valid = false;
            for (auto& err : single_result.errors) {
                result.errors.push_back(std::move(err));
            }
        }
        if (command.empty()) {
            result.valid = false;
            result.errors.push_back({
                combo, "Command must not be empty", ValidationError::Error});
        }
    }

    return result;
}

/// Suggest a fix for an invalid key combination
[[nodiscard]] inline std::optional<std::string> suggest_fix(std::string_view invalid_combo) {
    if (invalid_combo.empty()) return std::nullopt;

    std::string combo(invalid_combo);

    // Common typos: "crtl" -> "ctrl", "shfit" -> "shift"
    auto replace_all = [](std::string& s, std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    std::string lower = combo;
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    bool modified = false;
    if (lower.find("crtl") != std::string::npos) {
        replace_all(combo, "crtl", "Ctrl");
        replace_all(combo, "Crtl", "Ctrl");
        replace_all(combo, "CRTL", "Ctrl");
        modified = true;
    }
    if (lower.find("shfit") != std::string::npos) {
        replace_all(combo, "shfit", "Shift");
        replace_all(combo, "Shfit", "Shift");
        replace_all(combo, "SHFIT", "Shift");
        modified = true;
    }
    if (lower.find("mta") != std::string::npos) {
        replace_all(combo, "mta", "Meta");
        replace_all(combo, "Mta", "Meta");
        replace_all(combo, "MTA", "Meta");
        modified = true;
    }

    if (modified) return combo;
    return std::nullopt;
}

} // namespace cc::keybindings::validate
