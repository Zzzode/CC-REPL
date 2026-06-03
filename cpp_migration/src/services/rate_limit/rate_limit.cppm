module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

export module cc.services.rate_limit;


export namespace cc::services {


enum class RateLimitTier { free, pro, team, enterprise };


struct RateLimitInfo {
    int64_t requests_remaining{-1};
    int64_t tokens_remaining{-1};
    std::chrono::system_clock::time_point reset_at{};
    std::chrono::milliseconds retry_after{0};
};


struct RateLimitMockConfig {
    bool simulate_429{false};
    int remaining_requests{0};
    int remaining_tokens{0};
    std::chrono::milliseconds retry_after{1000};
};


struct RateLimitState {
    RateLimitInfo current_info{};
    RateLimitTier tier{RateLimitTier::pro};
    bool is_mocked{false};
    RateLimitMockConfig mock_config{};
    int consecutive_429s{0};
    std::chrono::system_clock::time_point last_request_at{};
};


class RateLimitManager {
    RateLimitState state_{};
    std::mt19937 rng_{std::random_device{}()};

public:

    void update_from_headers(const std::unordered_map<std::string, std::string>& headers) {
        if (auto it = headers.find("x-ratelimit-remaining-requests"); it != headers.end())
            state_.current_info.requests_remaining = std::stoll(it->second);
        if (auto it = headers.find("x-ratelimit-remaining-tokens"); it != headers.end())
            state_.current_info.tokens_remaining = std::stoll(it->second);
        if (auto it = headers.find("retry-after"); it != headers.end())
            state_.current_info.retry_after = std::chrono::milliseconds(std::stoll(it->second) * 1000);

        state_.consecutive_429s = 0;
    }


    void record_rate_limit_hit() {
        ++state_.consecutive_429s;
    }


    [[nodiscard]] auto should_retry() const -> bool {
        return state_.consecutive_429s < 5;
    }


    [[nodiscard]] auto get_retry_delay() -> std::chrono::milliseconds {
        if (state_.current_info.retry_after.count() > 0)
            return state_.current_info.retry_after;
        

        int64_t base_ms = 1000LL * (1LL << std::min(state_.consecutive_429s, 4));

        std::uniform_int_distribution<int64_t> jitter(-base_ms / 4, base_ms / 4);
        return std::chrono::milliseconds(base_ms + jitter(rng_));
    }


    [[nodiscard]] auto get_warning_message() const -> std::optional<std::string> {
        if (state_.current_info.requests_remaining >= 0 && state_.current_info.requests_remaining < 5)
            return "接近请求限制，剩余 " + std::to_string(state_.current_info.requests_remaining) + " 次请求";
        if (state_.current_info.tokens_remaining >= 0 && state_.current_info.tokens_remaining < 10000)
            return "接近 token 限制，剩余 " + std::to_string(state_.current_info.tokens_remaining) + " tokens";
        if (state_.consecutive_429s > 0)
            return "已触发限流，正在等待重试 (第 " + std::to_string(state_.consecutive_429s) + " 次)";
        return std::nullopt;
    }


    [[nodiscard]] auto is_rate_limited() const -> bool {
        if (state_.is_mocked) return state_.mock_config.simulate_429;
        return state_.consecutive_429s > 0 || state_.current_info.requests_remaining == 0;
    }


    void mock_rate_limit(RateLimitMockConfig config) {
        state_.is_mocked = true;
        state_.mock_config = config;
    }

    void clear_mock() { state_.is_mocked = false; }


    [[nodiscard]] static auto get_tier_limits(RateLimitTier tier) -> RateLimitInfo {
        switch (tier) {
            case RateLimitTier::free: return {.requests_remaining = 50, .tokens_remaining = 40'000};
            case RateLimitTier::pro: return {.requests_remaining = 1000, .tokens_remaining = 800'000};
            case RateLimitTier::team: return {.requests_remaining = 2000, .tokens_remaining = 1'600'000};
            case RateLimitTier::enterprise: return {.requests_remaining = 4000, .tokens_remaining = 4'000'000};
        }
        return {};
    }


    void record_request(size_t /*input_tokens*/, size_t /*output_tokens*/) {
        state_.last_request_at = std::chrono::system_clock::now();
        if (state_.current_info.requests_remaining > 0) --state_.current_info.requests_remaining;
    }

    void reset() { state_ = RateLimitState{}; }
    [[nodiscard]] auto get_state() const -> const RateLimitState& { return state_; }
};

} // namespace cc::services
