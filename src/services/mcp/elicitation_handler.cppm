module;
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
export module cc.services.mcp.elicitation_handler;

export namespace cc::services::mcp {

// Elicitation request from an MCP server
struct ElicitationRequest {
    std::string server_name;
    std::string message;
    std::map<std::string, std::string> schema;
};

using ElicitationResponder =
    std::function<std::expected<std::map<std::string, std::string>, std::string>(const ElicitationRequest&)>;

namespace detail {
    // Servers that are allowed to elicit user input
    inline std::map<std::string, bool, std::less<>> allowed_servers;
    inline std::mutex responder_mutex;
    inline ElicitationResponder responder;
} // namespace detail

auto set_elicitation_allowed(std::string server, bool allowed) -> void {
    detail::allowed_servers[std::move(server)] = allowed;
}

auto clear_elicitation_policy() -> void {
    detail::allowed_servers.clear();
}

auto set_elicitation_responder(ElicitationResponder responder) -> void {
    std::lock_guard lock(detail::responder_mutex);
    detail::responder = std::move(responder);
}

auto clear_elicitation_responder() -> void {
    std::lock_guard lock(detail::responder_mutex);
    detail::responder = nullptr;
}

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

    ElicitationResponder active_responder;
    {
        std::lock_guard lock(detail::responder_mutex);
        active_responder = detail::responder;
    }
    if (!active_responder) {
        return std::unexpected(
            "No MCP elicitation responder is registered for server: " + request.server_name);
    }
    return active_responder(request);
}

} // namespace cc::services::mcp
