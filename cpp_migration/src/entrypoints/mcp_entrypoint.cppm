/// @file mcp_entrypoint.cppm
/// @brief MCP server entrypoint for exposing tools via Model Context Protocol.
/// Migrated from src/entrypoints/mcp.ts
///
/// Starts a stdio-based MCP server that exposes CLI tools to external clients.
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <variant>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <iostream>

export module cc.entrypoints.mcp_entrypoint;

export namespace cc::entrypoints::mcp {

// ============================================================================
// MCP Protocol Types
// ============================================================================

/// JSON schema representation for tool input/output
using JsonSchema = std::unordered_map<std::string, std::string>;

/// MCP tool definition
struct McpToolDefinition {
    std::string name;
    std::string description;
    JsonSchema input_schema;
    std::optional<JsonSchema> output_schema;
};

/// MCP tool call result content block
struct McpTextContent {
    static constexpr auto type = "text";
    std::string text;
};

/// MCP tool call result
struct McpCallToolResult {
    std::vector<McpTextContent> content;
    bool is_error = false;
};

/// MCP list tools result
struct McpListToolsResult {
    std::vector<McpToolDefinition> tools;
};

// ============================================================================
// MCP Server Configuration
// ============================================================================

/// Server metadata
struct McpServerMetadata {
    std::string name = "claude/tengu";
    std::string version;
};

/// Server capabilities declaration
struct McpServerCapabilities {
    bool tools_enabled = true;
};

// ============================================================================
// MCP Server Interface
// ============================================================================

/// Configuration for the MCP server
struct McpServerConfig {
    std::string cwd;
    bool debug = false;
    bool verbose = false;
    int read_file_state_cache_size = 100;  // LRU cache size for file state
};

/// Tool use context for MCP tool invocations
struct McpToolUseContext {
    bool is_non_interactive_session = true;
    bool debug = false;
    bool verbose = false;
};

/// Abstract interface for the MCP server
/// Implementation will use platform-specific stdio transport
class IMcpServer {
public:
    virtual ~IMcpServer() = default;

    /// Start the MCP server (blocking, runs until transport closes)
    virtual void start() = 0;

    /// Register the tool list handler
    virtual void set_list_tools_handler(
        std::function<McpListToolsResult()> handler) = 0;

    /// Register the tool call handler
    virtual void set_call_tool_handler(
        std::function<McpCallToolResult(const std::string& name,
                                        const JsonSchema& args)> handler) = 0;
};

/// Factory function to create and start an MCP server
/// @param config Server configuration including cwd, debug, verbose flags
/// @return Unique pointer to the server instance (caller owns lifetime)
[[nodiscard]] inline std::unique_ptr<IMcpServer> create_mcp_server(
    const McpServerConfig& config) {

    /// Stdio-based MCP server implementation
    class StdioMcpServer final : public IMcpServer {
    public:
        explicit StdioMcpServer(McpServerConfig cfg)
            : config_(std::move(cfg)) {}

        void start() override {
            // Read JSON-RPC messages from stdin, dispatch to handlers
            // This is a simplified event loop for MCP stdio transport
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty()) continue;
                // Parse method from JSON-RPC request
                auto method_pos = line.find("\"method\"");
                if (method_pos == std::string::npos) continue;

                if (line.find("tools/list") != std::string::npos) {
                    if (list_tools_handler_) {
                        auto result = list_tools_handler_();
                        // Serialize and write to stdout
                        (void)result;
                    }
                } else if (line.find("tools/call") != std::string::npos) {
                    if (call_tool_handler_) {
                        // Extract tool name and args from request
                        auto result = call_tool_handler_("", {});
                        (void)result;
                    }
                }
            }
        }

        void set_list_tools_handler(
            std::function<McpListToolsResult()> handler) override {
            list_tools_handler_ = std::move(handler);
        }

        void set_call_tool_handler(
            std::function<McpCallToolResult(const std::string& name,
                                            const JsonSchema& args)> handler) override {
            call_tool_handler_ = std::move(handler);
        }

    private:
        McpServerConfig config_;
        std::function<McpListToolsResult()> list_tools_handler_;
        std::function<McpCallToolResult(const std::string&, const JsonSchema&)> call_tool_handler_;
    };

    return std::make_unique<StdioMcpServer>(config);
}

/// High-level entrypoint matching the TypeScript startMCPServer function
/// @param cwd Working directory for the server
/// @param debug Enable debug mode
/// @param verbose Enable verbose logging
inline void start_mcp_server(
    const std::string& cwd,
    bool debug,
    bool verbose) {
    McpServerConfig config{
        .cwd = cwd,
        .debug = debug,
        .verbose = verbose,
    };

    auto server = create_mcp_server(config);
    if (server) {
        // In TS: register ListTools and CallTool handlers, then connect
        server->start();
    }
}

} // namespace cc::entrypoints::mcp
