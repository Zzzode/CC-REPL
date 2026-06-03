module;
#include <string>
#include <string_view>
#include <vector>
#include <map>

export module cc.hooks.notifs.install_messages;

export namespace cc::hooks::notifs {

namespace detail {

    inline std::map<std::string, std::string>& message_store() {
        static std::map<std::string, std::string> store;
        return store;
    }


    inline std::vector<std::string>& shown_ids() {
        static std::vector<std::string> ids;
        return ids;
    }
} // namespace detail


inline std::vector<std::string> get_pending_install_messages() {
    std::vector<std::string> pending;
    for (const auto& [id, msg] : detail::message_store()) {

        bool shown = false;
        for (const auto& sid : detail::shown_ids()) {
            if (sid == id) { shown = true; break; }
        }
        if (!shown) {
            pending.push_back(msg);
        }
    }
    return pending;
}


inline void mark_message_shown(std::string_view id) {
    detail::shown_ids().emplace_back(id);
}


inline void add_install_message(std::string_view id, std::string_view message) {
    detail::message_store().emplace(std::string(id), std::string(message));
}

} // namespace cc::hooks::notifs
