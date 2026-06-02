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
// 通知类型与配置
// ============================================================

// 通知渠道
enum class NotifyChannel : std::uint8_t {
    Desktop,       // 桌面推送通知
    Sound,         // 声音提示
    TerminalBell,  // 终端响铃 (\a)
    Silent,        // 静默（仅记录）
};

// 通知优先级
enum class NotifyPriority : std::uint8_t {
    Low,
    Normal,
    High,
    Urgent,
};

// 通知消息
struct Notification {
    std::string title;
    std::string body;
    NotifyPriority priority{NotifyPriority::Normal};
    std::string category;    // 分类 (如 "task_complete", "error" 等)
    std::optional<std::string> action_url;
};

// 通知偏好设置
struct NotifyPreferences {
    bool desktop_enabled{true};
    bool sound_enabled{true};
    bool bell_enabled{false};
    NotifyPriority min_priority{NotifyPriority::Normal};  // 低于此优先级不通知
    std::vector<std::string> muted_categories;  // 静音的分类
    bool do_not_disturb{false};
};

// 通知结果
struct NotifyResult {
    bool delivered{false};
    NotifyChannel channel_used;
    std::string message;
};

// ============================================================
// NotifierService - 通知服务
// ============================================================

class NotifierService {
public:
    explicit NotifierService(NotifyPreferences prefs = {})
        : prefs_(std::move(prefs)) {}

    // 发送通知
    [[nodiscard]] std::expected<NotifyResult, Error> notify(const Notification& notif) const {
        // 检查免打扰模式
        if (prefs_.do_not_disturb) {
            return NotifyResult{false, NotifyChannel::Silent, "do not disturb"};
        }
        // 检查优先级过滤
        if (notif.priority < prefs_.min_priority) {
            return NotifyResult{false, NotifyChannel::Silent, "below priority threshold"};
        }
        // 检查分类静音
        for (const auto& muted : prefs_.muted_categories) {
            if (muted == notif.category) {
                return NotifyResult{false, NotifyChannel::Silent, "category muted"};
            }
        }
        // 选择通知渠道并投递
        if (prefs_.desktop_enabled) return send_desktop(notif);
        if (prefs_.sound_enabled) return send_sound(notif);
        if (prefs_.bell_enabled) return send_bell(notif);
        return NotifyResult{false, NotifyChannel::Silent, "all channels disabled"};
    }

    // 偏好管理
    void set_preferences(NotifyPreferences prefs) noexcept { prefs_ = std::move(prefs); }
    [[nodiscard]] const NotifyPreferences& preferences() const noexcept { return prefs_; }

    // 便捷方法
    void mute_category(std::string category) {
        prefs_.muted_categories.push_back(std::move(category));
    }
    void set_dnd(bool enabled) noexcept { prefs_.do_not_disturb = enabled; }

private:
    NotifyPreferences prefs_;

    // 桌面通知 (占位: 实际需要 OS 集成)
    [[nodiscard]] std::expected<NotifyResult, Error> send_desktop(const Notification& notif) const {
        // macOS: osascript, Linux: notify-send, Windows: toast
        auto msg = std::format("[Desktop] {}: {}", notif.title, notif.body);
        return NotifyResult{true, NotifyChannel::Desktop, std::move(msg)};
    }

    // 声音提醒
    [[nodiscard]] std::expected<NotifyResult, Error> send_sound(const Notification& /*notif*/) const {
        // 实际实现会调用系统声音 API
        return NotifyResult{true, NotifyChannel::Sound, "sound played"};
    }

    // 终端响铃
    [[nodiscard]] std::expected<NotifyResult, Error> send_bell(const Notification& /*notif*/) const {
        // 输出 BEL 字符
        return NotifyResult{true, NotifyChannel::TerminalBell, "\\a"};
    }
};

} // namespace cc::services::notifier
