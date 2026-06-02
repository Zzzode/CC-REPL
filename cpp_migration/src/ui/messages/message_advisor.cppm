/// @file message_advisor.cppm
/// @brief Advisor/suggestion message rendering
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_advisor;

export namespace cc::ui::messages {

using namespace ftxui;

/// Advisor message severity
enum class AdvisorSeverity {
    Info,
    Suggestion,
    Warning,
    Error,
};

/// Advisor message entry
struct AdvisorMessage {
    std::string content;
    AdvisorSeverity severity{AdvisorSeverity::Info};
    std::optional<std::string> action_label;
    std::optional<std::string> source;
};

/// Render advisor message
[[nodiscard]] inline Element render_advisor_message(const AdvisorMessage& msg) {
    auto [prefix, prefix_color] = [&]() -> std::pair<std::string, Color> {
        switch (msg.severity) {
            case AdvisorSeverity::Info: return {"ℹ", Color::Cyan};
            case AdvisorSeverity::Suggestion: return {"💡", Color::Green};
            case AdvisorSeverity::Warning: return {"⚠", Color::Yellow};
            case AdvisorSeverity::Error: return {"✗", Color::Red};
        }
        return {"?", Color::White};
    }();

    std::vector<Element> elements;
    elements.push_back(hbox({
        text(prefix) | color(prefix_color),
        text(" " + msg.content),
    }));

    if (msg.action_label) {
        elements.push_back(text("  → " + *msg.action_label) | dim);
    }

    return vbox(elements);
}

} // namespace cc::ui::messages
