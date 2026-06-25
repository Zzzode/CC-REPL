// API Error Handling - Comprehensive error types and utilities for Anthropic API
module;
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

export module cc.services.api.errors;

import cc.services.api.models;
import cc.utils.json;

export namespace cc::services::api::errors {

using cc::services::api::Provider;

// =========================================================================
// API Error Categories
// =========================================================================

enum class ApiErrorCategory {
    Authentication,      // 401/403 - auth failed, token expired, etc.
    InvalidRequest,      // 400 - bad request, invalid parameters
    RateLimited,         // 429 - too many requests
    ServerError,         // 5xx - server-side errors
    NetworkError,        // Connection issues, timeouts
    Overloaded,          // 529 - server overloaded
    Unknown              // Catch-all
};

// =========================================================================
// API Error Details
// =========================================================================

struct ApiErrorDetails {
    ApiErrorCategory category = ApiErrorCategory::Unknown;
    int http_status = 0;
    std::string error_type;
    std::string error_message;
    std::optional<std::string> request_id;

    // Rate limit specific
    std::optional<int> retry_after_seconds;
    std::optional<std::string> rate_limit_type;

    // Overloaded error specific
    std::optional<bool> is_retryable;

    [[nodiscard]] std::string to_string() const {
        return std::format("[HTTP {}] {}: {} - {}",
            http_status, error_type, error_message,
            request_id ? *request_id : "no request id");
    }
};

// =========================================================================
// Retry Context
// =========================================================================

struct RetryContext {
    int attempt = 0;
    int max_attempts = 10;
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{32000};
    std::chrono::steady_clock::time_point first_attempt_time;
    int consecutive_529_errors = 0;
    std::optional<std::string> fallback_model;
    bool fast_mode_active = false;
};

// =========================================================================
// Retry Decision
// =========================================================================

enum class RetryDecision {
    RetryImmediately,
    RetryWithDelay,
    FallbackModel,
    NoRetry,
    Abort
};

// =========================================================================
// Error Classifier
// =========================================================================

class ErrorClassifier {
public:
    // Classify HTTP status code to error category
    [[nodiscard]] static ApiErrorCategory classify_status(int http_status) {
        if (http_status == 401 || http_status == 403) {
            return ApiErrorCategory::Authentication;
        }
        if (http_status == 429) {
            return ApiErrorCategory::RateLimited;
        }
        if (http_status == 529) {
            return ApiErrorCategory::Overloaded;
        }
        if (http_status >= 400 && http_status < 500) {
            return ApiErrorCategory::InvalidRequest;
        }
        if (http_status >= 500) {
            return ApiErrorCategory::ServerError;
        }
        return ApiErrorCategory::Unknown;
    }

    // Check if error is retryable
    [[nodiscard]] static bool is_retryable(const ApiErrorDetails& error) {
        switch (error.category) {
            case ApiErrorCategory::RateLimited:
            case ApiErrorCategory::ServerError:
            case ApiErrorCategory::NetworkError:
            case ApiErrorCategory::Overloaded:
                return true;
            case ApiErrorCategory::Authentication:
                // Only retry auth errors if it might be a transient issue
                return error.error_message.contains("expired") ||
                       error.error_message.contains("timeout");
            case ApiErrorCategory::InvalidRequest:
            case ApiErrorCategory::Unknown:
            default:
                return false;
        }
    }

    // Check if error should trigger model fallback
    [[nodiscard]] static bool should_fallback(const ApiErrorDetails& error,
                                             const RetryContext& context) {
        if (error.category == ApiErrorCategory::Overloaded &&
            context.consecutive_529_errors >= 3) {
            return true;
        }
        return false;
    }

    // Calculate retry delay with exponential backoff and jitter
    [[nodiscard]] static std::chrono::milliseconds calculate_delay(
        const RetryContext& context,
        const ApiErrorDetails& error) {

        // Use retry-after header if provided
        if (error.retry_after_seconds) {
            return std::chrono::seconds(*error.retry_after_seconds);
        }

        // Exponential backoff: base_delay * (2^attempt)
        auto delay = context.base_delay * (1 << context.attempt);

        // Cap at max_delay
        if (delay > context.max_delay) {
            delay = context.max_delay;
        }

        // Add jitter (±20%)
        auto jitter = static_cast<double>(std::rand()) / RAND_MAX * 0.4 - 0.2;
        delay = std::chrono::milliseconds(
            static_cast<long long>(delay.count() * (1 + jitter)));

        return delay;
    }

    // Determine retry decision
    [[nodiscard]] static RetryDecision decide(
        const ApiErrorDetails& error,
        RetryContext& context) {

        if (context.attempt >= context.max_attempts) {
            return RetryDecision::Abort;
        }

        if (!is_retryable(error)) {
            return RetryDecision::NoRetry;
        }

        if (should_fallback(error, context) && context.fallback_model) {
            return RetryDecision::FallbackModel;
        }

        // Track consecutive 529 errors
        if (error.category == ApiErrorCategory::Overloaded) {
            ++context.consecutive_529_errors;
        } else {
            context.consecutive_529_errors = 0;
        }

        // Check for retry-after header
        if (error.retry_after_seconds && *error.retry_after_seconds > 0) {
            return RetryDecision::RetryWithDelay;
        }

        return RetryDecision::RetryWithDelay;
    }
};

// =========================================================================
// Error Factory
// =========================================================================

class ErrorFactory {
public:
    // Create from HTTP status and message
    [[nodiscard]] static ApiErrorDetails from_http(
        int http_status,
        std::string_view message,
        std::optional<std::string> request_id = std::nullopt) {

        ApiErrorDetails error;
        error.http_status = http_status;
        error.category = ErrorClassifier::classify_status(http_status);
        error.request_id = std::move(request_id);

        // Parse error message to extract more details
        parse_error_message(message, error);

        return error;
    }

    // Create from JSON error response
    [[nodiscard]] static ApiErrorDetails from_json(
        int http_status,
        std::string_view json_body,
        std::optional<std::string> request_id = std::nullopt) {

        ApiErrorDetails error;
        error.http_status = http_status;
        error.category = ErrorClassifier::classify_status(http_status);
        error.request_id = std::move(request_id);

        parse_json_error(json_body, error);

        return error;
    }

    // Create network error
    [[nodiscard]] static ApiErrorDetails network_error(
        std::string_view message) {
        ApiErrorDetails error;
        error.category = ApiErrorCategory::NetworkError;
        error.error_type = "network_error";
        error.error_message = std::string(message);
        return error;
    }

    // Create timeout error
    [[nodiscard]] static ApiErrorDetails timeout_error(
        std::chrono::milliseconds timeout) {
        ApiErrorDetails error;
        error.category = ApiErrorCategory::NetworkError;
        error.error_type = "timeout";
        error.error_message = std::format("Request timed out after {}ms",
            timeout.count());
        return error;
    }

    // Create parse error
    [[nodiscard]] static ApiErrorDetails parse_error(
        std::string_view message) {
        ApiErrorDetails error;
        error.category = ApiErrorCategory::InvalidRequest;
        error.error_type = "parse_error";
        error.error_message = std::string(message);
        return error;
    }

private:
    static void parse_error_message(std::string_view message,
                                   ApiErrorDetails& error) {
        // Simple heuristic parsing
        if (message.contains("overloaded") || message.contains("529")) {
            error.category = ApiErrorCategory::Overloaded;
            error.error_type = "overloaded_error";
            error.is_retryable = true;
        } else if (message.contains("rate_limit") || message.contains("429")) {
            error.category = ApiErrorCategory::RateLimited;
            error.error_type = "rate_limit_error";
        } else if (message.contains("authentication") ||
                   message.contains("auth") ||
                   message.contains("401") ||
                   message.contains("403")) {
            error.category = ApiErrorCategory::Authentication;
            error.error_type = "authentication_error";
        } else {
            error.error_type = "api_error";
        }
        error.error_message = std::string(message);
    }

    static void parse_json_error(std::string_view json_body,
                                ApiErrorDetails& error) {
        auto parsed = cc::utils::json::parse(json_body);
        if (!parsed) {
            error.error_message = std::string(json_body);
            error.error_type = "api_error";
            return;
        }
        auto root = parsed->root();
        auto error_obj = root.get("error");
        if (!error_obj.valid() || !error_obj.is_obj()) {
            error_obj = root;
        }

        if (auto type = error_obj.get("type"); type.valid() && type.is_str()) {
            error.error_type = std::string(type.as_str());
        } else {
            error.error_type = "api_error";
        }
        if (auto message = error_obj.get("message"); message.valid() && message.is_str()) {
            error.error_message = std::string(message.as_str());
        } else if (auto message = root.get("message"); message.valid() && message.is_str()) {
            error.error_message = std::string(message.as_str());
        } else {
            error.error_message = std::string(json_body);
        }
        if (auto retry_after = error_obj.get("retry_after_seconds"); retry_after.valid() && retry_after.is_num()) {
            error.retry_after_seconds = static_cast<int>(retry_after.as_int());
        }
        if (auto rate_limit_type = error_obj.get("rate_limit_type"); rate_limit_type.valid() && rate_limit_type.is_str()) {
            error.rate_limit_type = std::string(rate_limit_type.as_str());
        }
    }
};

// =========================================================================
// Provider-Specific Error Messages
// =========================================================================

class ProviderErrorMessages {
public:
    [[nodiscard]] static std::string_view model_not_found(
        Provider provider,
        std::string_view) {
        switch (provider) {
            case Provider::Anthropic:
                return "Model not found";
            case Provider::Bedrock:
                return "You don't have access to the model with the specified model ID";
            case Provider::Vertex:
                return "Model not found or not accessible";
        }
        return "Model not found";
    }

    [[nodiscard]] static std::string_view quota_exceeded(Provider provider) {
        switch (provider) {
            case Provider::Anthropic:
                return "Credit balance is too low";
            case Provider::Bedrock:
                return "Bedrock service limits exceeded";
            case Provider::Vertex:
                return "Vertex AI quota exceeded";
        }
        return "Quota exceeded";
    }
};

// =========================================================================
// User-Facing Error Messages
// =========================================================================

class UserFacingErrors {
public:
    [[nodiscard]] static std::string format_for_user(
        const ApiErrorDetails& error,
        bool is_interactive = true) {

        switch (error.category) {
            case ApiErrorCategory::Authentication:
                return is_interactive
                    ? "Please run /login to authenticate"
                    : "Authentication failed";

            case ApiErrorCategory::RateLimited:
                if (error.retry_after_seconds) {
                    return std::format("Rate limited. Retry after {}s",
                        *error.retry_after_seconds);
                }
                return "Rate limit exceeded. Please try again later.";

            case ApiErrorCategory::Overloaded:
                return "Server is overloaded. Please try again later or switch models.";

            case ApiErrorCategory::InvalidRequest:
                if (error.error_message.contains("prompt is too long")) {
                    return "Prompt is too long. Try /compact to reduce context.";
                }
                if (error.error_message.contains("image")) {
                    return "Image error. Try removing images or using smaller ones.";
                }
                return std::format("Invalid request: {}", error.error_message);

            case ApiErrorCategory::NetworkError:
                return "Network error. Check your connection and try again.";

            case ApiErrorCategory::ServerError:
            case ApiErrorCategory::Unknown:
            default:
                return std::format("API error: {}", error.error_message);
        }
    }

    [[nodiscard]] static std::string prompt_too_long(
        std::optional<int> actual_tokens,
        std::optional<int> limit_tokens,
        bool is_interactive = true) {

        if (is_interactive) {
            if (actual_tokens && limit_tokens) {
                return std::format(
                    "Prompt is too long ({} tokens > {} limit). "
                    "Run /compact to reduce context.",
                    *actual_tokens, *limit_tokens);
            }
            return "Prompt is too long. Run /compact to reduce context.";
        }
        return "Prompt is too long. Try reducing the context.";
    }

    [[nodiscard]] static std::string credit_balance_low() {
        return "Credit balance is too low. Please add more credits.";
    }

    [[nodiscard]] static std::string model_not_available(
        std::string_view model_id,
        std::optional<std::string> fallback_suggestion = std::nullopt) {

        if (fallback_suggestion) {
            return std::format(
                "Model {} is not available. Try switching to {}.",
                model_id, *fallback_suggestion);
        }
        return std::format("Model {} is not available.", model_id);
    }
};

} // namespace cc::services::api::errors
