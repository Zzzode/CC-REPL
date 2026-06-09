/// @file user_text_message.cppm
/// @brief FTXUI component for user text messages (UserTextMessage.tsx)
///
/// Visual layout:
///   ┌── Right-aligned (user side)
///   │  ┌──────────────────────────────┐
///   │  │ [content, selectable]   ⏱ HH:MM │
///   │  │ [quoted reply if any]     👤 You│
///   │  └──────────────────────────────┘
///   └──
///
/// Interactions:
///   - Click on message -> toggle selection highlight
///   - Ctrl+C / copy button -> copy content to clipboard (placeholder callback)
// ────────────────────────────────────────────────────────────────────────
module;

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.user_text_message;

import cc.ui.messages.message_timestamp;

export namespace cc::ui::messages {

using namespace ftxui;

// ─── Input data ────────────────────────────────────────────────────────

/// Plain data input, no globals.
struct UserTextMessageData {
    std::string content;
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();
    std::optional<std::string> quoted_reply;   ///< Text being replied to (>)
    bool is_transcript_mode = false;           ///< Dimmed presentation
    std::optional<std::string> command_name;   ///< "/commit" etc. shown as chip
};

// ─── Component ─────────────────────────────────────────────────────────

class UserTextMessageComponent : public ComponentBase {
  public:
    using OnCopyFn = std::function<void(std::string_view)>;
    using OnClickFn = std::function<void()>;

    explicit UserTextMessageComponent(UserTextMessageData data,
                                      OnCopyFn on_copy = nullptr,
                                      OnClickFn on_click = nullptr)
        : data_(std::move(data)),
          on_copy_(std::move(on_copy)),
          on_click_(std::move(on_click)) {}

    Element Render() override {
        auto bubble_body = BuildContent();

        // Right-side header: timestamp + role tag
        auto header_right = hbox({
            text(" ") | size(WIDTH, EQUAL, 1),
            text(render_timestamp(data_.timestamp)) | dim | color(Color::GrayDark),
            text(" 👤 You") | color(Color::Blue),
        });

        // User message is right-aligned. We indent from the left so that
        // the content visually hugs the right edge.
        return vbox({
            hbox({
                filler(),
                header_right,
            }),
            hbox({
                filler(),
                vbox(std::move(bubble_body))
                    | (hovered_ ? bgcolor(Color::DarkBlue) : nothing)
                    | flex_shrink,
            }),
        });
    }

    bool OnEvent(Event event) override {
        if (event == Event::Return) {
            hovered_ = !hovered_;
            if (on_click_) on_click_();
            return true;
        }
        // Custom copy accelerator
        if (event == Event::Special({3})) {  // Ctrl+C convention
            if (on_copy_) on_copy_(data_.content);
            return true;
        }
        if (event.is_mouse() && event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Released) {
            hovered_ = !hovered_;
            if (on_click_) on_click_();
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    auto BuildContent() -> Elements {
        Elements out;

        // Quoted reply block (indented, dimmed)
        if (data_.quoted_reply && !data_.quoted_reply->empty()) {
            auto lines = SplitLines(*data_.quoted_reply);
            for (auto& line : lines) {
                out.push_back(hbox({
                    text("│ ") | color(Color::GrayDark),
                    text(std::move(line)) | dim,
                }));
            }
            out.push_back(separator() | dim);
        }

        // Command chip
        if (data_.command_name) {
            out.push_back(hbox({
                text(" $ ") | bold | color(Color::Green),
                text(*data_.command_name) | bold,
            }));
        }

        // Main body
        auto lines = SplitLines(data_.content);
        for (auto& line : lines) {
            out.push_back(text(std::move(line))
                              | color(Color::White)
                              | (data_.is_transcript_mode ? dim : nothing));
        }

        return out;
    }

    static auto SplitLines(std::string_view text) -> std::vector<std::string> {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < text.size()) {
            auto nl = text.find('\n', start);
            if (nl == std::string_view::npos) {
                lines.emplace_back(text.substr(start));
                break;
            }
            lines.emplace_back(text.substr(start, nl - start));
            start = nl + 1;
        }
        if (lines.empty()) lines.emplace_back("");
        return lines;
    }

    UserTextMessageData data_;
    OnCopyFn on_copy_;
    OnClickFn on_click_;
    bool hovered_ = false;
};

/// Factory function.
[[nodiscard]] inline Component MakeUserTextMessage(UserTextMessageData data,
                                                   UserTextMessageComponent::OnCopyFn on_copy = nullptr,
                                                   UserTextMessageComponent::OnClickFn on_click = nullptr) {
    return Make<UserTextMessageComponent>(std::move(data), std::move(on_copy), std::move(on_click));
}

/// Convenience stateless element renderer (for transcript / no-interaction).
[[nodiscard]] inline Element RenderUserTextMessageBubble(const UserTextMessageData& data) {
    Elements body;
    body.push_back(text(data.content));
    return vbox({
        hbox({
            filler(),
            text(render_timestamp(data.timestamp)) | dim,
            text(" 👤") | color(Color::Blue),
        }),
        hbox({ filler(), vbox(std::move(body)) }),
    });
}

}  // namespace cc::ui::messages
