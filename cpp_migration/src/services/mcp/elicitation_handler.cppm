module;
#include <expected>
#include <map>
#include <string>
#include <string_view>
export module cc.services.mcp.elicitation_handler;

export namespace cc::services::mcp {

// Elicitation request from an MCP server
struct ElicitationRequest {
    std::string server_name;
    std::string message;
    std::map<std::string, std::string> schema;
};

namespace detail {
    // Servers that are allowed to elicit user input
    inline std::map<std::string, bool, std::less<>> allowed_servers;
} // namespace detail

// Check if a server is allowed to elicit user input
auto is_elicitation_allowed(std::string_view server) -> bool {
    auto it = detail::allowed_servers.find(server);
    if (it != detail::allowed_servers.end()) {
        return it->second;
    }
    // Default: allow elicitation from known servers
    return true;
}

// Handle an elicitation request from an MCP server
auto handle_elicitation(ElicitationRequest request)
    -> std::expected<std::map<std::string, std::string>, std::string> {
    if (!is_elicitation_allowed(request.server_name)) {
        return std::unexpected("Elicitation not allowed for server: " + request.server_name);
    }
    (void)request;
    // Non-interactive fallback returns an empty response map.
    return std::map<std::string, std::string>{};
}

} // namespace cc::services::mcp
