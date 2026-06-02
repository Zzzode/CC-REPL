module;
#include <cstdlib>
#include <string>
#include <string_view>

export module cc.utils.fast_mode;

export namespace cc::utils {

namespace detail {
    inline bool& fast_mode_flag() {
        static bool enabled = false;
        return enabled;
    }
} // namespace detail

// Check if fast mode is enabled
bool is_fast_mode() {
    // Check runtime flag
    if (detail::fast_mode_flag()) return true;

    // Check environment
    const char* env = std::getenv("CLAUDE_FAST_MODE");
    return env && (std::string_view(env) == "1" || std::string_view(env) == "true");
}

// Toggle fast mode
void set_fast_mode(bool enabled) {
    detail::fast_mode_flag() = enabled;
}

// Get the model used in fast mode (lighter, faster model)
std::string get_fast_mode_model() {
    return "claude-haiku-4-20250514";
}

// Heuristic: determine if a query should use fast mode
bool should_use_fast_mode(std::string_view query) {
    // Short, simple queries can use fast mode
    if (query.size() < 50) return true;

    // Simple questions/commands
    if (query.starts_with("what is ") || query.starts_with("how to ") ||
        query.starts_with("list ") || query.starts_with("show ")) {
        return true;
    }

    // Commands that are just file operations
    if (query.find("read") != std::string_view::npos && query.size() < 100) return true;

    // Complex tasks should not use fast mode
    if (query.find("refactor") != std::string_view::npos) return false;
    if (query.find("implement") != std::string_view::npos) return false;
    if (query.find("debug") != std::string_view::npos) return false;
    if (query.find("analyze") != std::string_view::npos) return false;

    return false;
}

} // namespace cc::utils
