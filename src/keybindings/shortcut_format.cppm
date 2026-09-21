/// @file shortcut_format.cppm
/// @brief Shortcut display text retrieval for non-UI contexts.
/// Migrated from src/keybindings/shortcutFormat.ts
///
/// Provides get_shortcut_display() for use in non-UI contexts (commands,
/// services, etc.) where a React-like hook is not appropriate. This module
/// is kept separate from shortcut_display.cppm to avoid pulling in
/// observer/UI dependencies when not needed.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_set>
#include <functional>

export module cc.keybindings.shortcut_format;

import cc.keybindings.schema;
import cc.keybindings.defaults;

export namespace cc::keybindings {

/// Callback type for analytics/telemetry logging
using LogEventCallback = std::function<void(
    std::string_view event_name,
    std::string_view action,
    std::string_view context,
    std::string_view fallback,
    std::string_view reason
)>;

/// Tracks which action+context pairs have already logged a fallback event
/// to avoid duplicate analytics from repeated calls.
class FallbackTracker {
    std::unordered_set<std::string> logged_fallbacks_;

public:
    /// Check if a fallback has already been logged for this action+context
    [[nodiscard]] bool has_logged(std::string_view action, std::string_view context) const {
        std::string key = std::string(action) + ":" + std::string(context);
        return logged_fallbacks_.contains(key);
    }

    /// Mark a fallback as logged
    void mark_logged(std::string_view action, std::string_view context) {
        std::string key = std::string(action) + ":" + std::string(context);
        logged_fallbacks_.insert(std::move(key));
    }

    /// Clear all tracked fallbacks (for testing)
    void clear() { logged_fallbacks_.clear(); }
};

/// Get the display text for a binding by searching through resolved bindings.
/// Returns the formatted chord string for the first binding matching the action+context,
/// or std::nullopt if no matching binding is found.
[[nodiscard]] inline std::optional<std::string> get_binding_display_text(
    std::string_view action,
    std::string_view context,
    const std::vector<Keybinding>& bindings
) {
    // Search in reverse order (later bindings override earlier ones)
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
        if (it->command == action && it->when.value_or("") == context) {
            // Format the key chord as display text
            if (it->keys.empty()) continue;
            const auto& chord = it->keys[0];

            std::string display;
            if (chord.modifiers.ctrl) display += "ctrl+";
            if (chord.modifiers.alt) display += "alt+";
            if (chord.modifiers.shift) display += "shift+";
            if (chord.modifiers.meta) display += "cmd+";
            display += chord.key;
            return display;
        }
    }
    return std::nullopt;
}

/// Get the display text for a configured shortcut without UI framework hooks.
/// Use this in non-UI contexts (commands, services, background tasks, etc.).
///
/// @param action   The action name (e.g., "app:toggleTranscript")
/// @param context  The keybinding context (e.g., "Global")
/// @param fallback Fallback text if binding not found
/// @param bindings The current resolved bindings list
/// @param tracker  Fallback tracker for deduplicating telemetry
/// @param log_fn   Optional telemetry callback (nullptr to skip logging)
/// @returns The configured shortcut display text, or fallback if not found
///
/// Example:
///   auto text = get_shortcut_display("app:toggleTranscript", "Global", "ctrl+o", bindings, tracker);
[[nodiscard]] inline std::string get_shortcut_display(
    std::string_view action,
    std::string_view context,
    std::string_view fallback,
    const std::vector<Keybinding>& bindings,
    FallbackTracker& tracker,
    LogEventCallback log_fn = nullptr
) {
    auto resolved = get_binding_display_text(action, context, bindings);

    if (!resolved.has_value()) {
        // Log fallback usage at most once per action+context pair
        if (log_fn && !tracker.has_logged(action, context)) {
            tracker.mark_logged(action, context);
            log_fn(
                "tengu_keybinding_fallback_used",
                action,
                context,
                fallback,
                "action_not_found"
            );
        }
        return std::string(fallback);
    }

    return *resolved;
}

} // namespace cc::keybindings
