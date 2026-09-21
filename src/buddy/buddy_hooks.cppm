/// @file buddy_hooks.cppm
/// Buddy notification hook and trigger detection.
module;

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <algorithm>

export module cc.buddy.buddy_hooks;

export namespace cc::buddy {

// -- Date/time helpers for buddy feature window ----------------------------
// Local date checks, not UTC -- 24h rolling wave across timezones.
// Teaser window: April 1-7, 2026 only. Command stays live forever after.

inline auto is_buddy_teaser_window(int year, int month, int day) -> bool {
    return year == 2026 && month == 4 && day <= 7;
}

inline auto is_buddy_live(int year, int month) -> bool {
    return year > 2026 || (year == 2026 && month >= 4);
}

// -- Buddy notification hook -----------------------------------------------
// Observer pattern for buddy state changes.
// In the TS version this is a React hook; here we use a simple
// listener-based approach.

struct BuddyNotification {
    std::string key;
    std::string text;
    std::string priority;    // "immediate" | "normal"
    int timeout_ms;
};

class BuddyNotificationHook {
public:
    using Listener = std::function<void(const BuddyNotification&)>;

    /// Register a listener for buddy notifications.
    auto add_listener(Listener listener) -> int {
        int id = next_id_++;
        listeners_.push_back({id, std::move(listener)});
        return id;
    }

    /// Remove a listener by its registration ID.
    auto remove_listener(int id) -> void {
        listeners_.erase(
            std::remove_if(listeners_.begin(), listeners_.end(),
                [id](const Entry& e) { return e.id == id; }),
            listeners_.end());
    }

    /// Fire a notification to all registered listeners.
    auto notify(const BuddyNotification& notification) -> void {
        for (const auto& entry : listeners_) {
            entry.listener(notification);
        }
    }

    /// Convenience: check whether the buddy teaser should be shown
    /// (no companion hatched yet, within teaser window, feature enabled).
    auto should_show_teaser(bool buddy_enabled,
                            bool has_companion,
                            int year, int month, int day) const -> bool {
        if (!buddy_enabled) return false;
        if (has_companion) return false;
        return is_buddy_teaser_window(year, month, day);
    }

    /// Show the /buddy teaser notification if conditions are met.
    auto show_teaser_if_needed(bool buddy_enabled,
                               bool has_companion,
                               int year, int month, int day) -> void {
        if (should_show_teaser(buddy_enabled, has_companion, year, month, day)) {
            BuddyNotification note{
                .key        = "buddy-teaser",
                .text       = "/buddy",
                .priority   = "immediate",
                .timeout_ms = 15000,
            };
            notify(note);
        }
    }

private:
    struct Entry {
        int id;
        Listener listener;
    };
    std::vector<Entry> listeners_;
    int next_id_ = 0;
};

// -- find_buddy_trigger_positions -------------------------------------------
// Locates /buddy command triggers in input text.

struct TriggerPosition {
    int start;
    int end;
};

inline auto find_buddy_trigger_positions(const std::string& text)
    -> std::vector<TriggerPosition>
{
    std::vector<TriggerPosition> triggers;
    const std::string needle = "/buddy";
    size_t pos = 0;
    while (pos < text.size()) {
        pos = text.find(needle, pos);
        if (pos == std::string::npos) break;
        size_t end_pos = pos + needle.size();
        // Word boundary check: next char must not be alphanumeric
        if (end_pos < text.size()) {
            char c = text[end_pos];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                pos = end_pos;
                continue;
            }
        }
        triggers.push_back(TriggerPosition{
            .start = static_cast<int>(pos),
            .end   = static_cast<int>(end_pos),
        });
        pos = end_pos;
    }
    return triggers;
}

} // namespace cc::buddy
