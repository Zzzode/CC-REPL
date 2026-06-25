module;
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <sstream>
#include <algorithm>

export module cc.ui.prompt.notifications;

export namespace cc::ui::prompt {

// A notification message displayed above or below the prompt
struct PromptNotification {
    std::string message;
    enum Level { Info, Warning, Error } level = Info;
    std::optional<std::chrono::seconds> ttl;
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
};

// Check if a notification has expired based on its TTL
inline auto is_notification_expired(const PromptNotification& notification,
                                     std::chrono::steady_clock::time_point now) -> bool {
    if (!notification.ttl.has_value()) {
        return false; // No TTL means persistent
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - notification.created_at);
    return elapsed >= notification.ttl.value();
}

// Render the notification bar showing active notifications
inline auto render_notification_bar(std::vector<PromptNotification> notifications,
                                     int width) -> std::string {
    if (notifications.empty()) return "";

    std::ostringstream out;
    auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < notifications.size(); ++i) {
        const auto& notif = notifications[i];

        // Skip expired notifications
        if (is_notification_expired(notif, now)) continue;

        // Level-specific styling
        const char* icon = "";
        const char* color = "\033[0m";
        switch (notif.level) {
            case PromptNotification::Info:
                icon = "ℹ";
                color = "\033[34m"; // blue
                break;
            case PromptNotification::Warning:
                icon = "⚠";
                color = "\033[33m"; // yellow
                break;
            case PromptNotification::Error:
                icon = "✗";
                color = "\033[31m"; // red
                break;
        }

        std::string message = notif.message;
        const int max_message_width = std::max(width - 2, 0);
        if (max_message_width > 0 &&
            static_cast<int>(message.size()) > max_message_width) {
            if (max_message_width > 3) {
                message = message.substr(
                    0, static_cast<std::size_t>(max_message_width - 3)) + "...";
            } else {
                message = message.substr(0, static_cast<std::size_t>(max_message_width));
            }
        }

        out << color << icon << " " << message << "\033[0m";

        if (i < notifications.size() - 1) {
            out << "\n";
        }
    }

    std::string result = out.str();
    return result;
}

} // namespace cc::ui::prompt
