/// @file config.cppm
/// @brief Query configuration and dependencies.
/// Migrated from src/query/config.ts, deps.ts
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>

export module cc.query.config;

export namespace cc::query {

/// Effort level for model inference
enum class EffortLevel : std::uint8_t {
    Low,
    Medium,
    High,
    Max,
};

/// Query configuration options
struct QueryConfig {
    std::string model;
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    EffortLevel effort = EffortLevel::High;
    bool enable_thinking = true;
    int thinking_budget = 10'000;
    bool enable_caching = true;
    std::optional<std::chrono::milliseconds> timeout;
    std::vector<std::string> stop_sequences;
    bool stream = true;
};

/// Dependencies injected into the query engine
struct QueryDeps {
    /// Callback to get current system prompt
    std::function<std::string()> get_system_prompt;
    /// Callback to get tool definitions
    std::function<std::string()> get_tool_definitions;
    /// Callback for token counting
    std::function<int(std::string_view)> count_tokens;
    /// Callback for analytics events
    std::function<void(std::string_view event_name, std::string_view payload)> track_event;
};

/// Default query config
[[nodiscard]] inline QueryConfig default_query_config(std::string_view model = "claude-sonnet-4-20250514") {
    return QueryConfig{
        .model = std::string(model),
        .max_tokens = 16'384,
        .effort = EffortLevel::High,
        .enable_thinking = true,
        .thinking_budget = 10'000,
        .enable_caching = true,
        .stream = true,
    };
}

} // namespace cc::query
