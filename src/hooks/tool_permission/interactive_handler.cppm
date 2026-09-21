module;
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <string_view>

export module cc.hooks.tool_permission.interactive_handler;

export namespace cc::hooks::tool_permission {


enum class PermissionDecision {
    Allow,
    Deny,
    Ask
};

namespace detail {

    inline std::map<std::string, PermissionDecision>& permission_cache_store() {
        static std::map<std::string, PermissionDecision> cache;
        return cache;
    }
} // namespace detail


inline PermissionDecision handle_interactive_permission(
    std::string_view tool_name,
    std::string_view description
) {

    auto& cache = detail::permission_cache_store();
    std::string key(tool_name);
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }


    (void)description;
    return PermissionDecision::Ask;
}


inline bool prompt_user_permission(std::string_view message) {
    // Non-interactive mode: log the permission request and deny by default.
    // In an interactive session, a UI layer would override this.
    std::fprintf(stderr, "[Permission] %.*s — denied (non-interactive)\n",
                 static_cast<int>(message.size()), message.data());
    return false;
}


inline std::map<std::string, PermissionDecision> get_permission_cache() {
    return detail::permission_cache_store();
}


inline void cache_permission(std::string_view key, PermissionDecision decision) {
    detail::permission_cache_store()[std::string(key)] = decision;
}

} // namespace cc::hooks::tool_permission
