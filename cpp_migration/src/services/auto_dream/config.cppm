module;
#include <chrono>
#include <string>
export module cc.services.auto_dream.config;

export namespace cc::services::auto_dream {

// AutoDream consolidation configuration
struct DreamConfig {
    bool enabled{false};
    std::chrono::hours min_interval{24};
    int max_consolidations_per_day{3};
};

namespace detail {
    inline DreamConfig current_config;
} // namespace detail

// Get the current dream configuration
auto get_dream_config() -> DreamConfig {
    return detail::current_config;
}

// Update dream configuration
auto set_dream_config(DreamConfig config) -> void {
    detail::current_config = std::move(config);
}

// Check if conditions are met for dream consolidation
auto is_dream_eligible() -> bool {
    if (!detail::current_config.enabled) {
        return false;
    }
    // Eligibility currently depends only on the enabled flag; persisted cadence
    // state is not available in this migration module.
    return true;
}

} // namespace cc::services::auto_dream
