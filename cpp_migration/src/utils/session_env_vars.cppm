module;
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

export module cc.utils.session_env_vars;

export namespace cc::utils {

namespace detail {
    inline std::mutex& env_mutex() {
        static std::mutex m;
        return m;
    }

    inline std::map<std::string, std::string>& session_env_map() {
        static std::map<std::string, std::string> m;
        return m;
    }
} // namespace detail

// Get all session-scoped environment variables
std::map<std::string, std::string> get_session_env() {
    std::lock_guard lock(detail::env_mutex());
    return detail::session_env_map();
}

// Set a session-scoped environment variable
void set_session_env(std::string_view key, std::string_view value) {
    std::lock_guard lock(detail::env_mutex());
    detail::session_env_map()[std::string(key)] = std::string(value);
}

// Export all session env vars to the current process environment
void export_session_env() {
    std::lock_guard lock(detail::env_mutex());
    for (auto& [key, value] : detail::session_env_map()) {
        setenv(key.c_str(), value.c_str(), 1);
    }
}

} // namespace cc::utils
