/// @file experiment_event.cppm
/// @brief GrowthBook experiment assignment event types.
/// Migrated from: src/types/generated/events_mono/growthbook/v1/growthbook_experiment_event.ts
module;

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

export module cc.types.experiment_event;

import cc.types.auth;

export namespace cc::types::experiment_event {

/// GrowthBook experiment assignment event.
/// Tracks when a user is exposed to an experiment variant.
struct GrowthbookExperimentEvent {
    /// Unique event identifier (for deduplication)
    std::optional<std::string> event_id;
    /// When user was exposed to experiment
    std::optional<std::chrono::system_clock::time_point> timestamp;
    /// Experiment tracking key (maps to GrowthBook's experiment_id column)
    std::optional<std::string> experiment_id;
    /// Variation index: 0=control, 1+=variants
    std::optional<int32_t> variation_id;
    /// Environment where assignment occurred
    std::optional<std::string> environment;
    /// User attributes at time of assignment (JSON string)
    std::optional<std::string> user_attributes;
    /// Experiment metadata (JSON string)
    std::optional<std::string> experiment_metadata;
    /// Device identifier for the client
    std::optional<std::string> device_id;
    /// Authentication context automatically injected by the API
    std::optional<cc::types::auth::PublicApiAuth> auth;
    /// Session identifier for tracking user sessions
    std::optional<std::string> session_id;
    /// Anonymous identifier for unauthenticated users
    std::optional<std::string> anonymous_id;
    /// Event metadata variables (auto-populated by event_logging library)
    std::optional<std::string> event_metadata_vars;
};

/// Create a default-initialized GrowthbookExperimentEvent
[[nodiscard]] inline GrowthbookExperimentEvent create_default() {
    return GrowthbookExperimentEvent{
        .event_id = "",
        .timestamp = std::nullopt,
        .experiment_id = "",
        .variation_id = 0,
        .environment = "",
        .user_attributes = "",
        .experiment_metadata = "",
        .device_id = "",
        .auth = std::nullopt,
        .session_id = "",
        .anonymous_id = "",
        .event_metadata_vars = "",
    };
}

} // namespace cc::types::experiment_event
