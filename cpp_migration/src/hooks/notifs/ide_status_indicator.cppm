module;
#include <functional>
#include <string>
#include <vector>

export module cc.hooks.notifs.ide_status_indicator;

export namespace cc::hooks::notifs {

// IDE 连接状态枚举
enum class IdeStatus {
    Connected,
    Disconnected,
    Connecting,
    Error
};

namespace detail {
    // 状态变更监听器列表
    inline std::vector<std::function<void(IdeStatus)>>& status_listeners() {
        static std::vector<std::function<void(IdeStatus)>> listeners;
        return listeners;
    }

    // 当前 IDE 状态
    inline IdeStatus& current_status() {
        static IdeStatus s = IdeStatus::Disconnected;
        return s;
    }
} // namespace detail

// 获取当前 IDE 连接状态
inline IdeStatus get_ide_status() {
    return detail::current_status();
}

// 将 IDE 状态格式化为可显示的指示符字符串
inline std::string format_ide_status_indicator(IdeStatus status) {
    switch (status) {
        case IdeStatus::Connected:    return "[IDE: Connected]";
        case IdeStatus::Disconnected: return "[IDE: Disconnected]";
        case IdeStatus::Connecting:   return "[IDE: Connecting...]";
        case IdeStatus::Error:        return "[IDE: Error]";
    }
    return "[IDE: Unknown]";
}

// 注册 IDE 状态变更回调
inline void on_ide_status_change(std::function<void(IdeStatus)> callback) {
    detail::status_listeners().push_back(std::move(callback));
}

} // namespace cc::hooks::notifs
