module;
#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>

export module cc.hooks.teleport_resume;

export namespace cc::hooks {


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
