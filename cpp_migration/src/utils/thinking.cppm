module;
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

export module cc.utils.thinking;

export namespace cc::utils {

struct ThinkingBlock {
    std::string content;
    std::chrono::milliseconds duration;
};

// Check if thinking/extended thinking is enabled for a model
bool is_thinking_enabled(std::string_view model_id) {
    // All Claude 4 models support thinking
    return model_id.find("claude-sonnet-4") != std::string_view::npos ||
           model_id.find("claude-opus-4") != std::string_view::npos ||
           model_id.find("claude-haiku-4") != std::string_view::npos;
}

// Format a thinking block for display
std::string format_thinking_block(const ThinkingBlock& block) {
    std::string result;

    // Header with duration
    auto ms = block.duration.count();
    result += "\033[2m"; // Dim
    result += "── Thinking";
    if (ms > 0) {
        if (ms >= 1000) {
            result += " (" + std::to_string(ms / 1000) + "." + std::to_string((ms % 1000) / 100) + "s)";
        } else {
            result += " (" + std::to_string(ms) + "ms)";
        }
    }
    result += " ──\n";

    // Content (indented, dimmed)
    std::string_view content = block.content;
    std::size_t pos = 0;
    while (pos < content.size()) {
        auto line_end = content.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = content.size();
        result += "  ";
        result += content.substr(pos, line_end - pos);
        result += "\n";
        pos = line_end + 1;
    }

    result += "── End thinking ──";
    result += "\033[0m\n"; // Reset

    return result;
}

// Estimate token count from thinking content
std::size_t estimate_thinking_tokens(const ThinkingBlock& block) {
    // Rough estimate: ~4 characters per token for English text
    return (block.content.size() + 3) / 4;
}

} // namespace cc::utils
