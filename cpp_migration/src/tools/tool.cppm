/// @file tool.cppm
/// @brief Tool system module for the Claude Code CLI engine.
/// Defines the Tool concept, registry, permission model, and execution types.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>
#include <expected>
#include <concepts>
#include <format>
#include <ranges>
#include <span>

export module cc.tools.tool;

export import cc.types.types;
import cc.utils.json;

export namespace cc::core {

// ============================================================
// Tool Permission Model
// ============================================================

/// Permission level required to execute a tool
enum class ToolPermission : std::uint8_t {
    ReadOnly,   // Only reads data (file read, glob, grep)
    Write,      // Modifies files on disk
    Execute,    // Runs arbitrary commands (bash)
    Network,    // Makes network requests
};

/// Convert permission enum to human-readable string
[[nodiscard]] constexpr std::string_view permission_to_string(ToolPermission perm) noexcept {
    switch (perm) {
        case ToolPermission::ReadOnly: return "read_only";
        case ToolPermission::Write:    return "write";
        case ToolPermission::Execute:  return "execute";
        case ToolPermission::Network:  return "network";
    }
    return "unknown";
}

// ============================================================
// Tool Input / Output types
// ============================================================

/// JSON-based tool input wrapping raw parameter data
struct ToolInput {
    std::string raw_json;   // Raw JSON string of parameters

    /// Check if a top-level key exists in the JSON input object
    [[nodiscard]] bool has_field(std::string_view key) const noexcept {
        if (key.empty()) return false;
        auto parsed = cc::utils::json::parse(raw_json);
        if (!parsed) return false;
        return parsed->root().has(key);
    }

    /// Get the raw JSON as string view
    [[nodiscard]] std::string_view json() const noexcept { return raw_json; }

    /// Create from raw JSON string
    [[nodiscard]] static ToolInput from_json(std::string json) {
        return ToolInput{std::move(json)};
    }
};

/// Content returned by tool execution, modeled after API content blocks
struct ToolOutputContent {
    std::string text;                   // Primary output text
    std::optional<std::string> format;  // "text", "json", "markdown"
    std::optional<std::string> media_type;
    std::optional<std::string> data;

    /// Create a plain text output
    [[nodiscard]] static ToolOutputContent text_output(std::string text) {
        return ToolOutputContent{
            .text = std::move(text),
            .format = "text",
            .media_type = std::nullopt,
            .data = std::nullopt,
        };
    }

    /// Create a JSON-formatted output
    [[nodiscard]] static ToolOutputContent json_output(std::string json) {
        return ToolOutputContent{
            .text = std::move(json),
            .format = "json",
            .media_type = std::nullopt,
            .data = std::nullopt,
        };
    }

    /// Create a base64 image output
    [[nodiscard]] static ToolOutputContent image_output(std::string media_type, std::string data) {
        return ToolOutputContent{
            .text = {},
            .format = "image",
            .media_type = std::move(media_type),
            .data = std::move(data),
        };
    }

    /// Create a base64 document output
    [[nodiscard]] static ToolOutputContent document_output(std::string media_type, std::string data) {
        return ToolOutputContent{
            .text = {},
            .format = "document",
            .media_type = std::move(media_type),
            .data = std::move(data),
        };
    }
};

/// Result of a tool execution
struct ToolResult {
    std::vector<ToolOutputContent> content;  // Output content blocks
    bool is_error = false;                   // Whether execution failed

    /// Create a successful single-text result
    [[nodiscard]] static ToolResult success(std::string text) {
        return ToolResult{
            {ToolOutputContent::text_output(std::move(text))},
            false
        };
    }

    /// Create an error result
    [[nodiscard]] static ToolResult error(std::string message) {
        return ToolResult{
            {ToolOutputContent::text_output(std::move(message))},
            true
        };
    }

    /// Create a multi-content successful result
    [[nodiscard]] static ToolResult success_multi(std::vector<ToolOutputContent> content) {
        return ToolResult{std::move(content), false};
    }
};

// ============================================================
// JSON Schema property for tool input definition
// ============================================================

/// Describes a single property in the tool's JSON input schema
struct SchemaProperty {
    std::string name;
    std::string type;                        // "string", "number", "boolean", "array", "object"
    std::string description;
    bool required = false;
    std::optional<std::string> default_value = std::nullopt;
    std::optional<std::vector<std::string>> enum_values = std::nullopt;  // For enum-typed properties
};

/// Full JSON schema describing a tool's expected input
struct InputSchema {
    std::vector<SchemaProperty> properties;

    /// Get all required property names
    [[nodiscard]] auto required_properties() const {
        return properties
            | std::views::filter([](const SchemaProperty& p) { return p.required; })
            | std::views::transform([](const SchemaProperty& p) -> const std::string& { return p.name; });
    }

    /// Serialize schema to JSON string (simplified representation)
    [[nodiscard]] std::string to_json() const {
        std::string result = R"({"type":"object","properties":{)";
        bool first = true;
        for (const auto& prop : properties) {
            if (!first) result += ",";
            first = false;
            result += std::format(R"("{}":{{  "type":"{}","description":"{}"}})",
                                  prop.name, prop.type, prop.description);
        }
        result += R"(},"required":[)";
        first = true;
        for (const auto& prop : properties) {
            if (!prop.required) continue;
            if (!first) result += ",";
            first = false;
            result += std::format(R"("{}")", prop.name);
        }
        result += "]}";
        return result;
    }
};

// ============================================================
// Tool Definition
// ============================================================

/// Static metadata describing a tool available to the model
struct ToolDefinition {
    std::string name;                    // Unique tool identifier (e.g., "Read", "Write")
    std::string description;             // Human-readable description shown to model
    InputSchema input_schema;            // JSON schema for tool parameters
    ToolPermission permission;           // Required permission level
    bool is_hidden = false;              // Hidden tools are not shown in listings
    std::optional<std::string> category = std::nullopt; // Grouping category (e.g., "filesystem", "search")
    std::size_t max_result_size_chars = 100'000; // TS maxResultSizeChars default for result persistence
    bool max_result_size_unbounded = false;      // TS maxResultSizeChars: Infinity
};

// ============================================================
// Tool concept - the contract every tool must satisfy
// ============================================================

/// Concept defining the interface all tools must implement
template <typename T>
concept Tool = requires(T tool, const ToolInput& input) {
    // Must provide its static definition
    { T::definition() } -> std::same_as<ToolDefinition>;

    // Must be executable with ToolInput, returning a Result<ToolResult>
    { tool.execute(input) } -> std::same_as<Result<ToolResult>>;

    // Must support permission checking
    { tool.check_permission(input) } -> std::same_as<bool>;
};

/// Concept for tools that support async/coroutine execution
template <typename T>
concept AsyncTool = Tool<T> && requires(T tool, const ToolInput& input) {
    // Async tools provide a coroutine-based execution path
    { tool.execute_async(input) } -> std::same_as<Result<ToolResult>>;
};

// ============================================================
// Type-erased tool wrapper for runtime polymorphism
// ============================================================

/// Type-erased interface for storing heterogeneous tools in registry
class ITool {
public:
    virtual ~ITool() = default;

    [[nodiscard]] virtual const ToolDefinition& definition() const = 0;
    [[nodiscard]] virtual Result<ToolResult> execute(const ToolInput& input) = 0;
    [[nodiscard]] virtual bool check_permission(const ToolInput& input) const = 0;
};

/// Concrete wrapper adapting any Tool concept implementor to ITool interface
template <Tool T>
class ToolWrapper final : public ITool {
    T tool_;
    ToolDefinition def_;

public:
    explicit ToolWrapper(T tool)
        : tool_(std::move(tool)), def_(T::definition()) {}

    [[nodiscard]] const ToolDefinition& definition() const override { return def_; }

    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) override {
        return tool_.execute(input);
    }

    [[nodiscard]] bool check_permission(const ToolInput& input) const override {
        return tool_.check_permission(input);
    }
};

// ============================================================
// Tool Registry - manages all available tools
// ============================================================

/// Central registry holding all registered tools, keyed by name
class ToolRegistry {
    std::unordered_map<std::string, std::unique_ptr<ITool>> tools_;

public:
    ToolRegistry() = default;

    // Non-copyable, movable
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;
    ToolRegistry(ToolRegistry&&) = default;
    ToolRegistry& operator=(ToolRegistry&&) = default;

    /// Register a tool by constructing it in-place
    template <Tool T, typename... Args>
    void register_tool(Args&&... args) {
        auto wrapper = std::make_unique<ToolWrapper<T>>(T{std::forward<Args>(args)...});
        auto name = wrapper->definition().name;
        tools_.emplace(std::move(name), std::move(wrapper));
    }

    /// Register a pre-constructed tool (bypasses concept check across module boundaries)
    void register_tool(std::unique_ptr<ITool> tool) {
        if (!tool) return;
        auto name = std::string(tool->definition().name);
        tools_.emplace(std::move(name), std::move(tool));
    }

    /// Retrieve a tool by name
    [[nodiscard]] ITool* get(std::string_view name) const {
        auto it = tools_.find(std::string(name));
        if (it == tools_.end()) return nullptr;
        return it->second.get();
    }

    /// Execute a tool by name with given input
    [[nodiscard]] Result<ToolResult> execute(std::string_view name, const ToolInput& input) {
        auto* tool = get(name);
        if (!tool) {
            // TS PARITY FALLBACK: if the tool is not registered (e.g. an
            // MCP server tool like "analyze_image"), try the missing-tool
            // handler which can route it to a connected MCP server.
            if (missing_tool_handler_) {
                return missing_tool_handler_(name, input);
            }
            return std::unexpected(Error::make(
                ErrorCode::ToolNotFound,
                std::format("Tool '{}' not found in registry", name)
            ));
        }

        // Check permission before execution
        if (!tool->check_permission(input)) {
            return std::unexpected(Error::make(
                ErrorCode::ToolPermissionDenied,
                std::format("Permission denied for tool '{}'", name)
            ));
        }

        return tool->execute(input);
    }

    /// Get definitions of all registered (non-hidden) tools for the API
    [[nodiscard]] std::vector<ToolDefinition> get_visible_definitions() const {
        std::vector<ToolDefinition> defs;
        for (const auto& [_, tool] : tools_) {
            if (!tool->definition().is_hidden) {
                defs.push_back(tool->definition());
            }
        }
        return defs;
    }

    /// Get all tool names
    [[nodiscard]] std::vector<std::string_view> tool_names() const {
        std::vector<std::string_view> names;
        names.reserve(tools_.size());
        for (const auto& [name, _] : tools_) {
            names.emplace_back(name);
        }
        return names;
    }

    /// Check if a tool is registered
    [[nodiscard]] bool contains(std::string_view name) const {
        return tools_.contains(std::string(name));
    }

    /// Number of registered tools
    [[nodiscard]] std::size_t size() const noexcept { return tools_.size(); }

    /// Callback for handling unregistered tool lookups.
    /// When `execute()` is called for a tool not in the registry,
    /// this handler is invoked as a fallback.  Returns a valid
    /// `Result<ToolResult>` if the fallback could execute the tool,
    /// or `std::unexpected` with a `ToolNotFound` error otherwise.
    ///
    /// TS PARITY: In TS, `assembleToolPool` merges built-in tools with
    /// `mcp.tools` (dynamically registered per-server tools).  When the
    /// model calls an MCP tool by its short name (e.g. "analyze_image"),
    /// `findToolByName` locates it in the merged pool.  In CPP, MCP
    /// server tools are NOT individually registered — only the generic
    /// "mcp" wrapper is.  This fallback bridges the gap: if a tool is
    /// not found, we try to route it to a connected MCP server that
    /// exposes a tool with the same name.
    using MissingToolHandler = std::function<Result<ToolResult>(
        std::string_view tool_name, const ToolInput& input)>;

    void set_missing_tool_handler(MissingToolHandler handler) {
        missing_tool_handler_ = std::move(handler);
    }

private:
    MissingToolHandler missing_tool_handler_;
};

} // namespace cc::core
