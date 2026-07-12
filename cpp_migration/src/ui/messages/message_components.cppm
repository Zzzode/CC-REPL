module;
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.messages.message_components;

import cc.ui.layout;
import cc.ui.design.figures;  // kBlackCircleFallback (● U+25CF) — single source of truth

export namespace cc::ui::messages {

// --- Error constants ---
inline constexpr std::string_view kApiErrorPrefix = "API Error: ";
inline constexpr std::string_view kApiTimeoutError = "Request timed out. Please try again.";
inline constexpr std::string_view kInvalidApiKeyError = "Invalid API key. Please check your configuration.";
inline constexpr std::string_view kCreditBalanceTooLowError = "Your credit balance is too low.";
inline constexpr std::string_view kPromptTooLongError = "Your prompt is too long for the current model's context window.";
inline constexpr std::string_view kOrgDisabledError = "Your organization has disabled this feature.";
inline constexpr std::string_view kTokenRevokedError = "Your API token has been revoked.";
inline constexpr std::string_view kUserAbortError = "[interrupted by user]";
inline constexpr std::string_view kNoResponseRequested = "[no response requested]";
inline constexpr int kMaxApiErrorChars = 1000;
// kBlackCircle (● U+25CF) — formerly defined locally, now imported from
// cc::ui::design::figures::kBlackCircleFallback.  See figures.cppm for the
// Darwin-correct kBlackCircle (⏺ U+23FA) and platform fallback rationale.

// --- Message block types ---
enum class BlockType {
    Text,
    Code,
    ToolUse,
    ToolResult,
    Thinking,
    Image,
    Error
};

struct TextBlockParam {
    std::string text;
    BlockType type;
};

inline auto make_text_block_param(std::string text) -> TextBlockParam {
    return TextBlockParam{.text = std::move(text), .type = BlockType::Text};
}

// --- AssistantTextMessage props ---
struct AssistantTextMessageProps {
    TextBlockParam param;
    bool add_margin;
    bool should_show_dot;
    bool verbose;
    std::optional<int> width;
    std::optional<std::function<void()>> on_open_rate_limit_options;
};

struct AssistantTextMessagePropsRequired {
    TextBlockParam param;
    bool add_margin;
    bool should_show_dot;
    bool verbose;
};

inline auto make_assistant_text_message_props(AssistantTextMessagePropsRequired required)
    -> AssistantTextMessageProps {
    return AssistantTextMessageProps{
        .param = std::move(required.param),
        .add_margin = required.add_margin,
        .should_show_dot = required.should_show_dot,
        .verbose = required.verbose,
        .width = std::nullopt,
        .on_open_rate_limit_options = std::nullopt
    };
}

// --- Rate limit message (UNIFIED definition, canonical location) ---
// Fields represent the union of what rate-limit renderers, audit previews,
// and row-dispatch code access.  All are optional / default-initialised
// so callers can fill only the subset they know about.
struct RateLimitInfo {
    std::string reason;                 ///< Short reason (shown in preview)
    std::string message;                ///< Human-readable description
    std::string model;                  ///< Affected model name
    std::string raw_text;               ///< Original error string (for verbosity)
    int retry_after_seconds{0};         ///< Legacy integer seconds fallback
    std::optional<std::chrono::seconds> retry_after;   ///< Precise backoff
    std::optional<double> tokens_remaining;
    std::optional<double> requests_remaining;
    bool is_hard_limit{false};          ///< 429 hard stop vs warning
    bool is_overloaded{false};          ///< "server overloaded" vs plain 429
};

// --- Shared layout helpers (indent / padding Decorators) ---
/// Return a Decorator that indents content by N spaces on the left.
[[nodiscard]] inline ftxui::Decorator indent(int n) {
    using namespace ftxui;
    return [n](Element e) {
        return hbox({
            ftxui::text(std::string(static_cast<std::size_t>(n), ' ')),
            std::move(e)
        });
    };
}

/// CSS-style padding decorator.
[[nodiscard]] inline ftxui::Decorator padding(int top, int right, int bottom, int left) {
    using namespace ftxui;
    return [=](Element e) -> Element {
        std::vector<Element> lines;
        lines.reserve(static_cast<std::size_t>(top) + 1 +
                      static_cast<std::size_t>(bottom));
        const std::string h_pad_l(static_cast<std::size_t>(left), ' ');
        const std::string h_pad_r(static_cast<std::size_t>(right), ' ');
        for (int i = 0; i < top; ++i)
            lines.push_back(ftxui::text(h_pad_l + h_pad_r));
        lines.push_back(ftxui::hbox({
            ftxui::text(h_pad_l), std::move(e), ftxui::text(h_pad_r)
        }));
        for (int i = 0; i < bottom; ++i)
            lines.push_back(ftxui::text(h_pad_l + h_pad_r));
        return ftxui::vbox(std::move(lines));
    };
}

/// Uniform padding on all four sides.
[[nodiscard]] inline ftxui::Decorator padding(int all) {
    return padding(all, all, all, all);
}

/// Horizontal + vertical padding.
[[nodiscard]] inline ftxui::Decorator padding(int h, int v) {
    return padding(v, h, v, h);
};

// --- Message rendering error types ---
enum class MessageRenderError {
    InvalidApiKey,
    TokenRevoked,
    CreditBalanceLow,
    PromptTooLong,
    OrgDisabled,
    ApiTimeout,
    RateLimit,
    UserAbort,
    GenericApi
};

// Classify an error message string into a known error type
[[nodiscard]] inline auto classify_error(std::string_view text) -> std::optional<MessageRenderError> {
    if (text.find("Invalid API key") != std::string_view::npos ||
        text.find("invalid_api_key") != std::string_view::npos) {
        return MessageRenderError::InvalidApiKey;
    }
    if (text.find("token") != std::string_view::npos &&
        text.find("revoked") != std::string_view::npos) {
        return MessageRenderError::TokenRevoked;
    }
    if (text.find("credit balance") != std::string_view::npos) {
        return MessageRenderError::CreditBalanceLow;
    }
    if (text.find("too long") != std::string_view::npos ||
        text.find("context window") != std::string_view::npos) {
        return MessageRenderError::PromptTooLong;
    }
    if (text.find("organization") != std::string_view::npos &&
        text.find("disabled") != std::string_view::npos) {
        return MessageRenderError::OrgDisabled;
    }
    if (text.find("timed out") != std::string_view::npos) {
        return MessageRenderError::ApiTimeout;
    }
    if (text.find("rate limit") != std::string_view::npos ||
        text.find("overloaded") != std::string_view::npos) {
        return MessageRenderError::RateLimit;
    }
    if (text == kUserAbortError) {
        return MessageRenderError::UserAbort;
    }
    if (text.starts_with(kApiErrorPrefix)) {
        return MessageRenderError::GenericApi;
    }
    return std::nullopt;
}

// Check if text constitutes an empty/no-content message
[[nodiscard]] inline auto is_empty_message_text(std::string_view text) -> bool {
    if (text.empty()) return true;
    if (text == kNoResponseRequested) return true;
    // Trim whitespace check
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

// Check if the message is a rate limit error
[[nodiscard]] inline auto is_rate_limit_error(std::string_view text) -> bool {
    return text.find("rate limit") != std::string_view::npos ||
           text.find("overloaded") != std::string_view::npos ||
           text.find("429") != std::string_view::npos;
}

// Truncate API error message to max display length
[[nodiscard]] inline auto truncate_api_error(std::string_view error) -> std::string {
    if (static_cast<int>(error.size()) <= kMaxApiErrorChars) {
        return std::string(error);
    }
    return std::string(error.substr(0, static_cast<std::size_t>(kMaxApiErrorChars))) + "...";
}

// --- Render functions ---

// Render an error message with appropriate styling
[[nodiscard]] inline auto render_error_message(MessageRenderError error_type,
                                                std::string_view raw_text)
    -> std::string {
    switch (error_type) {
        case MessageRenderError::InvalidApiKey:
            return "\033[31m" + std::string(kInvalidApiKeyError) + "\033[0m";
        case MessageRenderError::TokenRevoked:
            return "\033[31m" + std::string(kTokenRevokedError) + "\033[0m";
        case MessageRenderError::CreditBalanceLow:
            return "\033[31m" + std::string(kCreditBalanceTooLowError) + "\033[0m";
        case MessageRenderError::PromptTooLong:
            return "\033[33m" + std::string(kPromptTooLongError) + "\033[0m";
        case MessageRenderError::OrgDisabled:
            return "\033[31m" + std::string(kOrgDisabledError) + "\033[0m";
        case MessageRenderError::ApiTimeout:
            return "\033[33m" + std::string(kApiTimeoutError) + "\033[0m";
        case MessageRenderError::RateLimit:
            return "\033[33mRate limited. Retrying...\033[0m";
        case MessageRenderError::UserAbort:
            return "\033[2m" + std::string(kUserAbortError) + "\033[0m";
        case MessageRenderError::GenericApi:
            return "\033[31m" + truncate_api_error(raw_text) + "\033[0m";
    }
    return std::string(raw_text);
}

// Render the assistant text message element
[[nodiscard]] inline auto render_assistant_text_message(
    const AssistantTextMessageProps& props,
    int terminal_width)
    -> std::expected<std::string, std::string> {

    const auto& text = props.param.text;

    // Check for empty message
    if (is_empty_message_text(text)) {
        if (props.should_show_dot) {
            return std::string(cc::ui::design::figures::kBlackCircleFallback);
        }
        return std::string{};
    }

    // Check for error conditions
    if (auto error_type = classify_error(text)) {
        if (*error_type == MessageRenderError::RateLimit && props.on_open_rate_limit_options) {
            // Caller can invoke the callback externally
        }
        return render_error_message(*error_type, text);
    }

    // Normal text rendering with optional margin
    std::string result;
    if (props.add_margin) {
        result += "\n";
    }

    // Apply width constraint
    int width = props.width.value_or(terminal_width);
    if (width <= 0) width = 80;

    // For verbose mode, render raw text; otherwise apply markdown formatting
    if (props.verbose) {
        result += text;
    } else {
        // Markdown rendering delegated to markdown module
        result += text;
    }

    return result;
}

// --- InterruptedByUser indicator ---
[[nodiscard]] inline auto render_interrupted_by_user() -> std::string {
    return "\033[2m" + std::string(kUserAbortError) + "\033[0m";
}

// --- FTXUI element factories ---

// Create an FTXUI Element for an assistant text message
[[nodiscard]] auto make_assistant_text_element(
    const AssistantTextMessageProps& props,
    int terminal_width) -> ftxui::Element;

// Create an interactive FTXUI Component for rate-limit messages with retry button
[[nodiscard]] auto make_rate_limit_component(
    const RateLimitInfo& info,
    std::function<void()> on_retry) -> ftxui::Component;

} // namespace cc::ui::messages
