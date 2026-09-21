module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module cc.services.mcp.mcp_string_utils;

export namespace cc::services::mcp {

/// Parse a tool name from its fully qualified form (server/tool)
inline std::pair<std::string, std::string> parse_qualified_tool_name(
    std::string_view qualified_name) {
    auto pos = qualified_name.find('/');
    if (pos == std::string_view::npos) {
        return {"", std::string(qualified_name)};
    }
    return {std::string(qualified_name.substr(0, pos)),
            std::string(qualified_name.substr(pos + 1))};
}

/// Create a fully qualified tool name
inline std::string make_qualified_tool_name(
    std::string_view server_name, std::string_view tool_name) {
    return std::string(server_name) + "/" + std::string(tool_name);
}

/// Normalize a server name for comparison
inline std::string normalize_server_name(std::string_view name) {
    std::string result(name);
    for (auto& c : result) {
        if (c == ' ' || c == '-') c = '_';
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    }
    return result;
}

/// Truncate tool output for display
inline std::string truncate_tool_output(std::string_view output, std::size_t max_len = 1000) {
    if (output.size() <= max_len) return std::string(output);
    return std::string(output.substr(0, max_len)) + "... (truncated)";
}

/// Check if string looks like a JSON object
inline bool looks_like_json(std::string_view s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) return false;
    return s[start] == '{' || s[start] == '[';
}

} // namespace cc::services::mcp
