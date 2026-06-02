module;
#include <expected>
#include <string>
#include <string_view>
export module cc.services.compact.session_memory_compact;

export namespace cc::services::compact {

namespace detail {
    // Simple in-memory token count tracking per session
    inline int session_token_count = 0;
    constexpr int default_max_tokens = 100000;
    constexpr float compact_threshold = 0.8f;
} // namespace detail

// Compact session memory to fit within target token count
auto compact_session_memory(std::string_view session_id, int target_tokens)
    -> std::expected<int, std::string> {
    if (target_tokens <= 0) {
        return std::unexpected("Target tokens must be positive");
    }
    (void)session_id;
    // Deterministically clamp tracked memory to the requested target.
    int new_count = target_tokens;
    detail::session_token_count = new_count;
    return new_count;
}

// Check if session memory should be compacted
auto should_compact_memory(std::string_view session_id) -> bool {
    (void)session_id;
    return detail::session_token_count >
           static_cast<int>(detail::default_max_tokens * detail::compact_threshold);
}

// Get current memory token count for session
auto get_memory_token_count(std::string_view session_id) -> int {
    (void)session_id;
    return detail::session_token_count;
}

} // namespace cc::services::compact
