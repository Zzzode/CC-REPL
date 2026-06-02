module;
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
export module cc.services.mcp.mcp_utils;

export namespace cc::services::mcp {

// Validate an MCP URI format (mcp://server/resource)
auto validate_mcp_uri(std::string_view uri) -> bool {
    if (!uri.starts_with("mcp://")) {
        return false;
    }
    // Must have at least server portion after mcp://
    auto rest = uri.substr(6);
    return !rest.empty() && rest.find('/') != std::string_view::npos;
}

// Format a user-friendly MCP error message
auto format_mcp_error(std::string_view server, std::string_view error) -> std::string {
    return std::string("MCP error from '") + std::string(server) + "': " + std::string(error);
}

// Get timeout for a specific MCP server (from config or default)
auto get_mcp_timeout(std::string_view server) -> std::chrono::seconds {
    (void)server;
    // Default timeout when no per-server override is configured.
    return std::chrono::seconds{60};
}

// Check if MCP is enabled globally
auto is_mcp_enabled() -> bool {
    // Check if MCP is explicitly disabled
    if (auto* val = std::getenv("CLAUDE_MCP_DISABLED")) {
        return std::string_view(val) != "1" && std::string_view(val) != "true";
    }
    return true;
}

} // namespace cc::services::mcp
