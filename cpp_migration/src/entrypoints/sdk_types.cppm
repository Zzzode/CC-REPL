/// @file sdk_types.cppm
/// @brief SDK type definitions for external integration.
/// Migrated from src/entrypoints/sdk/ (coreTypes, controlTypes, toolTypes, etc.)
module;

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <chrono>
#include <cstdint>

export module cc.entrypoints.sdk_types;

export namespace cc::sdk {

/// SDK message role
enum class Role : std::uint8_t {
    User,
    Assistant,
    System,
};

/// SDK content block types
enum class ContentBlockType : std::uint8_t {
    Text,
    ToolUse,
    ToolResult,
    Thinking,
    Image,
};

/// SDK text content block
struct TextBlock {
    std::string text;
};

/// SDK tool use content block
struct ToolUseBlock {
    std::string id;
    std::string name;
    std::string input_json;
};

/// SDK tool result content block
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
};

/// SDK thinking content block
struct ThinkingBlock {
    std::string thinking;
};

/// Union of content block types
using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock>;

/// SDK message
struct Message {
    Role role;
    std::vector<ContentBlock> content;
    std::optional<std::string> model;
    std::optional<std::string> stop_reason;
};

/// Control request types
enum class ControlRequestType : std::uint8_t {
    StopTask,
    BackgroundTask,
    ForegroundTask,
    Compact,
    SetModel,
    InjectMessage,
};

/// Control request
struct ControlRequest {
    ControlRequestType type;
    std::optional<std::string> task_id;
    std::optional<std::string> message;
    std::optional<std::string> model;
};

/// Session info returned by SDK
struct SessionInfo {
    std::string session_id;
    std::string model;
    std::string cwd;
    bool is_running = false;
    int message_count = 0;
};

} // namespace cc::sdk
