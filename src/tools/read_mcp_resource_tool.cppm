module;
#include <string>
#include <string_view>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>

export module cc.tools.read_mcp_resource_tool;

export namespace cc::tools {


struct ReadMcpResourceInput {
    std::string server_name;
    std::string uri;
};


inline auto execute_read_mcp_resource(
    const ReadMcpResourceInput& input
) -> std::expected<std::string, std::string> {

    if (input.server_name.empty()) {
        return std::unexpected(std::string("server_name is required"));
    }
    if (input.uri.empty()) {
        return std::unexpected(std::string("uri is required"));
    }


    if (input.uri.find("://") == std::string::npos &&
        !input.uri.starts_with("/")) {
        return std::unexpected(std::string("Invalid URI format: ") + input.uri);
    }

    if (input.server_name == "filesystem" || input.server_name == "local" || input.uri.starts_with("file://")) {
        std::filesystem::path path = input.uri.starts_with("file://")
            ? std::filesystem::path(input.uri.substr(7))
            : std::filesystem::path(input.uri);
        if (!std::filesystem::exists(path)) return std::unexpected("Resource does not exist: " + path.string());
        if (std::filesystem::is_directory(path)) return std::unexpected("Resource is a directory: " + path.string());
        std::ifstream file(path);
        if (!file) return std::unexpected("Failed to open resource: " + path.string());
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    return std::string(R"({"uri":")") + input.uri + R"(","server":")" + input.server_name + R"("})";
}


inline auto get_read_mcp_resource_prompt() -> std::string {
    return R"(## ReadMcpResourceTool

Read content from an MCP (Model Context Protocol) server resource.

### Parameters:
- `server_name` (required): Name of the MCP server to connect to
- `uri` (required): URI of the resource to read

### Usage:
- Read files, documents, or data exposed by MCP servers
- Access resources that require server-side processing
- Retrieve structured data from tool servers

### Example:
```json
{
  "server_name": "filesystem",
  "uri": "file:///path/to/document.md"
}
```)";
}

} // namespace cc::tools
