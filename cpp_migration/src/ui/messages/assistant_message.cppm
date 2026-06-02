module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <variant>
#include <chrono>
#include <cstdint>

export module cc.ui.assistant_message;

export namespace cc::ui::messages {

// ─── Content block types ─────────────────────────────────────────────

enum class ContentBlockType {
    Text,
    Code,
    ToolUse,
    ToolResult,
    Thinking,
    Image,
    Error
};

struct TextBlock {
    std::string content;
    bool is_markdown{true};
};

struct CodeBlock {
    std::string code;
    std::string language;
    std::optional<std::string> filename;
    bool highlighted{false};
};

struct ThinkingBlock {
    std::string content;
    std::chrono::milliseconds duration;
    bool collapsed{false};
};

using ContentBlock = std::variant<TextBlock, CodeBlock, ThinkingBlock>;

// ─── Message data ────────────────────────────────────────────────────

struct AssistantMessageData {
    std::string id;
    std::vector<ContentBlock> blocks;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> model;
    std::uint64_t input_tokens{0};
    std::uint64_t output_tokens{0};
    bool is_streaming{false};
};

struct MessageRenderConfig {
    bool show_tokens{true};
    bool show_model{false};
    bool collapse_thinking{false};
    std::size_t max_code_lines{100};
    bool syntax_highlight{true};
};

// ─── Helper functions ────────────────────────────────────────────

inline std::string truncate_code_block(std::string_view code, std::size_t max_lines) {
    std::size_t line_count = 0;
    std::size_t pos = 0;
    while (pos < code.size() && line_count < max_lines) {
        auto next = code.find('\n', pos);
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
        ++line_count;
    }
    if (line_count >= max_lines && pos < code.size()) {
        return std::string(code.substr(0, pos)) + "... (truncated)";
    }
    return std::string(code);
}

inline std::string render_streaming_indicator(bool active) {
    if (active) {
        return " |";
    }
    return "";
}

inline std::string format_token_count(std::uint64_t input, std::uint64_t output) {
    return "\n[tokens: " + std::to_string(input) + " in / "
         + std::to_string(output) + " out]";
}

inline std::string render_content_block(
    const ContentBlock& block,
    const MessageRenderConfig& config = {}
) {
    return std::visit([&](const auto& b) -> std::string {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, TextBlock>) {
            return b.content;
        } else if constexpr (std::is_same_v<T, CodeBlock>) {
            std::string rendered = "```" + b.language + "\n";
            rendered += truncate_code_block(b.code, config.max_code_lines);
            rendered += "\n```";
            return rendered;
        } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
            if (config.collapse_thinking || b.collapsed) {
                return "[thinking collapsed]";
            }
            return b.content;
        }
    }, block);
}

// ─── Rendering functions ─────────────────────────────────────────────

inline std::string render_assistant_message(
    const AssistantMessageData& data,
    const MessageRenderConfig& config = {}
) {
    std::string result;
    for (const auto& block : data.blocks) {
        result += render_content_block(block, config);
    }
    if (config.show_tokens && (data.input_tokens > 0 || data.output_tokens > 0)) {
        result += format_token_count(data.input_tokens, data.output_tokens);
    }
    if (data.is_streaming) {
        result += render_streaming_indicator(true);
    }
    return result;
}

} // namespace cc::ui::messages
