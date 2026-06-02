/// @file system.cppm
/// @brief System prompt prefix and attribution header constants.
/// Migrated from src/constants/system.ts
module;

#include <string>
#include <string_view>
#include <unordered_set>

export module cc.constants.system;

export namespace cc::constants::system {

/// Default system prompt prefix for interactive CLI
inline constexpr std::string_view DEFAULT_PREFIX = 
    "You are Claude Code, Anthropic's official CLI for Claude.";

/// Prefix for Agent SDK in claude-code preset mode
inline constexpr std::string_view AGENT_SDK_CLAUDE_CODE_PRESET_PREFIX = 
    "You are Claude Code, Anthropic's official CLI for Claude, running within the Claude Agent SDK.";

/// Prefix for generic Agent SDK usage
inline constexpr std::string_view AGENT_SDK_PREFIX = 
    "You are a Claude agent, built on Anthropic's Claude Agent SDK.";

/// All possible prefix values (for splitting system prompts)
[[nodiscard]] inline const std::unordered_set<std::string_view>& cli_sysprompt_prefixes() {
    static const std::unordered_set<std::string_view> prefixes = {
        DEFAULT_PREFIX,
        AGENT_SDK_CLAUDE_CODE_PRESET_PREFIX,
        AGENT_SDK_PREFIX,
    };
    return prefixes;
}

/// Get the appropriate system prompt prefix based on context
[[nodiscard]] inline std::string_view get_cli_sysprompt_prefix(
    bool is_non_interactive = false,
    bool has_append_system_prompt = false
) {
    if (is_non_interactive) {
        if (has_append_system_prompt) {
            return AGENT_SDK_CLAUDE_CODE_PRESET_PREFIX;
        }
        return AGENT_SDK_PREFIX;
    }
    return DEFAULT_PREFIX;
}

} // namespace cc::constants::system
