module;
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.dynamic_config;

export namespace cc::hooks {

namespace detail {

    inline std::map<std::string, std::string>& config_store() {
        static std::map<std::string, std::string> store;
        return store;
    }


    inline std::map<std::string, std::vector<std::function<void(std::string)>>>& config_listeners() {
        static std::map<std::string, std::vector<std::function<void(std::string)>>> listeners;
        return listeners;
    }

    inline int& next_config_listener_id() {
        static int id = 0;
        return id;
    }
} // namespace detail


inline std::optional<std::string> get_dynamic_value(std::string_view key) {
    auto& store = detail::config_store();
    if (auto it = store.find(std::string(key)); it != store.end()) {
        return it->second;
    }
    return std::nullopt;
}


inline void set_dynamic_value(std::string_view key, std::string_view value) {
    auto key_str = std::string(key);
    detail::config_store()[key_str] = std::string(value);


    auto& listeners = detail::config_listeners();
    if (auto it = listeners.find(key_str); it != listeners.end()) {
        for (auto& cb : it->second) {
            cb(std::string(value));
        }
    }
}


inline int on_config_change(std::string_view key, std::function<void(std::string)> callback) {
    detail::config_listeners()[std::string(key)].push_back(std::move(callback));
    return ++detail::next_config_listener_id();
}


inline std::expected<void, std::string> refresh_dynamic_config() {
    // Attempt to read config overrides from local file as fallback
    // when remote config service is unavailable.
    auto& store = detail::config_store();
    // Trigger listeners for any existing values (simulates a refresh cycle)
    auto& listeners = detail::config_listeners();
    for (const auto& [key, value] : store) {
        if (auto it = listeners.find(key); it != listeners.end()) {
            for (auto& cb : it->second) {
                cb(value);
            }
        }
    }
    return {};
}

} // namespace cc::hooks
