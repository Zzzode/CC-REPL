module;
#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>

export module cc.hooks.teleport_resume;

export namespace cc::hooks {


// Honest stub for src/utils/teleport.tsx. The full TS module (git bundling,
// fetchSession via teleport/api, OAuth-header fetch, queryHaiku title gen,
// environment selection, createAndUploadGitBundle, deserializeMessages) is a
// large multi-module flow and is NOT migrated here. What IS mirrored:
//   - empty target/source sessions are rejected with explicit errors
//   - initiate_teleport is gated behind can_teleport_resume()
//
// KNOWN PARITY GAP: TS gates the real flow on isPolicyAllowed +
// checkGate_CACHED_OR_BLOCKING growthbook, NOT on a CC_TELEPORT_ENABLED env
// var. The env gate below is an honest placeholder so the function is callable
// in tests without wiring the policy/growthbook stack; replace it with the TS
// policy gate when those subsystems are migrated.
inline bool can_teleport_resume() {

    const char* teleport_enabled = std::getenv("CC_TELEPORT_ENABLED");
    return teleport_enabled != nullptr && std::string_view(teleport_enabled) == "1";
}


inline std::expected<void, std::string> initiate_teleport(std::string_view target_session) {
    if (!can_teleport_resume()) {
        return std::unexpected("Teleport is not available in current environment");
    }
    if (target_session.empty()) {
        return std::unexpected("Target session ID cannot be empty");
    }

    return {};
}


inline std::expected<void, std::string> handle_teleport_arrival(std::string_view source_session) {
    if (source_session.empty()) {
        return std::unexpected("Source session ID cannot be empty");
    }

    return {};
}

} // namespace cc::hooks
