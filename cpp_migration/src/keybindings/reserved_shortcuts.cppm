/// @file reserved_shortcuts.cppm
/// @brief Reserved keyboard shortcuts that cannot be overridden.
/// Migrated from src/keybindings/reservedShortcuts.ts - defines shortcuts that cannot be overridden
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <span>

export module cc.keybindings.reserved_shortcuts;

export namespace cc::keybindings::reserved {

// ============================================================
// Types
// ============================================================

/// A shortcut reserved by the system
struct ReservedShortcut {
    std::string key_combo;
    std::string description;
    std::string context;
};

// ============================================================
// Functions
// ============================================================

/// Get all reserved shortcuts
[[nodiscard]] inline std::span<const ReservedShortcut> get_reserved_shortcuts() {
    static const std::vector<ReservedShortcut> shortcuts = {
        {"Ctrl+C", "Interrupt / Copy", "global"},
        {"Ctrl+D", "End of input / Exit", "global"},
        {"Ctrl+Z", "Suspend process", "global"},
        {"Ctrl+\\", "Quit (SIGQUIT)", "global"},
        {"Ctrl+S", "Pause terminal output (XOFF)", "terminal"},
        {"Ctrl+Q", "Resume terminal output (XON)", "terminal"},
        {"Ctrl+L", "Clear screen", "terminal"},
        {"Ctrl+A", "Move to beginning of line", "readline"},
        {"Ctrl+E", "Move to end of line", "readline"},
        {"Ctrl+K", "Kill to end of line", "readline"},
        {"Ctrl+U", "Kill to beginning of line", "readline"},
        {"Ctrl+W", "Kill previous word", "readline"},
        {"Ctrl+R", "Reverse history search", "readline"},
        {"Ctrl+P", "Previous history entry", "readline"},
        {"Ctrl+N", "Next history entry", "readline"},
    };
    return std::span<const ReservedShortcut>(shortcuts);
}

/// Check if a key combo is reserved
[[nodiscard]] inline bool is_reserved(std::string_view key_combo) {
    for (const auto& shortcut : get_reserved_shortcuts()) {
        if (shortcut.key_combo == key_combo) {
            return true;
        }
    }
    return false;
}

/// Get the reason a key combo is reserved, or nullopt if not reserved
[[nodiscard]] inline std::optional<std::string_view> get_reserved_reason(std::string_view key_combo) {
    for (const auto& shortcut : get_reserved_shortcuts()) {
        if (shortcut.key_combo == key_combo) {
            return std::string_view(shortcut.description);
        }
    }
    return std::nullopt;
}

} // namespace cc::keybindings::reserved
