module;
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <sstream>

export module cc.server.server_routes;

export namespace cc::server {

// A route handler definition
struct Route {
    std::string method;
    std::string path;
    std::function<std::string(std::map<std::string, std::string>)> handler;
};

namespace detail {
    inline std::vector<Route> routes;
}

// Register a route handler
inline auto register_route(Route route) -> void {
    detail::routes.push_back(std::move(route));
}

// Get all default API routes
inline auto get_default_routes() -> std::vector<Route> {
    std::vector<Route> defaults;

    // GET /health - Health check endpoint
    defaults.push_back({
        "GET",
        "/health",
        [](std::map<std::string, std::string>) -> std::string {
            return R"({"status":"ok","version":"1.0.0"})";
        }
    });

    // POST /message - Send a message to the assistant
    defaults.push_back({
        "POST",
        "/message",
        [](std::map<std::string, std::string> params) -> std::string {
            auto it = params.find("content");
            if (it == params.end() || it->second.empty()) {
                return R"({"error":"content is required"})";
            }

            // In a real implementation, this would forward to the query engine
            std::ostringstream response;
            response << R"({"id":"msg_)" << std::hash<std::string>{}(it->second)
                     << R"(","status":"queued"})";
            return response.str();
        }
    });

    // GET /sessions - List active and recent sessions
    defaults.push_back({
        "GET",
        "/sessions",
        [](std::map<std::string, std::string>) -> std::string {
            return R"({"sessions":[],"total":0})";
        }
    });

    // POST /compact - Compact the current conversation
    defaults.push_back({
        "POST",
        "/compact",
        [](std::map<std::string, std::string>) -> std::string {
            return R"({"status":"compacted","messages_before":0,"messages_after":0})";
        }
    });

    return defaults;
}

// Initialize the route table with default routes
inline auto initialize_routes() -> void {
    auto defaults = get_default_routes();
    for (auto& route : defaults) {
        register_route(std::move(route));
    }
}

// Find a route matching the given method and path
inline auto find_route(std::string_view method, std::string_view path)
    -> const Route* {
    for (const auto& route : detail::routes) {
        if (route.method == method && route.path == path) {
            return &route;
        }
    }
    return nullptr;
}

} // namespace cc::server
