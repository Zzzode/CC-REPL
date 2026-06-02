/// @file betas.cppm
/// @brief Anthropic API beta header constants.
/// Migrated from src/constants/betas.ts
module;

#include <string_view>
#include <unordered_set>
#include <string>

export module cc.constants.betas;

export namespace cc::constants::betas {

inline constexpr std::string_view CLAUDE_CODE_20250219 = "claude-code-20250219";
inline constexpr std::string_view INTERLEAVED_THINKING = "interleaved-thinking-2025-05-14";
inline constexpr std::string_view CONTEXT_1M = "context-1m-2025-08-07";
inline constexpr std::string_view CONTEXT_MANAGEMENT = "context-management-2025-06-27";
inline constexpr std::string_view STRUCTURED_OUTPUTS = "structured-outputs-2025-12-15";
inline constexpr std::string_view WEB_SEARCH = "web-search-2025-03-05";
inline constexpr std::string_view TOOL_SEARCH_1P = "advanced-tool-use-2025-11-20";
inline constexpr std::string_view TOOL_SEARCH_3P = "tool-search-tool-2025-10-19";
inline constexpr std::string_view EFFORT = "effort-2025-11-24";
inline constexpr std::string_view TASK_BUDGETS = "task-budgets-2026-03-13";
inline constexpr std::string_view PROMPT_CACHING_SCOPE = "prompt-caching-scope-2026-01-05";
inline constexpr std::string_view FAST_MODE = "fast-mode-2026-02-01";
inline constexpr std::string_view REDACT_THINKING = "redact-thinking-2026-02-12";
inline constexpr std::string_view TOKEN_EFFICIENT_TOOLS = "token-efficient-tools-2026-03-28";
inline constexpr std::string_view ADVISOR = "advisor-tool-2026-03-01";

/// Betas that Bedrock passes via extraBodyParams, not headers
[[nodiscard]] inline const std::unordered_set<std::string_view>& bedrock_extra_params_headers() {
    static const std::unordered_set<std::string_view> set = {
        INTERLEAVED_THINKING,
        CONTEXT_1M,
        TOOL_SEARCH_3P,
    };
    return set;
}

/// Betas allowed on Vertex countTokens API
[[nodiscard]] inline const std::unordered_set<std::string_view>& vertex_count_tokens_allowed() {
    static const std::unordered_set<std::string_view> set = {
        CLAUDE_CODE_20250219,
        INTERLEAVED_THINKING,
        CONTEXT_MANAGEMENT,
    };
    return set;
}

} // namespace cc::constants::betas
