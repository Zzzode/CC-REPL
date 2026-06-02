module;
#include <optional>
#include <string>
export module cc.services.compact.warning_hook;

export namespace cc::services::compact {

namespace detail {
    inline float warning_threshold = 0.8f; // Warn at 80% capacity
} // namespace detail

// Check if compact is needed and return warning message if so
auto check_compact_needed(int current_tokens, int max_tokens) -> std::optional<std::string> {
    if (max_tokens <= 0) return std::nullopt;

    float usage_ratio = static_cast<float>(current_tokens) / static_cast<float>(max_tokens);
    if (usage_ratio >= detail::warning_threshold) {
        int pct = static_cast<int>(usage_ratio * 100);
        return "Context window is " + std::to_string(pct) + "% full (" +
               std::to_string(current_tokens) + "/" + std::to_string(max_tokens) +
               " tokens). Consider running /compact.";
    }
    return std::nullopt;
}

// Get the current compact warning threshold
auto get_compact_warning_threshold() -> float {
    return detail::warning_threshold;
}

// Set the compact warning threshold (0.0 - 1.0)
auto set_compact_warning_threshold(float threshold) -> void {
    if (threshold > 0.0f && threshold <= 1.0f) {
        detail::warning_threshold = threshold;
    }
}

} // namespace cc::services::compact
