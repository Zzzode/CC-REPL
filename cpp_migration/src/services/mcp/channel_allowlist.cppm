module;
#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.mcp.channel_allowlist;

export namespace cc::services::mcp {

namespace detail {
    inline std::mutex allowlist_mutex;
    inline std::vector<std::string> allowlist;
} // namespace detail

// Check if a channel is in the allowlist
auto is_channel_allowed(std::string_view channel) -> bool {
    std::lock_guard lock(detail::allowlist_mutex);
    return std::ranges::find(detail::allowlist, channel) != detail::allowlist.end();
}

// Add a channel to the allowlist
auto add_to_allowlist(std::string_view channel) -> void {
    std::lock_guard lock(detail::allowlist_mutex);
    if (std::ranges::find(detail::allowlist, channel) == detail::allowlist.end()) {
        detail::allowlist.emplace_back(channel);
    }
}

// Remove a channel from the allowlist
auto remove_from_allowlist(std::string_view channel) -> void {
    std::lock_guard lock(detail::allowlist_mutex);
    std::erase_if(detail::allowlist, [&](const auto& c) { return c == channel; });
}

// Get all channels in the allowlist
auto get_allowlist() -> std::vector<std::string> {
    std::lock_guard lock(detail::allowlist_mutex);
    return detail::allowlist;
}

} // namespace cc::services::mcp
