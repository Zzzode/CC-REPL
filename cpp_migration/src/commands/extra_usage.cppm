module;
#include <chrono>
#include <format>
#include <string>
#include <string_view>
export module cc.commands.extra_usage;

import cc.services.api.admin_requests;

export namespace cc::commands::extra_usage {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "extra_usage"; }

[[nodiscard]] inline auto run(std::string_view action = {}) -> CommandResponse {
    if (action.empty() || action == "status" || action == "check") {
        const auto status = cc::services::api::check_extra_usage_status();
        std::string message = std::format("Extra usage: {}", status.granted ? "granted" : "not granted");
        if (status.expires) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                status.expires->time_since_epoch()).count();
            message += "\nExpires at epoch seconds: " + std::to_string(seconds);
        }
        return {.ok = true, .message = std::move(message)};
    }

    if (action == "request" || action == "activate" || action == "buy") {
        auto requested = cc::services::api::request_extra_usage();
        if (!requested) return {.ok = false, .message = requested.error()};
        return {.ok = true, .message = "Extra usage request accepted locally; no remote admin grant is active"};
    }

    return {.ok = false, .message = "extra-usage supports: status, check, request"};
}
}
