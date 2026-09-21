module;
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
export module cc.services.api.error_utils;

export namespace cc::services::api {

// Structured API error representation
struct ApiError {
    int status_code{0};
    std::string error_type;
    std::string message;
    std::optional<std::string> request_id;
};

// Parse API error from response body and status code
auto parse_api_error(std::string_view response_body, int status) -> ApiError {
    ApiError error;
    error.status_code = status;

    // Simple JSON-like parsing for error fields
    if (auto pos = response_body.find("\"type\""); pos != std::string_view::npos) {
        auto start = response_body.find('"', pos + 6);
        auto end = response_body.find('"', start + 1);
        if (start != std::string_view::npos && end != std::string_view::npos) {
            error.error_type = std::string(response_body.substr(start + 1, end - start - 1));
        }
    }

    if (auto pos = response_body.find("\"message\""); pos != std::string_view::npos) {
        auto start = response_body.find('"', pos + 9);
        auto end = response_body.find('"', start + 1);
        if (start != std::string_view::npos && end != std::string_view::npos) {
            error.message = std::string(response_body.substr(start + 1, end - start - 1));
        }
    }

    return error;
}

// Check if error indicates rate limiting
auto is_rate_limited(const ApiError& error) -> bool {
    return error.status_code == 429;
}

// Check if error indicates server overload
auto is_overloaded(const ApiError& error) -> bool {
    return error.status_code == 529 || error.error_type == "overloaded_error";
}

// Extract retry-after duration from error
auto get_retry_after(const ApiError& error) -> std::optional<std::chrono::seconds> {
    if (!is_rate_limited(error) && !is_overloaded(error)) {
        return std::nullopt;
    }
    // Default retry after 30 seconds if not specified
    return std::chrono::seconds{30};
}

// Format error for display
auto format_api_error(const ApiError& error) -> std::string {
    std::string result = "API Error [" + std::to_string(error.status_code) + "]";
    if (!error.error_type.empty()) {
        result += " (" + error.error_type + ")";
    }
    result += ": " + error.message;
    if (error.request_id) {
        result += " [request_id: " + *error.request_id + "]";
    }
    return result;
}

} // namespace cc::services::api
