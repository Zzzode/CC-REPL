/// @file notifications.cppm
/// @brief Notification context for queued messages and stats.
/// Migrated from src/context/notifications.tsx, QueuedMessageContext.tsx, stats.tsx
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <cstdint>

export module cc.context.notifications;

export namespace cc::context {

/// A notification entry
struct Notification {
    std::string id;
    std::string message;
    std::chrono::system_clock::time_point timestamp;
    bool read = false;
};

/// A queued user message (injected by tools or events)
struct QueuedMessage {
    std::string content;
    std::optional<std::string> source;  // e.g., "tool", "channel", "teammate"
};

/// Turn statistics for display
struct TurnStats {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_creation_tokens = 0;
    int cache_read_tokens = 0;
    int tool_use_count = 0;
    std::chrono::milliseconds duration{0};
    std::optional<double> cost_usd;
};

/// Notification context state
struct NotificationState {
    std::vector<Notification> notifications;
    std::vector<QueuedMessage> queued_messages;
    TurnStats last_turn_stats;
    
    [[nodiscard]] std::size_t unread_count() const {
        std::size_t count = 0;
        for (const auto& n : notifications) {
            if (!n.read) count++;
        }
        return count;
    }
};

/// Drain all queued messages
[[nodiscard]] inline std::vector<QueuedMessage> drain_queued_messages(NotificationState& state) {
    auto result = std::move(state.queued_messages);
    state.queued_messages.clear();
    return result;
}

} // namespace cc::context
