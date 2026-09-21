module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>

export module cc.tools.list_mcp_resources_tool;

export namespace cc::tools {


struct McpResource {
    std::string uri;
    std::string name;
    std::optional<std::string> description;
    std::string mime_type;
};


inline auto list_mcp_resources(std::string_view server_name) -> std::vector<McpResource> {
    if (server_name.empty()) {
        return {};
    }

    if (server_name == "filesystem" || server_name == "local") {
        std::vector<McpResource> resources;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path())) {
            resources.push_back(McpResource{
                .uri = "file://" + entry.path().string(),
                .name = entry.path().filename().string(),
                .description = entry.is_directory() ? std::optional<std::string>("directory") : std::optional<std::string>("file"),
                .mime_type = entry.is_directory() ? "inode/directory" : "application/octet-stream"
            });
        }
        return resources;
    }

    return {McpResource{
        .uri = "mcp://" + std::string(server_name) + "/resources",
        .name = std::string(server_name) + " resource index",
        .description = "Resource index advertised by the MCP server name",
        .mime_type = "application/json"
    }};
}


inline auto get_list_mcp_resources_prompt() -> std::string {
    return R"(## ListMcpResourcesTool

List all available resources from an MCP (Model Context Protocol) server.

### Parameters:
- `server_name` (required): Name of the MCP server to query

### Usage:
- Discover what resources an MCP server exposes
- Browse available files, documents, or data sources
- Find resource URIs for subsequent read operations

### Returns:
Array of resources with:
- `uri`: Resource identifier for reading
- `name`: Human-readable resource name
- `description`: Optional description of the resource
- `mime_type`: Content type of the resource

### Example:
```json
{
  "server_name": "filesystem"
}
```)";
}


inline auto format_mcp_resources(const std::vector<McpResource>& resources) -> std::string {
    if (resources.empty()) {
        return "No resources available.";
    }

    std::string result = "Available resources:\n";
    for (const auto& res : resources) {
        result += "  • " + res.name + " [" + res.mime_type + "]\n";
        result += "    URI: " + res.uri + "\n";
        if (res.description.has_value()) {
            result += "    " + *res.description + "\n";
        }
    }
    return result;
}

} // namespace cc::tools
