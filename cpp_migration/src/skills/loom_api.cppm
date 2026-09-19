/// @file loom_api.cppm
/// @brief Loom API skill - direct API interaction patterns.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.loom_api;

import cc.skills.skill;

export namespace cc::skills::loom_api {

/// Loom API interaction skill definition
[[nodiscard]] inline SkillDefinition make_loom_api_skill() {
    return SkillDefinition{
        .name = "loom-api",
        .description = "Direct Loom API interaction patterns for programmatic use",
        .trigger_patterns = {
            R"(loom\s+api)",
            R"(api\s+(?:call|request|endpoint))",
            R"(programmatic.*loom)",
            R"(sdk.*(?:usage|example))",
        },
        .content = R"(## Loom API Skill

### Direct API Usage
- Use the Messages API for multi-turn conversations
- Set appropriate model, max_tokens, and system prompt
- Handle rate limits with exponential backoff
- Stream responses for long-running operations

### Best Practices
- Always set a system prompt for consistent behavior
- Use temperature=0 for deterministic outputs
- Prefer structured tool_use over free-form text parsing
- Cache conversation context efficiently

### Error Handling
- 429: Rate limited - implement backoff
- 529: Overloaded - retry with delay
- 400: Malformed request - validate inputs
- 401: Invalid API key - check credentials
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::loom_api
