/// @file message_tool_result.cppm
/// @brief Tool result message rendering (success/error states)
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <variant>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.messages.message_tool_result;

import cc.types.types;

export namespace cc::ui::messages {

using namespace ftxui;

/// Tool result status
enum class ToolResultStatus {
    Success,
    Error,
    Timeout,
    Cancelled,
};

/// Tool result display options
struct ToolResultOptions {
    std::string tool_name;
    ToolResultStatus status{ToolResultStatus::Success};
    std::optional<std::string> output;
    std::optional<std::string> error_message;
    std::optional<double> duration_ms;
    bool is_truncated{false};
};

/// Render tool result message
[[nodiscard]] inline Element render_tool_result(const ToolResultOptions& opts) {
    auto status_indicator = [&]() -> Element {
        switch (opts.status) {
            case ToolResultStatus::Success: return text("✓") | color(Color::Green);
            case ToolResultStatus::Error: return text("✗") | color(Color::Red);
            case ToolResultStatus::Timeout: return text("⏱") | color(Color::Yellow);
            case ToolResultStatus::Cancelled: return text("⊘") | color(Color::GrayDark);
        }
        return text("?");
    }();

    std::vector<Element> elements;
    elements.push_back(hbox({
        status_indicator, text(" "),
        text(opts.tool_name) | bold,
        opts.duration_ms ? (text(std::format(" ({:.0f}ms)", *opts.duration_ms)) | dim) : text(""),
    }));

    if (opts.output && !opts.output->empty()) {
        elements.push_back(text(*opts.output) | dim);
    }
    if (opts.error_message) {
        elements.push_back(text(*opts.error_message) | color(Color::Red));
    }
    if (opts.is_truncated) {
        elements.push_back(text("  (output truncated)") | dim);
    }

    return vbox(elements);
}

} // namespace cc::ui::messages
