/// @file assistant_text_message.cppm
/// @brief FTXUI component for assistant text messages (AssistantTextMessage.tsx)
///
/// Visual layout:
///   [🤖 assistant  │ model X │ ⏱ HH:MM]
///   ┌──────────────────────────────────┐
///   │   (content, auto-wrapped)         │
///   │   Markdown-rendered body          │
///   └──────────────────────────────────┘
///   [▸ Copy  │  ▸ Regenerate]
///
/// Interactions:
///   - Click -> toggle verbose / raw view
///   - Copy button -> callback with raw text
///   - Rate-limit text routed to rate_limit component externally
// ────────────────────────────────────────────────────────────────────────
module;

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.assistant_text_message;

import cc.ui.messages.message_components;
import cc.ui.messages.message_timestamp;
import cc.ui.markdown;

export namespace cc::ui::messages {

using namespace ftxui;

// ─── Input data ────────────────────────────────────────────────────────

enum class AssistantMessageKind {
    Normal,
    RateLimit,       // handled by sibling component, but flag preserved
    ApiError,        // routed to error_message by dispatcher
    Empty,           // render only a dot indicator
};

struct AssistantTextMessageData {
    std::string content;
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();
    std::optional<std::string> model_name;
    AssistantMessageKind kind = AssistantMessageKind::Normal;
    bool verbose = false;          // show raw text
    bool show_dot = false;         // for empty streaming start
    bool is_streaming = false;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
};

// ─── Component ─────────────────────────────────────────────────────────

class AssistantTextMessageComponent : public ComponentBase {
  public:
    using OnCopyFn = std::function<void(std::string_view)>;
    using OnRegenerateFn = std::function<void()>;
    using OnRateLimitFn = std::function<void()>;

    explicit AssistantTextMessageComponent(AssistantTextMessageData data,
                                           OnCopyFn on_copy = nullptr,
                                           OnRegenerateFn on_regen = nullptr,
                                           OnRateLimitFn on_rl = nullptr)
        : data_(std::move(data)),
          on_copy_(std::move(on_copy)),
          on_regen_(std::move(on_regen)),
          on_rate_limit_(std::move(on_rl))
    {
        if (on_copy_) {
            copy_btn_ = Button("Copy", [this] {
                if (on_copy_) on_copy_(data_.content);
            }) | size(WIDTH, EQUAL, 10);
            Add(copy_btn_);
        }
        if (on_regen_) {
            regen_btn_ = Button("Regenerate", [this] {
                if (on_regen_) on_regen_();
            }) | size(WIDTH, EQUAL, 14);
            Add(regen_btn_);
        }
        if (on_rl) {
            rl_btn_ = Button("RateLimit options", [this] {
                if (on_rate_limit_) on_rate_limit_();
            }) | color(Color::Yellow);
            Add(rl_btn_);
        }
    }

    Element Render() override {
        // Header row: role + optional model + timestamp + streaming cursor
        Elements header;
        header.push_back(text("🤖 ") | color(Color::Purple4));
        header.push_back(text("Assistant") | bold | color(Color::Purple4));

        if (data_.model_name) {
            header.push_back(text("  ") | dim);
            header.push_back(text("(" + *data_.model_name + ")") | dim);
        }

        header.push_back(text("   ") | nothing);
        header.push_back(text(render_timestamp(data_.timestamp))
                             | dim | color(Color::GrayDark));

        if (data_.is_streaming) {
            header.push_back(text(" ▍") | blink | color(Color::Cyan));
        }

        auto header_line = hbox(std::move(header));

        // Empty message -> single dot
        if (data_.kind == AssistantMessageKind::Empty ||
            (data_.content.empty() && data_.show_dot)) {
            return vbox({
                std::move(header_line),
                text("●") | dim | color(Color::GrayDark),
            });
        }

        // Body (lines)
        Elements body = BuildBody();

        // Token footer
        Elements footer;
        if (data_.input_tokens > 0 || data_.output_tokens > 0) {
            footer.push_back(hbox({
                text("🔢 ") | dim,
                text(std::to_string(data_.input_tokens) + " in / "
                     + std::to_string(data_.output_tokens) + " out") | dim,
            }));
        }

        // Action row (clickable)
        Elements actions;
        if (copy_btn_) {
            actions.push_back(copy_btn_->Render());
            actions.push_back(text(" "));
        }
        if (regen_btn_) {
            actions.push_back(regen_btn_->Render());
            actions.push_back(text(" "));
        }
        if (data_.kind == AssistantMessageKind::RateLimit && rl_btn_) {
            actions.push_back(rl_btn_->Render());
        }
        auto action_row = hbox(std::move(actions)) | dim;

        return vbox({
            std::move(header_line),
            separator() | dim,
            vbox(std::move(body)) | indent(2),
            separatorEmpty(),
            footer.empty() ? text("") : vbox(std::move(footer)),
            expanded_ || verbose_override_ ? separatorEmpty() : text(""),
            action_row,
        }) | focusPosition(0, 0);
    }

    bool OnEvent(Event event) override {
        if (event == Event::Character('v') || event == Event::Character('V')) {
            verbose_override_ = !verbose_override_;
            return true;
        }
        if (event == Event::Character('e') || event == Event::Character('E')) {
            expanded_ = !expanded_;
            return true;
        }
        // Ctrl+C -> copy content
        if (event == Event::Special({3})) {
            if (on_copy_) on_copy_(data_.content);
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    auto BuildBody() const -> Elements {
        Elements out;
        const bool raw = verbose_override_ || data_.verbose;
        constexpr std::size_t kPreviewLines = 20;

        if (!raw) {
            std::string content = data_.content;
            bool truncated = false;
            if (!expanded_) {
                auto lines = SplitLines(data_.content);
                if (lines.size() > kPreviewLines) {
                    content.clear();
                    for (std::size_t i = 0; i < kPreviewLines; ++i) {
                        if (i > 0) content.push_back('\n');
                        content += lines[i];
                    }
                    truncated = true;
                }
            }

            out.push_back(::cc::ui::render_markdown(content));
            if (truncated) {
                out.push_back(text("... (press E to expand)")
                                  | dim | color(Color::GrayDark));
            }
            if (out.empty()) out.push_back(text(""));
            return out;
        }

        auto lines = SplitLines(data_.content);

        std::size_t rendered = 0;
        for (auto& line : lines) {
            if (!expanded_ && !raw && rendered >= kPreviewLines) {
                out.push_back(text("... (press E to expand)")
                                  | dim | color(Color::GrayDark));
                break;
            }
            // Very minimal heuristic: lines starting with "    " or "```"
            // are treated as code (monospace-like visual via dim + italic).
            Decorator dec = nothing;
            if (line.starts_with("```") || line.starts_with("    ")) {
                dec = color(Color::CyanLight) | dim;
            }
            out.push_back(text(line) | dec);
            ++rendered;
        }
        if (out.empty()) out.push_back(text(""));
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

    AssistantTextMessageData data_;
    OnCopyFn on_copy_;
    OnRegenerateFn on_regen_;
    OnRateLimitFn on_rate_limit_;
    bool verbose_override_ = false;
    bool expanded_ = true;
    Component copy_btn_;
    Component regen_btn_;
    Component rl_btn_;
};

// ─── Factories ─────────────────────────────────────────────────────────

[[nodiscard]] inline Component MakeAssistantTextMessage(
    AssistantTextMessageData data,
    AssistantTextMessageComponent::OnCopyFn on_copy = nullptr,
    AssistantTextMessageComponent::OnRegenerateFn on_regen = nullptr,
    AssistantTextMessageComponent::OnRateLimitFn on_rl = nullptr)
{
    return Make<AssistantTextMessageComponent>(
        std::move(data), std::move(on_copy), std::move(on_regen), std::move(on_rl));
}

/// Stateless element renderer (no interactions).
[[nodiscard]] inline Element RenderAssistantTextMessageBubble(const AssistantTextMessageData& data) {
    return vbox({
        hbox({
            text("🤖") | color(Color::Purple4),
            text(" Assistant  "),
            text(render_timestamp(data.timestamp)) | dim,
        }),
        ::cc::ui::render_markdown(data.content),
    });
}

}  // namespace cc::ui::messages
