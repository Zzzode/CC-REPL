/// @file claude_ai_limits_hook.cppm
/// @brief Rate limit hook for Claude AI API responses.
/// Implements exponential backoff, state tracking, and callback notification
/// when rate limits are encountered.
module;

#include <string>
#include <string_view>
#include <optional>
#include <cstdint>
#include <functional>
#include <mutex>
#include <chrono>
#include <format>
#include <algorithm>
#include <vector>

export module cc.services.rate_limit.claude_ai_limits_hook;

export namespace cc::services::rate_limit {

/// Rate limit hook state — tracks current limiting status
struct RateLimitHookState {
    bool is_rate_limited{false};
    std::optional<int> retry_after_seconds;
    std::uint64_t total_retries{0};
    std::optional<std::string> last_limit_hit;
    std::chrono::steady_clock::time_point limited_until;
    std::uint32_t consecutive_limits{0};
};

/// Rate limit event types
enum class RateLimitEvent : std::uint8_t {
    LimitHit,           // New rate limit encountered
    RetryScheduled,     // Retry scheduled after backoff
    RetrySucceeded,     // Request succeeded after retry
    LimitLifted,        // Rate limit period ended
};

/// Callback for rate limit events
using RateLimitCallback = std::function<void(const RateLimitHookState&)>;

/// Event callback with event type information
using RateLimitEventCallback = std::function<void(RateLimitEvent, const RateLimitHookState&)>;

/// Rate limit hook configuration
struct RateLimitConfig {
    std::uint32_t initial_backoff_ms{1000};     // Initial backoff: 1s
    std::uint32_t max_backoff_ms{60000};        // Max backoff: 60s
    double backoff_multiplier{2.0};              // Exponential factor
    std::uint32_t max_consecutive_retries{5};   // Give up after this many
    bool jitter_enabled{true};                  // Add randomized jitter
};

/// Rate limit hook — monitors API responses and manages backoff
class RateLimitHook {
public:
    RateLimitHook() = default;
    explicit RateLimitHook(RateLimitConfig config) : config_(config) {}

    /// Install a callback for rate limit state changes
    void install(RateLimitCallback on_limit) {
        std::lock_guard lock(mutex_);
        callbacks_.push_back(std::move(on_limit));
    }

    /// Install an event-typed callback
    void on_event(RateLimitEventCallback callback) {
        std::lock_guard lock(mutex_);
        event_callbacks_.push_back(std::move(callback));
    }

    /// Check current rate limit state
    [[nodiscard]] RateLimitHookState check_state() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    /// Check if we should delay before making a request
    [[nodiscard]] bool should_wait() const {
        std::lock_guard lock(mutex_);
        if (!state_.is_rate_limited) return false;
        return std::chrono::steady_clock::now() < state_.limited_until;
    }

    /// Get remaining wait time in milliseconds (0 if not limited)
    [[nodiscard]] std::uint32_t remaining_wait_ms() const {
        std::lock_guard lock(mutex_);
        if (!state_.is_rate_limited) return 0;
        auto now = std::chrono::steady_clock::now();
        if (now >= state_.limited_until) return 0;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            state_.limited_until - now);
        return static_cast<std::uint32_t>(remaining.count());
    }

    /// Handle a rate limit response (HTTP 429 or 529)
    /// Returns true if retry should be attempted, false if we should give up
    [[nodiscard]] bool handle_rate_limit_response(
        int status_code,
        std::string_view retry_after_header) {

        std::lock_guard lock(mutex_);

        // Only handle rate limit status codes
        if (status_code != 429 && status_code != 529) return false;

        state_.is_rate_limited = true;
        state_.consecutive_limits++;
        state_.total_retries++;

        // Parse Retry-After header
        std::optional<int> retry_after;
        if (!retry_after_header.empty()) {
            try {
                retry_after = std::stoi(std::string(retry_after_header));
            } catch (...) {
                // Header might be a date string; use default backoff
            }
        }

        // Calculate backoff duration
        std::uint32_t backoff_ms;
        if (retry_after.has_value() && *retry_after > 0) {
            backoff_ms = static_cast<std::uint32_t>(*retry_after) * 1000;
            state_.retry_after_seconds = *retry_after;
        } else {
            // Exponential backoff
            double base = static_cast<double>(config_.initial_backoff_ms);
            for (std::uint32_t i = 1; i < state_.consecutive_limits; ++i) {
                base *= config_.backoff_multiplier;
            }
            backoff_ms = static_cast<std::uint32_t>(
                std::min(base, static_cast<double>(config_.max_backoff_ms)));
            state_.retry_after_seconds = static_cast<int>(backoff_ms / 1000);
        }

        // Set the limited_until time
        state_.limited_until = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(backoff_ms);

        // Record which limit was hit
        state_.last_limit_hit = std::format("HTTP {} at {}",
            status_code,
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        // Check if we should give up
        if (state_.consecutive_limits > config_.max_consecutive_retries) {
            notify_event(RateLimitEvent::LimitHit);
            return false; // Give up
        }

        // Notify callbacks
        notify_event(RateLimitEvent::RetryScheduled);
        return true; // Retry
    }

    /// Reset rate limit state after a successful request
    void reset_on_success() {
        std::lock_guard lock(mutex_);
        if (state_.is_rate_limited) {
            state_.is_rate_limited = false;
            state_.retry_after_seconds = std::nullopt;
            state_.consecutive_limits = 0;
            notify_event(RateLimitEvent::RetrySucceeded);
        }
    }

    /// Force reset all state (e.g., on session restart)
    void reset() {
        std::lock_guard lock(mutex_);
        state_ = RateLimitHookState{};
    }

    /// Get summary string for display
    [[nodiscard]] std::string format_state() const {
        std::lock_guard lock(mutex_);
        if (!state_.is_rate_limited) {
            return "Rate limit: OK (no active limits)";
        }
        auto wait = remaining_wait_ms_locked();
        return std::format(
            "Rate limited: retry in {}ms (attempt {}/{})",
            wait, state_.consecutive_limits, config_.max_consecutive_retries);
    }

private:
    /// Notify all registered callbacks (must be called with lock held)
    void notify_event(RateLimitEvent event) {
        for (const auto& cb : callbacks_) {
            if (cb) cb(state_);
        }
        for (const auto& cb : event_callbacks_) {
            if (cb) cb(event, state_);
        }
    }

    /// Get remaining wait without locking (caller holds lock)
    [[nodiscard]] std::uint32_t remaining_wait_ms_locked() const {
        if (!state_.is_rate_limited) return 0;
        auto now = std::chrono::steady_clock::now();
        if (now >= state_.limited_until) return 0;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            state_.limited_until - now);
        return static_cast<std::uint32_t>(remaining.count());
    }

    RateLimitConfig config_;
    RateLimitHookState state_;
    mutable std::mutex mutex_;
    std::vector<RateLimitCallback> callbacks_;
    std::vector<RateLimitEventCallback> event_callbacks_;
};

// ============================================================
// Module-level convenience API
// ============================================================

namespace detail {
    inline RateLimitHook global_hook;
}

/// Install the rate limit hook
inline void install_rate_limit_hook(RateLimitCallback on_limit) {
    detail::global_hook.install(std::move(on_limit));
}

/// Check current rate limit state
inline RateLimitHookState check_rate_limit_state() {
    return detail::global_hook.check_state();
}

/// Handle a rate limit response
inline bool handle_rate_limit_response(int status_code,
                                       std::string_view retry_after_header) {
    return detail::global_hook.handle_rate_limit_response(status_code, retry_after_header);
}

/// Reset rate limit state after successful request
inline void reset_rate_limit_state() {
    detail::global_hook.reset_on_success();
}

/// Force clear all rate limit state.
inline void clear_rate_limit_state() {
    detail::global_hook.reset();
}

} // namespace cc::services::rate_limit
