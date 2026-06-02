module;
#include <string>

export module cc.hooks.notifs.fast_mode;

export namespace cc::hooks::notifs {

namespace detail {
    // 追踪快速模式通知是否已展示
    inline bool& fast_mode_shown_flag() {
        static bool shown = false;
        return shown;
    }
} // namespace detail

// 判断是否应该展示快速模式通知（仅首次进入时展示）
inline bool should_show_fast_mode_notification() {
    return !detail::fast_mode_shown_flag();
}

// 标记快速模式通知已展示，后续不再重复
inline void mark_fast_mode_notification_shown() {
    detail::fast_mode_shown_flag() = true;
}

// 展示快速模式通知，提醒用户当前处于快速模式
inline void show_fast_mode_notification() {
    if (should_show_fast_mode_notification()) {
        // 输出快速模式提示信息
        mark_fast_mode_notification_shown();
    }
}

} // namespace cc::hooks::notifs
