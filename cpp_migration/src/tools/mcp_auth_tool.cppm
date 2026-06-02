module;
#include <string>
#include <string_view>
#include <expected>
#include <vector>
#include <cstdlib>

export module cc.tools.mcp_auth_tool;

export namespace cc::tools {

// MCP 认证输入参数
struct McpAuthInput {
    std::string server_name;  // MCP 服务器名称
    std::string auth_type;    // 认证类型（oauth, api_key, basic, token）
};

// 执行 MCP 服务器认证
inline auto execute_mcp_auth(
    const McpAuthInput& input
) -> std::expected<std::string, std::string> {
    // 验证输入
    if (input.server_name.empty()) {
        return std::unexpected(std::string("server_name is required"));
    }
    if (input.auth_type.empty()) {
        return std::unexpected(std::string("auth_type is required"));
    }

    // 验证认证类型
    static const std::vector<std::string_view> valid_auth_types = {
        "oauth", "api_key", "basic", "token", "none"
    };

    bool valid = false;
    for (const auto& type : valid_auth_types) {
        if (input.auth_type == type) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        return std::unexpected(std::string(
            "Invalid auth_type: '" + input.auth_type +
            "'. Must be one of: oauth, api_key, basic, token, none"
        ));
    }

    if (input.auth_type == "none") {
        return "Authentication not required for server '" + input.server_name + "'";
    }

    std::string env_name = "MCP_";
    for (char ch : input.server_name) {
        if ((ch >= 'a' && ch <= 'z')) env_name += static_cast<char>(ch - 'a' + 'A');
        else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) env_name += ch;
        else env_name += '_';
    }
    env_name += "_TOKEN";

    if (input.auth_type == "token" || input.auth_type == "api_key") {
        if (std::getenv(env_name.c_str())) {
            return "Authentication credentials found in " + env_name;
        }
        return std::unexpected("Missing credentials environment variable: " + env_name);
    }

    if (input.auth_type == "oauth") {
        return "OAuth authentication requires browser authorization for server '" + input.server_name + "'";
    }

    return std::unexpected("Basic authentication requires interactive credential input");
}

// 获取 MCP 认证工具的提示词
inline auto get_mcp_auth_prompt() -> std::string {
    return R"(## McpAuthTool

Authenticate with an MCP (Model Context Protocol) server.

### Parameters:
- `server_name` (required): Name of the MCP server to authenticate with
- `auth_type` (required): Authentication method - one of:
  - "oauth": OAuth 2.0 flow (opens browser for authorization)
  - "api_key": API key authentication
  - "basic": Basic username/password authentication
  - "token": Bearer token authentication
  - "none": No authentication required

### Usage:
- Authenticate before accessing protected MCP resources
- Refresh expired credentials
- Switch between different authentication contexts

### Security:
- Credentials are stored in the system keychain
- Tokens are never logged or displayed in full
- OAuth tokens are automatically refreshed when possible

### Example:
```json
{
  "server_name": "github-mcp",
  "auth_type": "oauth"
}
```)";
}

} // namespace cc::tools
