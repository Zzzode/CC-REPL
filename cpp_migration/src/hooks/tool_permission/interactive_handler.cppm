module;
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <string_view>

export module cc.hooks.tool_permission.interactive_handler;

export namespace cc::hooks::tool_permission {

// 权限决策枚举
enum class PermissionDecision {
    Allow,
    Deny,
    Ask
};

namespace detail {
    // 权限缓存存储
    inline std::map<std::string, PermissionDecision>& permission_cache_store() {
        static std::map<std::string, PermissionDecision> cache;
        return cache;
    }
} // namespace detail

// 处理交互式权限请求（需要用户确认）
inline PermissionDecision handle_interactive_permission(
    std::string_view tool_name,
    std::string_view description
) {
    // 先检查缓存，避免重复询问
    auto& cache = detail::permission_cache_store();
    std::string key(tool_name);
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }

    // 需要向用户询问权限
    (void)description;
    return PermissionDecision::Ask;
}

// 向用户提示权限确认（返回用户是否同意）
inline bool prompt_user_permission(std::string_view message) {
    // Non-interactive mode: log the permission request and deny by default.
    // In an interactive session, a UI layer would override this.
    std::fprintf(stderr, "[Permission] %.*s — denied (non-interactive)\n",
                 static_cast<int>(message.size()), message.data());
    return false;
}

// 获取当前权限缓存
inline std::map<std::string, PermissionDecision> get_permission_cache() {
    return detail::permission_cache_store();
}

// 将权限决策存入缓存，避免重复询问
inline void cache_permission(std::string_view key, PermissionDecision decision) {
    detail::permission_cache_store()[std::string(key)] = decision;
}

} // namespace cc::hooks::tool_permission
