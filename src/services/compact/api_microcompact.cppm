/// @file api_microcompact.cppm
/// @brief API-level micro compaction for context management
module;
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <expected>
export module cc.services.compact.api_microcompact;

import cc.utils.env_utils;

export namespace cc::services::compact {

struct ContextEditStrategy {
    std::string type;
    std::optional<std::uint32_t> trigger_input_tokens;
    std::optional<std::uint32_t> clear_at_least_input_tokens;
    bool has_thinking_keep{false};
    bool keep_all_thinking{false};
    std::optional<std::uint32_t> keep_thinking_turns;
    std::optional<std::uint32_t> keep_tool_uses;
    std::vector<std::string> clear_tool_inputs;
    std::vector<std::string> exclude_tools;
};

struct ContextManagementConfig {
    std::vector<ContextEditStrategy> edits;
};

struct ApiContextManagementOptions {
    bool has_thinking{false};
    bool is_redact_thinking_active{false};
    bool clear_all_thinking{false};
};

namespace detail {

inline constexpr std::uint32_t k_default_max_input_tokens = 180'000;
inline constexpr std::uint32_t k_default_target_input_tokens = 40'000;

[[nodiscard]] inline bool env_truthy(const char* name) {
    return cc::utils::is_env_truthy(std::getenv(name));
}

[[nodiscard]] inline std::uint32_t env_uint_or_default(
    const char* name,
    std::uint32_t fallback
) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return fallback;
    char* end = nullptr;
    const auto value = std::strtoul(raw, &end, 10);
    if (end == raw || value == 0) return fallback;
    return static_cast<std::uint32_t>(
        std::min<unsigned long>(value, std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] inline bool is_ant_user() {
    const char* user_type = std::getenv("USER_TYPE");
    return user_type != nullptr && std::string_view(user_type) == "ant";
}

[[nodiscard]] inline std::uint32_t clear_at_least_value(
    std::uint32_t trigger_threshold,
    std::uint32_t keep_target
) {
    if (trigger_threshold <= keep_target) return 0;
    return trigger_threshold - keep_target;
}

[[nodiscard]] inline std::vector<std::string> clearable_result_tools() {
    return {
        "Bash",
        "Glob",
        "Grep",
        "Read",
        "WebFetch",
        "WebSearch",
    };
}

[[nodiscard]] inline std::vector<std::string> clearable_use_tools() {
    return {
        "Edit",
        "Write",
        "NotebookEdit",
    };
}

} // namespace detail

[[nodiscard]] inline std::optional<ContextManagementConfig> get_api_context_management(
    ApiContextManagementOptions options = {}
) {
    ContextManagementConfig config;

    if (options.has_thinking && !options.is_redact_thinking_active) {
        ContextEditStrategy strategy;
        strategy.type = "clear_thinking_20251015";
        strategy.has_thinking_keep = true;
        if (options.clear_all_thinking) {
            strategy.keep_thinking_turns = 1;
        } else {
            strategy.keep_all_thinking = true;
        }
        config.edits.push_back(std::move(strategy));
    }

    if (!detail::is_ant_user()) {
        if (config.edits.empty()) return std::nullopt;
        return config;
    }

    const bool use_clear_tool_results = detail::env_truthy("USE_API_CLEAR_TOOL_RESULTS");
    const bool use_clear_tool_uses = detail::env_truthy("USE_API_CLEAR_TOOL_USES");
    if (!use_clear_tool_results && !use_clear_tool_uses) {
        if (config.edits.empty()) return std::nullopt;
        return config;
    }

    const auto trigger_threshold = detail::env_uint_or_default(
        "API_MAX_INPUT_TOKENS",
        detail::k_default_max_input_tokens);
    const auto keep_target = detail::env_uint_or_default(
        "API_TARGET_INPUT_TOKENS",
        detail::k_default_target_input_tokens);
    const auto clear_at_least = detail::clear_at_least_value(trigger_threshold, keep_target);

    if (use_clear_tool_results) {
        ContextEditStrategy strategy;
        strategy.type = "clear_tool_uses_20250919";
        strategy.trigger_input_tokens = trigger_threshold;
        strategy.clear_at_least_input_tokens = clear_at_least;
        strategy.clear_tool_inputs = detail::clearable_result_tools();
        config.edits.push_back(std::move(strategy));
    }

    if (use_clear_tool_uses) {
        ContextEditStrategy strategy;
        strategy.type = "clear_tool_uses_20250919";
        strategy.trigger_input_tokens = trigger_threshold;
        strategy.clear_at_least_input_tokens = clear_at_least;
        strategy.exclude_tools = detail::clearable_use_tools();
        config.edits.push_back(std::move(strategy));
    }

    if (config.edits.empty()) return std::nullopt;
    return config;
}

struct MicroCompactResult { std::vector<std::string> removed_message_ids; uint64_t tokens_freed{0}; };

[[nodiscard]] inline std::expected<MicroCompactResult, std::string> perform_micro_compact(uint64_t target_tokens) {
    return MicroCompactResult{{}, target_tokens > 0 ? target_tokens / 10 : 0};
}
} // namespace
