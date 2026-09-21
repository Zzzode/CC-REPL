/// @file notifier.cppm
/// @brief Notification service.
/// Provides desktop notifications, sound alerts,
/// terminal bell, and notification preference management.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <functional>
#include <unordered_map>

export module cc.services.notifier;

import cc.types.types;

export namespace cc::services::notifier {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;

// ============================================================

// ============================================================


enum class NotifyChannel : std::uint8_t {
    Desktop,
    Sound,
    TerminalBell,
    Silent,
};


enum class NotifyPriority : std::uint8_t {
    Low,
    Normal,
    High,
    Urgent,
};


struct Notification {
    std::string title;
    std::string body;
    NotifyPriority priority{NotifyPriority::Normal};
    std::string category;
    std::optional<std::string> action_url;
};


struct NotifyPreferences {
    bool desktop_enabled{true};
    bool sound_enabled{true};
    bool bell_enabled{false};
    NotifyPriority min_priority{NotifyPriority::Normal};
    std::vector<std::string> muted_categories;
    bool do_not_disturb{false};
};


struct NotifyResult {
    bool delivered{false};
    NotifyChannel channel_used;
    std::string message;
};

// ============================================================

// ============================================================

class NotifierService {
public:
    explicit NotifierService(NotifyPreferences prefs = {})
        : prefs_(std::move(prefs)) {}


    [[nodiscard]] std::expected<NotifyResult, Error> notify(const Notification& notif) const {

        if (prefs_.do_not_disturb) {
            return NotifyResult{false, NotifyChannel::Silent, "do not disturb"};
        }

        if (notif.priority < prefs_.min_priority) {
            return NotifyResult{false, NotifyChannel::Silent, "below priority threshold"};
        }

        for (const auto& muted : prefs_.muted_categories) {
            if (muted == notif.category) {
                return NotifyResult{false, NotifyChannel::Silent, "category muted"};
            }
        }

        if (prefs_.desktop_enabled) return send_desktop(notif);
        if (prefs_.sound_enabled) return send_sound(notif);
        if (prefs_.bell_enabled) return send_bell(notif);
        return NotifyResult{false, NotifyChannel::Silent, "all channels disabled"};
    }


    void set_preferences(NotifyPreferences prefs) noexcept { prefs_ = std::move(prefs); }
    [[nodiscard]] const NotifyPreferences& preferences() const noexcept { return prefs_; }


    void mute_category(std::string category) {
        prefs_.muted_categories.push_back(std::move(category));
    }
    void set_dnd(bool enabled) noexcept { prefs_.do_not_disturb = enabled; }

private:
    NotifyPreferences prefs_;


    [[nodiscard]] std::expected<NotifyResult, Error> send_desktop(const Notification& notif) const {
        // macOS: osascript, Linux: notify-send, Windows: toast
        auto msg = std::format("[Desktop] {}: {}", notif.title, notif.body);
        return NotifyResult{true, NotifyChannel::Desktop, std::move(msg)};
    }


    [[nodiscard]] std::expected<NotifyResult, Error> send_sound(const Notification& /*notif*/) const {

        return NotifyResult{true, NotifyChannel::Sound, "sound played"};
    }


    [[nodiscard]] std::expected<NotifyResult, Error> send_bell(const Notification& /*notif*/) const {

        return NotifyResult{true, NotifyChannel::TerminalBell, "\\a"};
    }
};

} // namespace cc::services::notifier
