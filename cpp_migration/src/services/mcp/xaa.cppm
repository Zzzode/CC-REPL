module;
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
export module cc.services.mcp.xaa;

export namespace cc::services::mcp {

// External Authentication Authority configuration
struct XaaConfig {
    std::string idp_url;
    std::string client_id;
    std::optional<std::string> scope;
};

// Get XAA configuration for a specific MCP server
auto get_xaa_config(std::string_view server) -> std::optional<XaaConfig> {
    (void)server;
    // Read XAA configuration from ~/.cc-repl/xaa-idp.txt if present
    const char* home = std::getenv("HOME");
    if (!home) return std::nullopt;
    auto path = std::filesystem::path(home) / ".cc-repl" / "xaa-idp.txt";
    std::ifstream file(path);
    if (!file) return std::nullopt;
    XaaConfig config;
    std::string line;
    while (std::getline(file, line)) {
        if (line.starts_with("idp_url=")) {
            config.idp_url = line.substr(8);
        } else if (line.starts_with("client_id=")) {
            config.client_id = line.substr(10);
        } else if (line.starts_with("scope=")) {
            config.scope = line.substr(6);
        }
    }
    if (config.idp_url.empty() || config.client_id.empty()) return std::nullopt;
    return config;
}

// Authenticate using XAA configuration, returns access token
auto authenticate_xaa(const XaaConfig& config) -> std::expected<std::string, std::string> {
    if (config.idp_url.empty()) {
        return std::unexpected("IDP URL is required for XAA authentication");
    }
    if (config.client_id.empty()) {
        return std::unexpected("Client ID is required for XAA authentication");
    }
    return std::unexpected("XAA authentication requires an external IDP flow");
}

} // namespace cc::services::mcp
