/// @file message_channel.cppm
/// @brief Channel/thread message rendering
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_channel;

export namespace cc::ui::messages {

using namespace ftxui;

/// Channel message source
enum class ChannelSource {
    User,
    Assistant,
    System,
    Tool,
};

/// Channel message entry
struct ChannelMessageEntry {
    ChannelSource source{ChannelSource::User};
    std::string content;
    std::optional<std::string> sender_name;
    std::chrono::system_clock::time_point timestamp;
};

/// Render channel message
[[nodiscard]] inline Element render_channel_message(const ChannelMessageEntry& entry) {
    auto source_color = [&]() -> Color {
        switch (entry.source) {
            case ChannelSource::User: return Color::Cyan;
            case ChannelSource::Assistant: return Color::Green;
            case ChannelSource::System: return Color::Yellow;
            case ChannelSource::Tool: return Color::Magenta;
        }
        return Color::White;
    }();

    auto sender = entry.sender_name.value_or([&]{
        switch (entry.source) {
            case ChannelSource::User: return "user";
            case ChannelSource::Assistant: return "assistant";
            case ChannelSource::System: return "system";
            case ChannelSource::Tool: return "tool";
        }
        return "unknown";
    }());

    return hbox({
        text(sender) | bold | color(source_color),
        text(": "),
        text(entry.content),
    });
}

} // namespace cc::ui::messages
