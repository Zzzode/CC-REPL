module;
#include <functional>
#include <string>
#include <vector>

export module cc.hooks.notifs.ide_status_indicator;

export namespace cc::hooks::notifs {


enum class IdeStatus {
    Connected,
    Disconnected,
    Connecting,
    Error
};

namespace detail {

    inline std::vector<std::function<void(IdeStatus)>>& status_listeners() {
        static std::vector<std::function<void(IdeStatus)>> listeners;
        return listeners;
    }


    inline IdeStatus& current_status() {
        static IdeStatus s = IdeStatus::Disconnected;
        return s;
    }
} // namespace detail


inline IdeStatus get_ide_status() {
    return detail::current_status();
}


inline std::string format_ide_status_indicator(IdeStatus status) {
    switch (status) {
        case IdeStatus::Connected:    return "[IDE: Connected]";
        case IdeStatus::Disconnected: return "[IDE: Disconnected]";
        case IdeStatus::Connecting:   return "[IDE: Connecting...]";
        case IdeStatus::Error:        return "[IDE: Error]";
    }
    return "[IDE: Unknown]";
}


inline void on_ide_status_change(std::function<void(IdeStatus)> callback) {
    detail::status_listeners().push_back(std::move(callback));
}

} // namespace cc::hooks::notifs
