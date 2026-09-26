/// @file tool_types.cppm
/// @brief Tool input/output DTOs shared across layers.
/// Rank-1 leaf: ToolInput/ToolOutputContent/ToolResult with zero cc.*
/// imports; cc.tools.tool re-exports it so existing importers stay
/// unchanged. has_field stays in cc.tools.tool (it needs cc.utils.json).
export module cc.types.tool_types;

import std;

export namespace cc::core {

// ============================================================
// Tool Input / Output types
// ============================================================

/// JSON-based tool input wrapping raw parameter data
struct ToolInput {
    std::string raw_json;   // Raw JSON string of parameters

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

} // namespace cc::core
