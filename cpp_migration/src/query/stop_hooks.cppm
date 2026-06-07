/// @file stop_hooks.cppm
/// @brief Query stop hooks - detecting when to stop the query loop.
/// Migrated from src/query/stopHooks.ts
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <optional>

export module cc.query.stop_hooks;

export namespace cc::query {

/// Reason for stopping a query loop
enum class StopReason : std::uint8_t {
    EndTurn,           // Model ended its turn naturally
    MaxTokens,        // Hit output token limit
    StopSequence,     // Hit a stop sequence
    ToolUse,          // Model wants to use a tool (loop continues)
    UserCancel,       // User pressed Ctrl+C
    Error,            // An error occurred
    ContextExhausted, // Context window full, needs compaction
};

/// Result of checking stop conditions
struct StopCheckResult {
    bool should_stop = false;
    StopReason reason = StopReason::EndTurn;
    std::optional<std::string> message;
};

/// Hook that can be registered to check stop conditions
using StopHook = std::function<StopCheckResult()>;

/// Evaluate all stop hooks in order
[[nodiscard]] inline StopCheckResult evaluate_stop_hooks(
    const std::vector<StopHook>& hooks
) {
    for (const auto& hook : hooks) {
        auto result = hook();
        if (result.should_stop) {
            return result;
        }
    }
    return StopCheckResult{.should_stop = false};
}

/// Create a hook that stops on context exhaustion
[[nodiscard]] inline StopHook make_context_exhaustion_hook(
    std::function<int()> get_current_tokens,
    int max_tokens
) {
    return [get_current_tokens = std::move(get_current_tokens), max_tokens]() -> StopCheckResult {
        if (get_current_tokens() >= max_tokens) {
            return StopCheckResult{
                .should_stop = true,
                .reason = StopReason::ContextExhausted,
                .message = "Context window exhausted, compacting...",
            };
        }
        return StopCheckResult{.should_stop = false};
    };
}

} // namespace cc::query
