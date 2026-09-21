/// @file shortcut_display.cppm
/// @brief Shortcut display with observer-based reactivity.
/// Migrated from src/keybindings/useShortcutDisplay.ts
///
/// In TypeScript this was a React hook (useShortcutDisplay). In C++ we use
/// an observer/subscription pattern to achieve the same reactive behavior:
/// when keybindings change, registered observers are notified and can
/// retrieve updated display text.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <atomic>

export module cc.keybindings.shortcut_display;

import cc.keybindings.schema;
import cc.keybindings.shortcut_format;

export namespace cc::keybindings {

/// Reason why fallback was used
enum class FallbackReason {
    no_context,      // No keybinding context available
    action_not_found // Action not found in bindings
};

/// Callback for logging fallback events
using FallbackLogCallback = std::function<void(
    std::string_view action,
    std::string_view context,
    std::string_view fallback,
    FallbackReason reason
)>;

/// Observer interface for shortcut display changes
class ShortcutDisplayObserver {
public:
    virtual ~ShortcutDisplayObserver() = default;

    /// Called when the resolved display text changes
    virtual void on_display_changed(std::string_view new_display) = 0;
};

/// Keybinding context that provides display text resolution.
/// Equivalent to the React KeybindingContext provider.
class KeybindingDisplayContext {
    std::vector<Keybinding> bindings_;
    mutable std::mutex mutex_;

public:
    explicit KeybindingDisplayContext(std::vector<Keybinding> bindings)
        : bindings_(std::move(bindings)) {}

    /// Update the bindings (e.g., after hot-reload)
    void update_bindings(std::vector<Keybinding> new_bindings) {
        std::lock_guard lock(mutex_);
        bindings_ = std::move(new_bindings);
    }

    /// Get display text for an action in a given context
    [[nodiscard]] std::optional<std::string> get_display_text(
        std::string_view action,
        std::string_view context
    ) const {
        std::lock_guard lock(mutex_);
        return get_binding_display_text(action, context, bindings_);
    }

    /// Get current bindings snapshot
    [[nodiscard]] std::vector<Keybinding> get_bindings() const {
        std::lock_guard lock(mutex_);
        return bindings_;
    }
};

/// A reactive shortcut display binding that tracks a single action.
/// Replaces the React useShortcutDisplay hook with an observer pattern.
///
/// Usage:
///   auto display = ShortcutDisplay(context, "app:toggleTranscript", "Global", "ctrl+o");
///   std::string text = display.get();  // Returns resolved text or fallback
///   display.set_observer(my_observer); // Get notified on changes
class ShortcutDisplay {
    std::shared_ptr<KeybindingDisplayContext> context_;
    std::string action_;
    std::string keybinding_context_;
    std::string fallback_;
    ShortcutDisplayObserver* observer_ = nullptr;
    std::atomic<bool> has_logged_fallback_{false};
    FallbackLogCallback log_fn_;

public:
    /// Construct a shortcut display tracker
    /// @param context   Shared keybinding display context (may be nullptr)
    /// @param action    Action name to resolve (e.g., "app:toggleTranscript")
    /// @param kb_context Keybinding context name (e.g., "Global")
    /// @param fallback  Fallback text if binding is unavailable
    /// @param log_fn    Optional callback for telemetry logging
    ShortcutDisplay(
        std::shared_ptr<KeybindingDisplayContext> context,
        std::string action,
        std::string kb_context,
        std::string fallback,
        FallbackLogCallback log_fn = nullptr
    )
        : context_(std::move(context))
        , action_(std::move(action))
        , keybinding_context_(std::move(kb_context))
        , fallback_(std::move(fallback))
        , log_fn_(std::move(log_fn))
    {}

    /// Get the current display text (resolved binding or fallback)
    [[nodiscard]] std::string get() const {
        if (!context_) {
            log_fallback(FallbackReason::no_context);
            return fallback_;
        }

        auto resolved = context_->get_display_text(action_, keybinding_context_);
        if (!resolved.has_value()) {
            log_fallback(FallbackReason::action_not_found);
            return fallback_;
        }

        return *resolved;
    }

    /// Set an observer to be notified when display text changes
    void set_observer(ShortcutDisplayObserver* obs) {
        observer_ = obs;
    }

    /// Notify this display that bindings have been reloaded.
    /// Call this when the KeybindingDisplayContext is updated.
    void refresh() {
        if (observer_) {
            observer_->on_display_changed(get());
        }
    }

    /// Get the action being tracked
    [[nodiscard]] std::string_view action() const { return action_; }

    /// Get the fallback text
    [[nodiscard]] std::string_view fallback() const { return fallback_; }

private:
    /// Log fallback usage at most once per lifetime (mirrors React useRef behavior)
    void log_fallback(FallbackReason reason) const {
        if (!log_fn_) return;
        // Use atomic to ensure we only log once (mirrors hasLoggedRef in React)
        bool expected = false;
        if (const_cast<std::atomic<bool>&>(has_logged_fallback_)
                .compare_exchange_strong(expected, true)) {
            log_fn_(action_, keybinding_context_, fallback_, reason);
        }
    }
};

/// Registry that manages multiple ShortcutDisplay instances and propagates
/// binding changes to all of them. Equivalent to a React context provider
/// that re-renders all consuming components on state change.
class ShortcutDisplayRegistry {
    std::shared_ptr<KeybindingDisplayContext> context_;
    std::vector<ShortcutDisplay*> displays_;
    mutable std::mutex mutex_;

public:
    explicit ShortcutDisplayRegistry(std::vector<Keybinding> initial_bindings)
        : context_(std::make_shared<KeybindingDisplayContext>(std::move(initial_bindings)))
    {}

    /// Get the shared context
    [[nodiscard]] std::shared_ptr<KeybindingDisplayContext> context() const {
        return context_;
    }

    /// Register a display for change notifications
    void register_display(ShortcutDisplay* display) {
        std::lock_guard lock(mutex_);
        displays_.push_back(display);
    }

    /// Unregister a display
    void unregister_display(ShortcutDisplay* display) {
        std::lock_guard lock(mutex_);
        std::erase(displays_, display);
    }

    /// Update bindings and notify all registered displays
    void update_bindings(std::vector<Keybinding> new_bindings) {
        context_->update_bindings(std::move(new_bindings));

        std::lock_guard lock(mutex_);
        for (auto* display : displays_) {
            display->refresh();
        }
    }
};

} // namespace cc::keybindings
