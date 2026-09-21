// Redacted thinking message - displays placeholder for filtered thinking content
module;

#include <string>
#include <string_view>
#include <optional>

#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_redacted_thinking;

export namespace cc::ui::messages::redacted_thinking {
using namespace ftxui;

/// Data for a redacted thinking message
struct RedactedThinkingData {
    std::string reason;          // Why thinking was redacted
    int block_index = 0;        // Index in the content blocks
    bool show_placeholder = true;
};

/// Render a redacted thinking block
[[nodiscard]] inline Element render(const RedactedThinkingData& data) {
    Elements parts;
    
    parts.push_back(text("🔒 ") | dim);
    parts.push_back(text("Thinking (redacted)") | color(Color::GrayLight) | dim);
    
    if (!data.reason.empty()) {
        parts.push_back(text(" — " + data.reason) | color(Color::GrayDark) | dim);
    }
    
    auto header = hbox(parts);
    
    if (data.show_placeholder) {
        return vbox({
            header,
            text("  This thinking content has been filtered for safety.") | dim | color(Color::GrayDark),
        }) | borderLight | color(Color::GrayDark);
    }
    
    return header;
}

} // namespace cc::ui::messages::redacted_thinking
