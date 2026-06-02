module;
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
export module cc.services.mcp.normalization;

export namespace cc::services::mcp {

// Normalize a server name to canonical form (lowercase, trimmed)
auto normalize_server_name(std::string_view name) -> std::string {
    std::string result(name);
    // Trim whitespace
    auto start = result.find_first_not_of(" \t\n\r");
    auto end = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    result = result.substr(start, end - start + 1);
    // Lowercase
    std::ranges::transform(result, result.begin(), ::tolower);
    return result;
}

// Normalize a tool name to qualified format (server:tool)
auto normalize_tool_name(std::string_view server, std::string_view tool) -> std::string {
    auto normalized_server = normalize_server_name(server);
    std::string normalized_tool(tool);
    std::ranges::transform(normalized_tool, normalized_tool.begin(), ::tolower);
    return normalized_server + ":" + normalized_tool;
}

// Parse a qualified tool name into (server, tool) pair
auto parse_qualified_tool_name(std::string_view qualified)
    -> std::pair<std::string, std::string> {
    auto sep = qualified.find(':');
    if (sep == std::string_view::npos) {
        return {"", std::string(qualified)};
    }
    return {
        std::string(qualified.substr(0, sep)),
        std::string(qualified.substr(sep + 1))
    };
}

} // namespace cc::services::mcp
