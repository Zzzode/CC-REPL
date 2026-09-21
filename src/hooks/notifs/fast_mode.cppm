module;
#include <string>

export module cc.hooks.notifs.fast_mode;

export namespace cc::hooks::notifs {

namespace detail {

    inline bool& fast_mode_shown_flag() {
        static bool shown = false;
        return shown;
    }
} // namespace detail


inline bool should_show_fast_mode_notification() {
    return !detail::fast_mode_shown_flag();
}


inline void mark_fast_mode_notification_shown() {
    detail::fast_mode_shown_flag() = true;
}


inline void show_fast_mode_notification() {
    if (should_show_fast_mode_notification()) {

        mark_fast_mode_notification_shown();
    }
}

} // namespace cc::hooks::notifs
