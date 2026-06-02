module;
#include <string>
#include <string_view>
#include <optional>
#include <cstdint>

export module cc.services.api.claude_ai_limits;

export namespace cc::services::api {

/// Rate limit tier information
struct RateLimitTier {
    std::string tier_name;
    std::uint64_t requests_per_minute{0};
    std::uint64_t tokens_per_minute{0};
    std::uint64_t tokens_per_day{0};
};

/// Current usage against limits
struct UsageAgainstLimits {
    std::uint64_t requests_used{0};
    std::uint64_t tokens_used{0};
    std::uint64_t requests_remaining{0};
    std::uint64_t tokens_remaining{0};
    std::optional<std::string> reset_at;
};

/// Empty usage sentinel
inline UsageAgainstLimits empty_usage() {
    return {};
}

/// Check if usage is approaching limit
inline bool is_approaching_limit(const UsageAgainstLimits& usage, double threshold = 0.9) {
    if (usage.requests_remaining == 0 && usage.requests_used == 0) return false;
    auto total = usage.requests_used + usage.requests_remaining;
    if (total == 0) return false;
    return static_cast<double>(usage.requests_used) / static_cast<double>(total) >= threshold;
}

/// Get rate limit tier for a model
inline std::optional<RateLimitTier> get_rate_limit_tier(std::string_view model_id) {
    if (model_id.empty()) return std::nullopt;

    // Known tier mappings for Claude models
    if (model_id.find("opus") != std::string_view::npos) {
        return RateLimitTier{
            .tier_name = "tier-4",
            .requests_per_minute = 2000,
            .tokens_per_minute = 80000,
            .tokens_per_day = 2500000,
        };
    }
    if (model_id.find("sonnet") != std::string_view::npos) {
        return RateLimitTier{
            .tier_name = "tier-4",
            .requests_per_minute = 4000,
            .tokens_per_minute = 160000,
            .tokens_per_day = 5000000,
        };
    }
    if (model_id.find("haiku") != std::string_view::npos) {
        return RateLimitTier{
            .tier_name = "tier-4",
            .requests_per_minute = 8000,
            .tokens_per_minute = 400000,
            .tokens_per_day = 25000000,
        };
    }

    // Default tier for unknown models
    return RateLimitTier{
        .tier_name = "tier-1",
        .requests_per_minute = 50,
        .tokens_per_minute = 40000,
        .tokens_per_day = 1000000,
    };
}

} // namespace cc::services::api
