/// @file thinking_message.cppm
/// @brief Thinking process message rendering - displays the model's chain-of-thought
/// with streaming animation, collapsible sections, and duration tracking.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.thinking_message;

import cc.types.types;

export namespace cc::ui::messages::thinking_message {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// State of the thinking process
enum class ThinkingState : std::uint8_t {
    Active,     // Currently thinking (streaming)
    Paused,     // Paused/waiting for tool result
    Complete,   // Thinking finished
};

/// A section within the thinking content
struct ThinkingSection {
    std::string title;          // Optional section heading
    std::string content;        // The thinking text
    bool is_key_insight = false; // Highlight as important
};

/// Data for a thinking message
struct ThinkingMessageData {
    ThinkingState state;
    std::vector<ThinkingSection> sections;
    std::string raw_text;                   // Full raw thinking text
    std::chrono::milliseconds duration{0};  // Time spent thinking
    int token_count = 0;                    // Tokens used for thinking
    bool is_collapsed = true;               // Default collapsed
    int budget_remaining_pct = 100;         // Extended thinking budget
};

/// Options for the thinking message component
struct ThinkingMessageOptions {
    ThinkingMessageData data;
    int max_visible_lines = 15;
    bool show_token_count = true;
    bool animate_streaming = true;
    std::function<void()> on_toggle;
};

// ============================================================
// Rendering
// ============================================================

/// Spinner frames for active thinking
[[nodiscard]] inline std::string thinking_spinner(int frame) {
    static constexpr const char* frames[] = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"
    };
    return frames[((frame % 8) + 8) % 8];
}

/// Render the thinking message header
[[nodiscard]] inline Element RenderThinkingHeader(const ThinkingMessageData& data, int frame) {
    Elements parts;

    // State icon
    switch (data.state) {
        case ThinkingState::Active:
            parts.push_back(text(thinking_spinner(frame) + " ") | color(Color::Cyan));
            parts.push_back(text("Thinking") | color(Color::Cyan) | bold);
            parts.push_back(text("...") | color(Color::Cyan) | dim | blink);
            break;
        case ThinkingState::Paused:
            parts.push_back(text("⏸ ") | color(Color::Yellow));
            parts.push_back(text("Thinking (paused)") | color(Color::Yellow));
            break;
        case ThinkingState::Complete:
            parts.push_back(text("💭 ") | dim);
            parts.push_back(text("Thought") | color(Color::GrayLight));
            break;
    }

    // Duration
    if (data.duration.count() > 0) {
        std::string dur;
        if (data.duration.count() >= 1000) {
            dur = std::format(" ({:.1f}s)", data.duration.count() / 1000.0);
        } else {
            dur = std::format(" ({}ms)", data.duration.count());
        }
        parts.push_back(text(dur) | dim);
    }

    // Token count
    if (data.token_count > 0) {
        parts.push_back(filler());
        parts.push_back(text(std::format("{} tokens", data.token_count))
                        | color(Color::GrayDark) | dim);
    }

    return hbox(parts);
}

/// Render the thinking content body
[[nodiscard]] inline Element RenderThinkingBody(
    const ThinkingMessageData& data, int max_lines) {

    if (data.is_collapsed) {
        // Show summary only
        std::string summary;
        if (!data.raw_text.empty()) {
            summary = data.raw_text.substr(0, 80);
            if (data.raw_text.size() > 80) summary += "...";
        } else {
            summary = "(thinking content hidden)";
        }
        return hbox({
            text("  ▸ ") | color(Color::GrayDark),
            text(summary) | dim | color(Color::GrayLight),
        });
    }

    // Expanded view
    Elements elements;

    if (!data.sections.empty()) {
        // Render structured sections
        for (const auto& section : data.sections) {
            if (!section.title.empty()) {
                auto title_el = text("  ■ " + section.title)
                    | bold | color(section.is_key_insight ? Color::Yellow : Color::White);
                elements.push_back(title_el);
            }

            // Split content into lines with limit
            int lines_shown = 0;
            size_t pos = 0;
            while (pos < section.content.size() && lines_shown < max_lines) {
                auto nl = section.content.find('\n', pos);
                std::string line;
                if (nl == std::string::npos) {
                    line = section.content.substr(pos);
                    pos = section.content.size();
                } else {
                    line = section.content.substr(pos, nl - pos);
                    pos = nl + 1;
                }
                auto line_el = text("    " + line) | dim;
                if (section.is_key_insight) {
                    line_el = line_el | color(Color::Yellow);
                }
                elements.push_back(line_el);
                ++lines_shown;
            }
            if (pos < section.content.size()) {
                elements.push_back(text("    ...") | dim);
            }
            elements.push_back(text(""));
        }
    } else if (!data.raw_text.empty()) {
        // Render raw text with line limit
        int lines_shown = 0;
        size_t pos = 0;
        while (pos < data.raw_text.size() && lines_shown < max_lines) {
            auto nl = data.raw_text.find('\n', pos);
            std::string line;
            if (nl == std::string::npos) {
                line = data.raw_text.substr(pos);
                pos = data.raw_text.size();
            } else {
                line = data.raw_text.substr(pos, nl - pos);
                pos = nl + 1;
            }
            elements.push_back(text("  " + line) | dim | color(Color::GrayLight));
            ++lines_shown;
        }
        if (pos < data.raw_text.size()) {
            int remaining = 0;
            while (pos < data.raw_text.size()) {
                if (data.raw_text[pos] == '\n') ++remaining;
                ++pos;
            }
            elements.push_back(
                text(std::format("  ... {} more lines", remaining)) | dim);
        }
    }

    return vbox(elements);
}

/// Render a complete thinking message
[[nodiscard]] inline Element RenderThinkingMessage(
    const ThinkingMessageOptions& opts, int frame = 0) {

    const auto& data = opts.data;

    auto header = RenderThinkingHeader(data, frame);
    auto body = RenderThinkingBody(data, opts.max_visible_lines);

    Elements elements = {header};

    // Budget indicator for extended thinking
    if (data.state == ThinkingState::Active && data.budget_remaining_pct < 100) {
        double progress = (100 - data.budget_remaining_pct) / 100.0;
        elements.push_back(hbox({
            text("  Budget: ") | dim,
            gauge(progress) | color(Color::Cyan) | flex,
            text(std::format(" {}%", data.budget_remaining_pct)) | dim,
        }));
    }

    elements.push_back(body);

    // Toggle hint
    if (data.state == ThinkingState::Complete) {
        auto hint_text = data.is_collapsed ? "▸ Expand" : "▾ Collapse";
        elements.push_back(text(std::string("  ") + hint_text)
                           | dim | color(Color::GrayDark));
    }

    return vbox(elements) | borderRounded | color(Color::GrayDark);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive thinking message component
[[nodiscard]] inline Component ThinkingMessage(ThinkingMessageOptions options) {
    struct State {
        ThinkingMessageOptions opts;
        int frame = 0;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(options);

    return Renderer([state] {
        // Advance animation frame if active
        if (state->opts.data.state == ThinkingState::Active) {
            state->frame++;
        }
        return RenderThinkingMessage(state->opts, state->frame);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::Return || event == Event::Character(' ')) {
            state->opts.data.is_collapsed = !state->opts.data.is_collapsed;
            if (state->opts.on_toggle) {
                state->opts.on_toggle();
            }
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::messages::thinking_message
