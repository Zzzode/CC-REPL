// Compact boundary message - marks where conversation history was compacted
module;

#include <string>
#include <optional>
#include <cstdint>
#include <format>

#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_compact_boundary;

export namespace cc::ui::messages::compact_boundary {
using namespace ftxui;

/// Data for a compaction boundary marker
struct CompactBoundaryData {
    int messages_compacted = 0;       // Number of messages that were compacted
    int tokens_saved = 0;             // Tokens freed by compaction
    std::optional<std::string> summary;  // Brief summary of compacted content
    bool is_auto_compact = true;      // Auto vs manual compaction
};

/// Render the compact boundary marker
[[nodiscard]] inline Element render(const CompactBoundaryData& data) {
    Elements parts;
    
    // Divider line with label
    std::string label = data.is_auto_compact ? "auto-compacted" : "compacted";
    
    std::string info;
    if (data.messages_compacted > 0) {
        info += std::format("{} messages", data.messages_compacted);
    }
    if (data.tokens_saved > 0) {
        if (!info.empty()) info += ", ";
        if (data.tokens_saved >= 1000) {
            info += std::format("{:.1f}k tokens saved", data.tokens_saved / 1000.0);
        } else {
            info += std::format("{} tokens saved", data.tokens_saved);
        }
    }
    
    parts.push_back(text("─── ") | dim | color(Color::GrayDark));
    parts.push_back(text(label) | color(Color::Yellow) | dim);
    if (!info.empty()) {
        parts.push_back(text(" (" + info + ")") | dim | color(Color::GrayDark));
    }
    parts.push_back(text(" ") | dim);
    parts.push_back(text("───────────────────") | dim | color(Color::GrayDark));
    
    auto header = hbox(parts);
    
    if (data.summary && !data.summary->empty()) {
        return vbox({
            header,
            text("  " + *data.summary) | dim | color(Color::GrayLight),
        });
    }
    
    return header;
}

} // namespace cc::ui::messages::compact_boundary
