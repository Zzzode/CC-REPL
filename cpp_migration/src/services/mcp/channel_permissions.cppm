module;
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
export module cc.services.mcp.channel_permissions;

export namespace cc::services::mcp {

// Permission types for MCP channels
enum class ChannelPermission { Read, Write, Execute, Admin };

namespace detail {
    inline std::mutex perm_mutex;
    // channel -> set of permissions
    inline std::map<std::string, std::set<ChannelPermission>, std::less<>> permissions;
} // namespace detail

// Check if a channel has a specific permission
auto check_channel_permission(std::string_view channel, ChannelPermission perm) -> bool {
    std::lock_guard lock(detail::perm_mutex);
    auto it = detail::permissions.find(channel);
    if (it == detail::permissions.end()) {
        return false;
    }
    return it->second.contains(perm);
}

// Grant a permission to a channel
auto grant_permission(std::string_view channel, ChannelPermission perm) -> void {
    std::lock_guard lock(detail::perm_mutex);
    detail::permissions[std::string(channel)].insert(perm);
}

// Revoke a permission from a channel
auto revoke_permission(std::string_view channel, ChannelPermission perm) -> void {
    std::lock_guard lock(detail::perm_mutex);
    auto it = detail::permissions.find(channel);
    if (it != detail::permissions.end()) {
        it->second.erase(perm);
    }
}

} // namespace cc::services::mcp
