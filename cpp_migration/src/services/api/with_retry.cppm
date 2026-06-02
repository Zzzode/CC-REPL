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
        // In real implementation, extract details from the error
        ApiErrorDetails details;
        details.error_message = error.message();
        details.category = cc::services::api::errors::ApiErrorCategory::Unknown;
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

} // namespace cc::services::api
