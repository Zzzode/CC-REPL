module;
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
export module cc.services.api.admin_requests;

export namespace cc::services::api {

using time_point = std::chrono::system_clock::time_point;

// Extra usage grant status
struct ExtraUsageStatus {
    bool granted{false};
    std::optional<time_point> expires;
};

// Request additional usage quota from admin
auto request_extra_usage() -> std::expected<void, std::string> {
    // Request is accepted locally; no admin transport is configured here.
    return {};
}

// Check status of extra usage request
auto check_extra_usage_status() -> ExtraUsageStatus {
    // Default status is not granted until a remote admin source is wired in.
    return ExtraUsageStatus{};
}

// Submit user feedback to the platform
auto submit_feedback(std::string_view message, std::string_view category)
    -> std::expected<void, std::string> {
    if (message.empty()) {
        return std::unexpected("Feedback message cannot be empty");
    }
    if (category.empty()) {
        return std::unexpected("Feedback category is required");
    }
    // Feedback is validated and accepted locally by this migration module.
    return {};
}

} // namespace cc::services::api
