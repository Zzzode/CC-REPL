/// @file message_rate_limit.cppm
/// @brief Rate limit warning message rendering
module;

#include <string>
#include <optional>
#include <chrono>
#include <format>
#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_rate_limit;

export namespace cc::ui::messages {

using namespace ftxui;

/// Rate limit info for display
struct RateLimitInfo {
    std::string model;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<double> tokens_remaining;
    std::optional<double> requests_remaining;
    bool is_hard_limit{false};
};

/// Render rate limit message
[[nodiscard]] inline Element render_rate_limit_message(const RateLimitInfo& info) {
    std::vector<Element> elements;

    auto header = info.is_hard_limit
        ? text("Rate limit reached") | bold | color(Color::Red)
        : text("Approaching rate limit") | bold | color(Color::Yellow);
    elements.push_back(header);

    elements.push_back(text("  Model: " + info.model) | dim);

    if (info.retry_after) {
        elements.push_back(text(std::format("  Retry after: {}s", info.retry_after->count())) | dim);
    }
    if (info.tokens_remaining) {
        elements.push_back(text(std::format("  Tokens remaining: {:.0f}", *info.tokens_remaining)) | dim);
    }
    if (info.requests_remaining) {
        elements.push_back(text(std::format("  Requests remaining: {:.0f}", *info.requests_remaining)) | dim);
    }

    return vbox(elements);
}

} // namespace cc::ui::messages
