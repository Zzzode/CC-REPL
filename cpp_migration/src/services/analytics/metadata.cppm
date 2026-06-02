module;
#include <map>
#include <string>
export module cc.services.analytics.metadata;

export namespace cc::services::analytics {

// Event metadata for analytics tracking
struct EventMetadata {
    std::string session_id;
    std::string user_id;
    std::string model;
    std::string version;
    std::map<std::string, std::string> extra;
};

namespace detail {
    inline EventMetadata current_metadata;
} // namespace detail

// Get current event metadata context
auto get_event_metadata() -> EventMetadata {
    return detail::current_metadata;
}

// Enrich an event map with standard metadata fields
auto enrich_event(std::map<std::string, std::string>& event) -> void {
    const auto& meta = detail::current_metadata;
    if (!meta.session_id.empty()) {
        event["session_id"] = meta.session_id;
    }
    if (!meta.user_id.empty()) {
        event["user_id"] = meta.user_id;
    }
    if (!meta.model.empty()) {
        event["model"] = meta.model;
    }
    if (!meta.version.empty()) {
        event["version"] = meta.version;
    }
    // Merge extra metadata
    for (const auto& [k, v] : meta.extra) {
        event.try_emplace(k, v);
    }
}

} // namespace cc::services::analytics
