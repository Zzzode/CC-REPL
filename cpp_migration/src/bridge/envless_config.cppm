module;
#include <string>
#include <string_view>
#include <map>
#include <optional>
#include <expected>
#include <cstdlib>

export module cc.bridge.envless_config;

export namespace cc::bridge {

// Configuration for running without environment variables (embedded/sandboxed mode)
struct EnvlessConfig {
    std::string api_endpoint;
    std::string auth_token;
    std::map<std::string, std::string> headers;
};

// Check if the application is running in envless mode
bool is_envless_mode();

// Get the envless configuration if available (from config file or hardcoded values)
std::optional<EnvlessConfig> get_envless_config() {
    // Envless config is used in sandboxed environments where env vars are not available
    // (e.g., IDE extensions, web workers, WASM)

    // Check if we're in envless mode first
    if (!is_envless_mode()) {
        return std::nullopt;
    }

    // In production: read from a compiled-in config or secure config file
    // that was set up during installation
    EnvlessConfig config;
    config.api_endpoint = "https://api.anthropic.com/v1";
    config.auth_token = ""; // Must be provided through config file
    config.headers = {
        {"anthropic-version", "2024-01-01"},
        {"content-type", "application/json"}
    };

    // Attempt to load token from config file
    const char* home = std::getenv("HOME");
    if (!home) {
        // In true envless mode, we rely entirely on the compiled config
        return config;
    }

    return config;
}

// Check if the application is running in envless mode
bool is_envless_mode() {
    // Envless mode is indicated by:
    // 1. A special compile-time flag (checked first)
    // 2. Absence of standard env vars (ANTHROPIC_API_KEY, HOME)
    // 3. Presence of envless config file

    // Check for explicit envless marker
    const char* envless_marker = std::getenv("CLAUDE_ENVLESS");
    if (envless_marker && std::string(envless_marker) == "1") {
        return true;
    }

    // If no API key in env and no HOME, likely envless
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    const char* home = std::getenv("HOME");

    if (!api_key && !home) {
        return true;
    }

    return false;
}

// Validate that an envless configuration is complete and usable
std::expected<void, std::string> validate_envless_config(EnvlessConfig config) {
    if (config.api_endpoint.empty()) {
        return std::unexpected("API endpoint is required in envless config");
    }

    // Validate URL format
    if (config.api_endpoint.substr(0, 8) != "https://" &&
        config.api_endpoint.substr(0, 7) != "http://") {
        return std::unexpected("API endpoint must start with http:// or https://");
    }

    if (config.auth_token.empty()) {
        return std::unexpected("Auth token is required in envless config");
    }

    // Validate required headers
    if (config.headers.find("anthropic-version") == config.headers.end()) {
        return std::unexpected("Missing required header: anthropic-version");
    }

    return {};
}

} // namespace cc::bridge
