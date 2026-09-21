/// @file message_bash_io.cppm
/// @brief Bash stdin/stdout/stderr message rendering
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <format>
#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_bash_io;

export namespace cc::ui::messages {

using namespace ftxui;

/// Stream type for bash output
enum class BashStream {
    Stdout,
    Stderr,
    Stdin,
};

/// Bash I/O display entry
struct BashIOEntry {
    BashStream stream{BashStream::Stdout};
    std::string content;
    std::optional<int> exit_code;
};

/// Render bash I/O entry
[[nodiscard]] inline Element render_bash_io(const BashIOEntry& entry) {
    auto stream_color = [&]() -> Color {
        switch (entry.stream) {
            case BashStream::Stdout: return Color::White;
            case BashStream::Stderr: return Color::Red;
            case BashStream::Stdin: return Color::Cyan;
        }
        return Color::White;
    }();

    std::vector<Element> elements;
    elements.push_back(text(entry.content) | color(stream_color));

    if (entry.exit_code) {
        auto code_color = *entry.exit_code == 0 ? Color::Green : Color::Red;
        elements.push_back(text(std::format("exit code: {}", *entry.exit_code)) | color(code_color) | dim);
    }

    return vbox(elements);
}

/// Render combined bash output (stdout + stderr interleaved)
[[nodiscard]] inline Element render_bash_output(const std::vector<BashIOEntry>& entries) {
    std::vector<Element> elements;
    for (const auto& entry : entries) {
        elements.push_back(render_bash_io(entry));
    }
    return vbox(elements);
}

} // namespace cc::ui::messages
