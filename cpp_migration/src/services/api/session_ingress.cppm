module;
#include <expected>
#include <string>
#include <string_view>
export module cc.services.api.session_ingress;

export namespace cc::services::api {

// Configuration for session ingress connection
struct IngressConfig {
    std::string endpoint;
    std::string session_id;
    std::string auth_token;
};

namespace detail {
    inline bool ingress_active = false;
    inline IngressConfig active_config;
} // namespace detail

// Create a new ingress connection for session streaming
auto create_ingress(IngressConfig config) -> std::expected<void, std::string> {
    if (config.endpoint.empty()) {
        return std::unexpected("Ingress endpoint is required");
    }
    if (config.session_id.empty()) {
        return std::unexpected("Session ID is required");
    }
    // Track an in-process ingress connection for deterministic migration behavior.
    detail::active_config = std::move(config);
    detail::ingress_active = true;
    return {};
}

// Send a message through the ingress channel
auto send_ingress_message(std::string_view message) -> std::expected<void, std::string> {
    if (!detail::ingress_active) {
        return std::unexpected("No active ingress connection");
    }
    if (message.empty()) {
        return std::unexpected("Cannot send empty message");
    }
    // Messages are accepted locally while no streaming transport is configured.
    return {};
}

// Close the active ingress connection
auto close_ingress() -> void {
    detail::ingress_active = false;
    detail::active_config = IngressConfig{};
}

} // namespace cc::services::api
