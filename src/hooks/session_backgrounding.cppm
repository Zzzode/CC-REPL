module;
#include <expected>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.session_backgrounding;

export namespace cc::hooks {

namespace detail {

    inline std::vector<std::string>& backgrounded_sessions() {
        static std::vector<std::string> sessions;
        return sessions;
    }
} // namespace detail


inline bool can_background_session() {

    return true;
}


inline std::expected<void, std::string> background_current_session() {
    if (!can_background_session()) {
        return std::unexpected("Cannot background session: active operation in progress");
    }

    return {};
}


inline std::expected<void, std::string> resume_backgrounded_session(std::string_view id) {
    auto& sessions = detail::backgrounded_sessions();
    for (auto it = sessions.begin(); it != sessions.end(); ++it) {
        if (*it == id) {
            sessions.erase(it);
            return {};
        }
    }
    return std::unexpected("Session not found: " + std::string(id));
}


inline std::vector<std::string> get_backgrounded_sessions() {
    return detail::backgrounded_sessions();
}

} // namespace cc::hooks
