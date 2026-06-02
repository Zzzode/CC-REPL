/// @file token_budget.cppm
/// @brief Token budget calculation and management for API requests.
/// Migrated from src/query/tokenBudget.ts
module;

#include <cstdint>
#include <algorithm>
#include <string>
#include <optional>

export module cc.query.token_budget;

export namespace cc::query {

/// Model context window sizes
struct ModelLimits {
    int max_context_tokens = 200'000;  // Default for Claude 3.5/4
    int max_output_tokens = 16'384;    // Default max output
    int reserved_for_output = 16'384;  // Reserve for model response
};

/// Get model limits by model name
[[nodiscard]] inline ModelLimits get_model_limits(std::string_view model_name) {
    // Claude 4 Opus with 1M context
    if (model_name.find("opus") != std::string_view::npos && 
        model_name.find("4") != std::string_view::npos) {
        return ModelLimits{
            .max_context_tokens = 1'000'000,
            .max_output_tokens = 32'000,
            .reserved_for_output = 32'000,
        };
    }
    // Claude 4 Sonnet / Haiku
    if (model_name.find("sonnet") != std::string_view::npos ||
        model_name.find("haiku") != std::string_view::npos) {
        return ModelLimits{
            .max_context_tokens = 200'000,
            .max_output_tokens = 16'384,
            .reserved_for_output = 16'384,
        };
    }
    return ModelLimits{};
}

/// Token budget for a single request
struct TokenBudget {
    int total_available = 0;       // max_context - reserved_for_output
    int system_prompt_budget = 0;  // Budget for system prompt
    int messages_budget = 0;       // Budget for conversation messages
    int tools_budget = 0;          // Budget for tool definitions
};

/// Calculate token budget for a request
[[nodiscard]] inline TokenBudget calculate_budget(
    const ModelLimits& limits,
    int system_prompt_tokens = 0,
    int tools_tokens = 0
) {
    int available = limits.max_context_tokens - limits.reserved_for_output;
    int remaining = available - system_prompt_tokens - tools_tokens;
    
    return TokenBudget{
        .total_available = available,
        .system_prompt_budget = system_prompt_tokens,
        .messages_budget = std::max(0, remaining),
        .tools_budget = tools_tokens,
    };
}

} // namespace cc::query
