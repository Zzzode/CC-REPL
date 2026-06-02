// CC-REPL: Terminal UI Event System
// Migrated from src/ink/events/ (click-event.ts, dispatcher.ts, emitter.ts,
// event-handlers.ts, event.ts, focus-event.ts, input-event.ts,
// keyboard-event.ts, terminal-event.ts, terminal-focus-event.ts)

module;

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <variant>
#include <optional>
#include <memory>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <algorithm>

export module cc.ui.event_system;

export namespace cc::ui::events {

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class EventType : std::uint8_t {
    KeyPress,
    MouseClick,
    MouseScroll,
    MouseMove,
    Focus,
    Blur,
    Resize,
    Paste,
    Custom
};

enum class MouseButton : std::uint8_t {
    Left,
    Right,
    Middle,
    ScrollUp,
    ScrollDown
};

// ---------------------------------------------------------------------------
// Event Data Structs
// ---------------------------------------------------------------------------

struct KeyModifiers {
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    bool meta = false;
};

struct KeyEvent {
    std::string key;
    KeyModifiers modifiers{};
    bool is_special = false;
};

struct MouseEvent {
    int x = 0;
    int y = 0;
    MouseButton button = MouseButton::Left;
    KeyModifiers modifiers{};
    bool pressed = false;
};

struct FocusEvent {
    bool focused = false;
};

struct ResizeEvent {
    int width = 0;
    int height = 0;
};

struct PasteEvent {
    std::string content;
};

// ---------------------------------------------------------------------------
// Event variant & handler types
// ---------------------------------------------------------------------------

using Event = std::variant<KeyEvent, MouseEvent, FocusEvent, ResizeEvent, PasteEvent>;

/// Returns true if the event was consumed.
using EventHandler = std::function<bool(const Event&)>;

struct EventSubscription {
    std::uint64_t id = 0;
    EventType type = EventType::Custom;
    EventHandler handler;
    int priority = 0;
};

// ---------------------------------------------------------------------------
// EventDispatcher
// ---------------------------------------------------------------------------

class EventDispatcher {
public:
    EventDispatcher() = default;

    /// Subscribe a handler for the given event type.
    /// Higher priority handlers are invoked first.
    /// Returns a subscription id for later unsubscription.
    inline auto subscribe(EventType type, EventHandler handler, int priority = 0)
        -> std::uint64_t {
        auto id = next_id_++;
        subscriptions_.push_back(EventSubscription{
            .id = id,
            .type = type,
            .handler = std::move(handler),
            .priority = priority,
        });
        // Keep sorted by descending priority for dispatch order.
        std::sort(subscriptions_.begin(), subscriptions_.end(),
                  [](const EventSubscription& a, const EventSubscription& b) {
                      return a.priority > b.priority;
                  });
        return id;
    }

    /// Remove a subscription by id.
    inline void unsubscribe(std::uint64_t subscription_id) {
        std::erase_if(subscriptions_, [subscription_id](const EventSubscription& s) {
            return s.id == subscription_id;
        });
    }

    /// Dispatch an event to all matching handlers (by priority order).
    /// Returns true if any handler consumed the event.
    inline auto dispatch(const Event& event) -> bool {
        auto type = event_type_for(event);
        for (auto& sub : subscriptions_) {
            if (sub.type == type && sub.handler) {
                if (sub.handler(event)) {
                    return true;
                }
            }
        }
        return false;
    }

    /// Remove all subscriptions.
    inline void clear() {
        subscriptions_.clear();
    }

private:
    std::vector<EventSubscription> subscriptions_;
    std::uint64_t next_id_ = 1;

    static inline auto event_type_for(const Event& event) -> EventType {
        return std::visit([](const auto& e) -> EventType {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, KeyEvent>) {
                return EventType::KeyPress;
            } else if constexpr (std::is_same_v<T, MouseEvent>) {
                return EventType::MouseClick;
            } else if constexpr (std::is_same_v<T, FocusEvent>) {
                return EventType::Focus;
            } else if constexpr (std::is_same_v<T, ResizeEvent>) {
                return EventType::Resize;
            } else if constexpr (std::is_same_v<T, PasteEvent>) {
                return EventType::Paste;
            } else {
                return EventType::Custom;
            }
        }, event);
    }
};

// ---------------------------------------------------------------------------
// EventEmitter
// ---------------------------------------------------------------------------

class EventEmitter {
public:
    EventEmitter() = default;

    /// Register a handler. Returns subscription id.
    inline auto on(EventType type, EventHandler handler) -> std::uint64_t {
        return dispatcher_.subscribe(type, std::move(handler), 0);
    }

    /// Unregister a handler by subscription id.
    inline void off(std::uint64_t id) {
        dispatcher_.unsubscribe(id);
    }

    /// Emit an event to all registered handlers.
    inline void emit(const Event& event) {
        dispatcher_.dispatch(event);
        // Clean up one-shot handlers that have fired.
        for (auto id : pending_removal_) {
            dispatcher_.unsubscribe(id);
        }
        pending_removal_.clear();
    }

    /// Register a handler that fires only once, then auto-removes itself.
    inline auto once(EventType type, EventHandler handler) -> std::uint64_t {
        // Use shared_ptr to capture the subscription id after registration.
        auto sub_id_holder = std::make_shared<std::uint64_t>(0);
        auto wrapped = [this, sub_id_holder, h = std::move(handler)](const Event& e) -> bool {
            bool result = h(e);
            pending_removal_.push_back(*sub_id_holder);
            return result;
        };
        auto id = dispatcher_.subscribe(type, std::move(wrapped), 0);
        *sub_id_holder = id;
        return id;
    }

private:
    EventDispatcher dispatcher_;
    std::vector<std::uint64_t> pending_removal_;
};

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

/// Create a KeyEvent wrapped in an Event variant.
inline auto make_key_event(std::string_view key, KeyModifiers modifiers = {}) -> Event {
    return KeyEvent{
        .key = std::string(key),
        .modifiers = modifiers,
        .is_special = (!key.empty() && key.size() > 1),
    };
}

/// Create a MouseEvent wrapped in an Event variant.
inline auto make_mouse_event(int x, int y, MouseButton button, bool pressed) -> Event {
    return MouseEvent{
        .x = x,
        .y = y,
        .button = button,
        .modifiers = {},
        .pressed = pressed,
    };
}

/// Check if a key event represents a printable character.
inline auto is_printable(const KeyEvent& event) -> bool {
    if (event.is_special) return false;
    if (event.modifiers.ctrl || event.modifiers.alt || event.modifiers.meta) return false;
    if (event.key.empty()) return false;
    // Single printable ASCII character.
    if (event.key.size() == 1) {
        char c = event.key[0];
        return c >= 0x20 && c <= 0x7E;
    }
    // Multi-byte UTF-8 character is considered printable.
    return event.key.size() <= 4 && static_cast<unsigned char>(event.key[0]) >= 0x80;
}

/// Get a human-readable name for an event type.
inline auto event_type_name(EventType type) -> std::string_view {
    switch (type) {
        case EventType::KeyPress:    return "KeyPress";
        case EventType::MouseClick:  return "MouseClick";
        case EventType::MouseScroll: return "MouseScroll";
        case EventType::MouseMove:   return "MouseMove";
        case EventType::Focus:       return "Focus";
        case EventType::Blur:        return "Blur";
        case EventType::Resize:      return "Resize";
        case EventType::Paste:       return "Paste";
        case EventType::Custom:      return "Custom";
    }
    return "Unknown";
}

} // namespace cc::ui::events
