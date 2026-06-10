// API Retry Logic - Exponential backoff with jitter, fallback, and persistence
module;
#include <algorithm>
#include <chrono>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

export module cc.services.api.with_retry;

import cc.services.api.errors;
import cc.services.api.models;
import cc.utils.error;

export namespace cc::services::api {

using cc::services::api::errors::ApiErrorDetails;
using cc::services::api::errors::ErrorClassifier;
using cc::services::api::errors::RetryContext;
using cc::services::api::errors::RetryDecision;
using cc::utils::Result;

// =========================================================================
// Retry Configuration
// =========================================================================

struct RetryConfig {
    int max_retries = 10;
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{32000};
    std::optional<std::string> fallback_model;
    bool enable_persistent_retry = false;
    std::chrono::milliseconds persistent_max_backoff{300000}; // 5min
    std::chrono::milliseconds heartbeat_interval{30000}; // 30s
};

// =========================================================================
// Retry Result
// =========================================================================

template <typename T>
struct RetryResult {
    T value;
    int attempts = 1;
    bool used_fallback = false;
    std::chrono::milliseconds total_delay{0};
};

// =========================================================================
// Retry Callback Types
// =========================================================================

template <typename T>
using RetryOperation = std::function<Result<T>(RetryContext&)>;

using ProgressCallback = std::function<void(int attempt, std::chrono::milliseconds delay, const ApiErrorDetails& error)>;

// =========================================================================
// With Retry Executor
// =========================================================================

template <typename T>
class RetryExecutor {
public:
    explicit RetryExecutor(RetryConfig config)
        : config_(std::move(config)) {}

    // Execute with retry logic
    [[nodiscard]] Result<RetryResult<T>> execute(
        RetryOperation<T> operation,
        ProgressCallback progress = nullptr) {

        RetryContext context;
        context.max_attempts = config_.max_retries;
        context.base_delay = config_.base_delay;
        context.max_delay = config_.max_delay;
        context.fallback_model = config_.fallback_model;
        context.first_attempt_time = std::chrono::steady_clock::now();

        RetryResult<T> result;
        std::chrono::milliseconds total_delay{0};

        while (true) {
            // Check if we've exceeded max attempts (for non-persistent mode)
            if (!config_.enable_persistent_retry && context.attempt >= config_.max_retries) {
                return std::unexpected(cc::utils::Error(
                    cc::utils::ErrorCode::internal_error,
                    "Max retry attempts exceeded"));
            }

            ++context.attempt;

            try {
                // Execute the operation
                auto op_result = operation(context);
                if (op_result) {
                    result.value = std::move(*op_result);
                    result.attempts = context.attempt;
                    result.total_delay = total_delay;
                    return result;
                }

                // Handle error
                auto error = extract_error_details(op_result.error());
                auto decision = ErrorClassifier::decide(error, context);

                // Check if we should retry
                switch (decision) {
                    case RetryDecision::NoRetry:
                    case RetryDecision::Abort:
                        return std::unexpected(op_result.error());

                    case RetryDecision::FallbackModel:
                        if (config_.fallback_model) {
                            result.used_fallback = true;
                            // Fallback will be handled in next attempt via context
                            continue;
                        }
                        return std::unexpected(op_result.error());

                    case RetryDecision::RetryImmediately:
                    case RetryDecision::RetryWithDelay:
                        // Calculate delay
                        auto delay = ErrorClassifier::calculate_delay(context, error);
                        
                        // For persistent mode, cap at persistent_max_backoff
                        if (config_.enable_persistent_retry && delay > config_.persistent_max_backoff) {
                            delay = config_.persistent_max_backoff;
                        }

                        total_delay += delay;

                        // Report progress
                        if (progress) {
                            progress(context.attempt, delay, error);
                        }

                        // Wait
                        if (config_.enable_persistent_retry && delay > config_.heartbeat_interval) {
                            // For long waits, split into heartbeats
                            auto remaining = delay;
                            while (remaining > std::chrono::milliseconds{0}) {
                                auto chunk = std::min(remaining, config_.heartbeat_interval);
                                std::this_thread::sleep_for(chunk);
                                remaining -= chunk;
                            }
                        } else {
                            std::this_thread::sleep_for(delay);
                        }

                        continue;
                }
            } catch (const std::exception& e) {
                // Handle unexpected exceptions
                return std::unexpected(cc::utils::Error(
                    cc::utils::ErrorCode::internal_error,
                    std::format("Unexpected exception during retry: {}", e.what())));
            }
        }
    }

private:
    [[nodiscard]] ApiErrorDetails extract_error_details(const cc::utils::Error& error) {
        ApiErrorDetails details;
        details.error_message = error.message();
        switch (error.code()) {
            case cc::utils::ErrorCode::network_error:
            case cc::utils::ErrorCode::unavailable:
                details.category = cc::services::api::errors::ApiErrorCategory::NetworkError;
                details.error_type = "network_error";
                break;
            case cc::utils::ErrorCode::timeout:
                details.category = cc::services::api::errors::ApiErrorCategory::NetworkError;
                details.error_type = "timeout";
                break;
            case cc::utils::ErrorCode::permission_denied:
                details.category = cc::services::api::errors::ApiErrorCategory::Authentication;
                details.error_type = "authentication_error";
                break;
            default:
                details.category = cc::services::api::errors::ApiErrorCategory::Unknown;
                details.error_type = "api_error";
                break;
        }
        return details;
    }

    RetryConfig config_;
};

// =========================================================================
// Helper Functions
// =========================================================================

template <typename T>
[[nodiscard]] Result<RetryResult<T>> with_retry(
    RetryOperation<T> operation,
    RetryConfig config = {},
    ProgressCallback progress = nullptr) {

    RetryExecutor<T> executor(std::move(config));
    return executor.execute(std::move(operation), std::move(progress));
}

// Calculate exponential backoff with jitter
[[nodiscard]] inline std::chrono::milliseconds calculate_backoff(
    int attempt,
    std::chrono::milliseconds base_delay,
    std::chrono::milliseconds max_delay) {

    // Exponential backoff: base * 2^(attempt-1)
    auto delay = base_delay * (1 << (attempt - 1));
    
    // Cap at max delay
    if (delay > max_delay) {
        delay = max_delay;
    }

    // Add jitter (±20%)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.8, 1.2);
    delay = std::chrono::milliseconds(static_cast<long long>(delay.count() * dis(gen)));

    return delay;
}

// Get retry-after from error if available
[[nodiscard]] inline std::optional<std::chrono::seconds> get_retry_after(
    const ApiErrorDetails& error) {
    if (error.retry_after_seconds) {
        return std::chrono::seconds(*error.retry_after_seconds);
    }
    return std::nullopt;
}

// =========================================================================
// Task #37 dispatch compatibility layer (lightweight WithRetry: expected<fn(), string>)
// Independent from RetryExecutor / RetryConfig above; avoids coupling to errors/models.
// =========================================================================

/// Lightweight retry config required by dispatch (pure value struct, no dependencies).
struct RetryConfigLite {
    int              max_attempts    = 3;
    int              base_delay_ms   = 500;
    int              max_delay_ms    = 10000;
    std::vector<int> retry_on_http   = {429, 500, 502, 503, 504};
};

namespace detail {
inline std::chrono::milliseconds lite_backoff(int attempt_0based,
                                              int base_ms,
                                              int max_ms) {
    const auto a = std::max(0, attempt_0based);
    const std::int64_t raw = static_cast<std::int64_t>(base_ms) * (1 << a);
    auto capped = std::chrono::milliseconds(std::min<std::int64_t>(
        static_cast<std::int64_t>(max_ms), raw));
    // +/-20% jitter
    std::mt19937_64 gen{std::random_device{}()};
    std::uniform_real_distribution<double> d(0.8, 1.2);
    return std::chrono::milliseconds(static_cast<std::int64_t>(
        std::max<double>(0.0,
                         static_cast<double>(capped.count()) * d(gen))));
}
}  // namespace detail

/// Dispatch-required template entry point: executes fn up to cfg.max_attempts times.
/// - On success returns `expected<T, string>` value
/// - Retries if fn returns negative, throws std::exception (converted to string error),
///   or returns an HTTP code in cfg.retry_on_http; otherwise returns unexpected<string>.
///
/// fn return value conventions (dispatched via concepts-like overloads):
///   A) `int`  -> 0/2xx means success; >0 in retry_on_http retries; <0 = transport error retries
///   B) `expected<T, string>`  -> judged by semantics directly
///   C) any other return type -> treated as one-shot success (no retry)
template <typename F>
[[nodiscard]] auto WithRetry(const RetryConfigLite& cfg, F&& fn)
    -> std::expected<
        std::conditional_t<
            std::is_void_v<std::invoke_result_t<F>>,
            std::monostate,
            std::invoke_result_t<F>>,
        std::string> {

    using Ret = std::invoke_result_t<F>;
    using ExpRet = typename std::conditional_t<
        std::is_void_v<Ret>,
        std::monostate,
        Ret>;

    const int max_att = std::max(1, cfg.max_attempts);
    std::string last_err;

    for (int attempt = 0; attempt < max_att; ++attempt) {
        try {
            if constexpr (std::is_same_v<Ret, int>) {
                const int rc = std::invoke(std::forward<F>(fn));
                if (rc == 0 || (rc >= 200 && rc < 300)) {
                    return std::expected<ExpRet, std::string>(rc);
                }
                const bool retry = rc < 0 ||
                    std::find(cfg.retry_on_http.begin(),
                              cfg.retry_on_http.end(), rc) != cfg.retry_on_http.end();
                last_err = std::string("rc=") + std::to_string(rc);
                if (!retry || attempt + 1 == max_att) {
                    return std::unexpected(std::move(last_err));
                }
            } else if constexpr (requires { typename Ret::value_type; typename Ret::error_type;
                                           std::declval<Ret>().has_value(); }) {
                // expected<T, string> pattern
                auto result = std::invoke(std::forward<F>(fn));
                if (result.has_value()) {
                    if constexpr (std::is_void_v<Ret>) {
                        return std::expected<ExpRet, std::string>(std::monostate{});
                    } else {
                        return std::expected<ExpRet, std::string>(*result);
                    }
                }
                last_err = std::string(result.error());
                if (attempt + 1 == max_att) {
                    return std::unexpected(std::move(last_err));
                }
            } else {
                // Any other return type: treat as one-shot success
                if constexpr (std::is_void_v<Ret>) {
                    std::invoke(std::forward<F>(fn));
                    return std::expected<ExpRet, std::string>(std::monostate{});
                } else {
                    return std::expected<ExpRet, std::string>(
                        std::invoke(std::forward<F>(fn)));
                }
            }
        } catch (const std::exception& e) {
            last_err = std::string("exception: ") + e.what();
            if (attempt + 1 == max_att) {
                return std::unexpected(std::move(last_err));
            }
        } catch (...) {
            last_err = "exception: unknown";
            if (attempt + 1 == max_att) {
                return std::unexpected(std::move(last_err));
            }
        }
        // Backoff before retry
        std::this_thread::sleep_for(
            detail::lite_backoff(attempt, cfg.base_delay_ms, cfg.max_delay_ms));
    }
    return std::unexpected(std::move(last_err));
}

} // namespace cc::services::api
