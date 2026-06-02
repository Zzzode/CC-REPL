/// @file claude_api_content.cppm
/// @brief Claude API content formatting skill - message construction patterns.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.claude_api_content;

import cc.skills.skill;

export namespace cc::skills::claude_api_content {

/// Claude API content formatting skill definition
[[nodiscard]] inline SkillDefinition make_claude_api_content_skill() {
    return SkillDefinition{
        .name = "claude-api-content",
        .description = "Content formatting and message construction for Claude API",
        .trigger_patterns = {
            R"(api\s+content)",
            R"(message\s+format)",
            R"(content\s+block)",
            R"(tool_use\s+(?:result|block))",
        },
        .content = R"(## Claude API Content Formatting

### Message Structure
- Messages alternate between 'user' and 'assistant' roles
- Content can be text or structured content blocks
- Image content uses base64 or URL sources
- Tool results must reference the tool_use_id

### Content Blocks
- text: Plain text content
- image: Base64 or URL image data
- tool_use: Tool invocation from assistant
- tool_result: Response to tool_use from user

### Multi-modal Content
- Combine text and images in a single message
- Images support PNG, JPEG, GIF, WebP
- Use appropriate media_type for each image
- Keep base64 images under 20MB

### Tool Use Pattern
1. Define tools in the request
2. Assistant responds with tool_use blocks
3. User sends tool_result with matching id
4. Continue conversation with results
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::claude_api_content
