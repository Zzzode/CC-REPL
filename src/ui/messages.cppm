// C++23 Module: Message display components for conversation rendering
module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ftxui/dom/elements.hpp>

export module cc.ui.messages;

export namespace cc::ui {

// Timestamp type alias for message timing
using Timestamp = std::chrono::system_clock::time_point;

// Role of the message sender
enum class MessageRole { User, Assistant, System, Tool };

// Markdown inline element types for rich rendering
enum class InlineStyle { Plain, Bold, Italic, Code, Link, Strikethrough };

// Markdown block types
enum class BlockType { Paragraph, CodeBlock, Heading, List, Quote, HorizontalRule, Table };

// Inline text segment with styling
struct InlineSegment {
    std::string text{};
    InlineStyle style{InlineStyle::Plain};
    std::optional<std::string> link_url{};
};

// A block of markdown content
struct MarkdownBlock {
    BlockType type{BlockType::Paragraph};
    std::vector<InlineSegment> content{};
    std::string language{};       // For code blocks
    std::size_t heading_level{0}; // For headings (1-6)
    std::vector<std::vector<InlineSegment>> list_items{};
};

// Parse markdown text into structured blocks
[[nodiscard]] auto parse_markdown(std::string_view text) -> std::vector<MarkdownBlock> {
    std::vector<MarkdownBlock> blocks;
    std::string current_para;
    bool in_code_block = false;
    std::string code_lang;
    std::string code_content;

    auto flush_paragraph = [&]() {
        if (!current_para.empty()) {
            MarkdownBlock block{.type = BlockType::Paragraph};
            block.content.push_back({.text = current_para, .style = InlineStyle::Plain});
            blocks.push_back(std::move(block));
            current_para.clear();
        }
    };

    // Simple line-by-line parsing
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        auto line = text.substr(pos, end == std::string_view::npos ? end : end - pos);
        pos = (end == std::string_view::npos) ? text.size() : end + 1;

        // Code fence detection
        if (line.starts_with("```")) {
            if (!in_code_block) {
                flush_paragraph();
                in_code_block = true;
                code_lang = std::string(line.substr(3));
                code_content.clear();
            } else {
                MarkdownBlock block{.type = BlockType::CodeBlock};
                block.content.push_back({.text = code_content, .style = InlineStyle::Code});
                block.language = code_lang;
                blocks.push_back(std::move(block));
                in_code_block = false;
            }
            continue;
        }

        if (in_code_block) { code_content += std::string(line) + "\n"; continue; }

        // Heading detection
        if (line.starts_with('#')) {
            flush_paragraph();
            std::size_t level = 0;
            while (level < line.size() && line[level] == '#') level++;
            MarkdownBlock block{.type = BlockType::Heading, .heading_level = level};
            auto heading_text = line.substr(std::min(level + 1, line.size()));
            block.content.push_back({.text = std::string(heading_text), .style = InlineStyle::Bold});
            blocks.push_back(std::move(block));
            continue;
        }

        // Blockquote detection
        if (line.starts_with("> ")) {
            flush_paragraph();
            MarkdownBlock block{.type = BlockType::Quote};
            block.content.push_back({.text = std::string(line.substr(2)), .style = InlineStyle::Italic});
            blocks.push_back(std::move(block));
            continue;
        }

        // Horizontal rule
        if (line == "---" || line == "***" || line == "___") {
            flush_paragraph();
            blocks.push_back({.type = BlockType::HorizontalRule});
            continue;
        }

        // List item detection
        if (line.starts_with("- ") || line.starts_with("* ")) {
            flush_paragraph();
            MarkdownBlock block{.type = BlockType::List};
            block.list_items.push_back({{.text = std::string(line.substr(2)), .style = InlineStyle::Plain}});
            blocks.push_back(std::move(block));
            continue;
        }

        // Regular paragraph text
        if (line.empty()) { flush_paragraph(); }
        else { current_para += std::string(line) + " "; }
    }
    flush_paragraph();
    return blocks;
}

// Format timestamp for display
[[nodiscard]] auto format_timestamp(Timestamp ts) -> std::string {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    auto tm = *std::localtime(&time_t);
    return std::format("{:02d}:{:02d}", tm.tm_hour, tm.tm_min);
}

// UserMessageView: renders user messages with timestamp
struct UserMessageView {
    std::string content;
    Timestamp timestamp;
    std::optional<std::string> attached_file;

    [[nodiscard]] auto render() const -> ftxui::Element;
};

// AssistantMessageView: renders assistant responses with markdown
struct AssistantMessageView {
    std::string content;
    Timestamp timestamp;
    std::string model_name;
    std::optional<std::size_t> token_count;
    std::vector<MarkdownBlock> parsed_blocks;

    // Parse content into blocks for rendering
    auto parse() -> void { parsed_blocks = parse_markdown(content); }

    [[nodiscard]] auto render() const -> ftxui::Element;
};

// SystemMessageView: renders system messages (muted style)
struct SystemMessageView {
    std::string content;
    Timestamp timestamp;
    bool is_ephemeral{false}; // Auto-dismiss after short time

    [[nodiscard]] auto render() const -> ftxui::Element;
};

// Tool use result status
enum class ToolStatus { Running, Success, Error, Cancelled };

// ToolUseView: renders tool calls with expandable results
struct ToolUseView {
    std::string tool_name;
    std::string tool_input;       // JSON-formatted input
    std::string tool_output;      // Result text
    ToolStatus status{ToolStatus::Running};
    Timestamp started_at;
    std::optional<Timestamp> completed_at;
    bool expanded{false};         // Whether to show full result
    std::optional<std::size_t> duration_ms;

    auto toggle_expand() -> void { expanded = !expanded; }

    [[nodiscard]] auto render() const -> ftxui::Element;

    // Calculate duration display
    [[nodiscard]] auto duration_display() const -> std::string {
        if (!duration_ms) return "...";
        if (*duration_ms < 1000) return std::format("{}ms", *duration_ms);
        return std::format("{:.1f}s", static_cast<double>(*duration_ms) / 1000.0);
    }

    // Status indicator icon
    [[nodiscard]] auto status_icon() const -> std::string {
        switch (status) {
            case ToolStatus::Running:   return "⟳";
            case ToolStatus::Success:   return "✓";
            case ToolStatus::Error:     return "✗";
            case ToolStatus::Cancelled: return "⊘";
        }
        return "?";
    }
};

// ThinkingView: renders thinking blocks (collapsible, italic)
struct ThinkingView {
    std::string content;
    Timestamp timestamp;
    bool collapsed{true};
    std::size_t token_count{0};

    auto toggle_collapse() -> void { collapsed = !collapsed; }

    [[nodiscard]] auto render() const -> ftxui::Element;

    // Summary for collapsed view (first line or truncated)
    [[nodiscard]] auto summary() const -> std::string {
        auto first_newline = content.find('\n');
        auto first_line = content.substr(0, std::min(first_newline, std::size_t{80}));
        if (first_line.size() < content.size()) return std::string(first_line) + "...";
        return std::string(first_line);
    }
};

// ErrorView: renders error messages (red, with error code)
struct ErrorView {
    std::string message;
    std::string error_code;
    Timestamp timestamp;
    std::optional<std::string> suggestion; // Helpful suggestion text
    bool is_retryable{false};

    [[nodiscard]] auto render() const -> ftxui::Element;

    // Format error display with code prefix
    [[nodiscard]] auto formatted() const -> std::string {
        if (error_code.empty()) return message;
        return std::format("[{}] {}", error_code, message);
    }
};

// StreamingView: partial message with cursor animation
struct StreamingView {
    std::string partial_content;
    Timestamp started_at;
    std::size_t cursor_phase{0}; // Animation frame (0-3)
    bool is_active{true};

    auto advance_cursor() -> void { cursor_phase = (cursor_phase + 1) % 4; }

    [[nodiscard]] auto render() const -> ftxui::Element;

    // Cursor character for animation
    [[nodiscard]] auto cursor_char() const -> std::string {
        constexpr std::array<std::string_view, 4> frames = {"█", "▌", "▐", "▀"};
        return std::string(frames[cursor_phase]);
    }

    // Elapsed time since streaming started
    [[nodiscard]] auto elapsed_ms() const -> std::size_t {
        auto now = std::chrono::system_clock::now();
        return static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at).count());
    }
};

// Unified message variant for type-safe message handling
using MessageView = std::variant<
    UserMessageView,
    AssistantMessageView,
    SystemMessageView,
    ToolUseView,
    ThinkingView,
    ErrorView,
    StreamingView
>;

// Render a single message variant via visitor pattern
[[nodiscard]] auto render_message(const MessageView& msg) -> ftxui::Element;

// Render full conversation (all messages, scrolled to bottom)
[[nodiscard]] auto render_conversation(const std::vector<MessageView>& messages,
                                       std::size_t scroll_offset = 0,
                                       std::size_t visible_height = 0) -> ftxui::Element;

// Concept: any type that has a render() method returning Element
template<typename T>
concept Renderable = requires(const T& t) {
    { t.render() } -> std::same_as<ftxui::Element>;
};

// Render any Renderable message component
template<Renderable T>
[[nodiscard]] auto render_component(const T& component) -> ftxui::Element {
    return component.render();
}

// Filter messages by role
[[nodiscard]] auto filter_by_role(const std::vector<MessageView>& messages,
                                   MessageRole role) -> std::vector<const MessageView*> {
    std::vector<const MessageView*> result;
    for (const auto& msg : messages) {
        bool matches = std::visit([role](const auto& m) {
            if constexpr (std::is_same_v<std::decay_t<decltype(m)>, UserMessageView>)
                return role == MessageRole::User;
            else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, AssistantMessageView>)
                return role == MessageRole::Assistant;
            else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, SystemMessageView>)
                return role == MessageRole::System;
            else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ToolUseView>)
                return role == MessageRole::Tool;
            else return false;
        }, msg);
        if (matches) result.push_back(&msg);
    }
    return result;
}

} // namespace cc::ui
