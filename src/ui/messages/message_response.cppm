module;
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>
#include <iomanip>

export module cc.ui.messages.message_response;

export namespace cc::ui::messages {

[[nodiscard]] inline std::string repeat_message_response(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) result += text;
    return result;
}

// A block within an assistant response (text, code, tool usage, etc.)
struct ResponseBlock {
    enum Type { Text, Code, ToolUse, Thinking, Error } type = Text;
    std::string content;
    std::optional<std::string> language;
};

// Render a fenced code block with syntax language label
inline auto render_code_block(std::string_view code,
                              std::string_view language,
                              int width) -> std::string {
    std::ostringstream out;
    int inner_width = std::max(20, width - 4);

    // Top border with language tag
    out << "\033[2m┌─";
    if (!language.empty()) {
        out << " " << language << " ";
    }
    int remaining = inner_width - static_cast<int>(language.size()) - 4;
    if (remaining > 0) {
        out << repeat_message_response("─", remaining);
    }
    out << "┐\033[0m\n";

    // Code lines with line numbers
    int line_num = 1;
    size_t start = 0;
    while (start <= code.size()) {
        auto end = code.find('\n', start);
        std::string_view line;
        if (end == std::string_view::npos) {
            line = code.substr(start);
            start = code.size() + 1;
        } else {
            line = code.substr(start, end - start);
            start = end + 1;
        }

        out << "\033[2m│\033[0m \033[90m" << std::setw(3) << line_num << "\033[0m │ "
            << line << "\n";
        ++line_num;
    }

    // Bottom border
    out << "\033[2m└─" << repeat_message_response("─", inner_width - 2) << "┘\033[0m";

    return out.str();
}

// Render a sequence of response blocks into formatted output
inline auto render_response(std::vector<ResponseBlock> blocks, int width) -> std::string {
    std::ostringstream out;

    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];

        switch (block.type) {
            case ResponseBlock::Text:
                out << block.content;
                break;

            case ResponseBlock::Code:
                out << render_code_block(
                    block.content,
                    block.language.value_or(""),
                    width);
                break;

            case ResponseBlock::ToolUse:
                out << "\033[36m⚡ " << block.content << "\033[0m";
                break;

            case ResponseBlock::Thinking:
                out << "\033[2m💭 " << block.content << "\033[0m";
                break;

            case ResponseBlock::Error:
                out << "\033[31m✗ Error: " << block.content << "\033[0m";
                break;
        }

        if (i < blocks.size() - 1) {
            out << "\n";
        }
    }

    return out.str();
}

} // namespace cc::ui::messages
