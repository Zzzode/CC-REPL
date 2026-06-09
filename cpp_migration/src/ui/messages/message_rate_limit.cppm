/// @file message_rate_limit.cppm
/// @brief Rate limit warning message rendering  (+ interactive component)
///
/// Mirrors RateLimitMessage.tsx (160 lines):
///   ⚠️ Rate limit reached (429)  ▸ 12s remaining   [Retry now] [Options]
///   model: claude-sonnet-4-20250514
module;

#include <functional>
#include <string>
#include <optional>
#include <chrono>
#include <format>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.message_rate_limit;

export namespace cc::ui::messages {

using namespace ftxui;

/// Rate limit info for display
struct RateLimitInfo {
    std::string model;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<double> tokens_remaining;
    std::optional<double> requests_remaining;
    bool is_hard_limit{false};
    bool is_overloaded{false};         ///< "server overloaded" vs plain 429
    std::string raw_text;              ///< Original error string (for verbosity)
};

/// ─── Stateless element (kept for non-interactive contexts) ────────────
[[nodiscard]] inline Element render_rate_limit_message(const RateLimitInfo& info) {
    std::vector<Element> elements;

    auto header = info.is_hard_limit
        ? text("Rate limit reached") | bold | color(Color::Red)
        : text("Approaching rate limit") | bold | color(Color::Yellow);
    elements.push_back(header);

    elements.push_back(text("  Model: " + info.model) | dim);

    if (info.retry_after) {
        elements.push_back(text(std::format("  Retry after: {}s", info.retry_after->count())) | dim);
    }
    if (info.tokens_remaining) {
        elements.push_back(text(std::format("  Tokens remaining: {:.0f}", *info.tokens_remaining)) | dim);
    }
    if (info.requests_remaining) {
        elements.push_back(text(std::format("  Requests remaining: {:.0f}", *info.requests_remaining)) | dim);
    }

    return vbox(elements);
}

// ─── Interactive component: Retry + Options buttons ────────────────────

class RateLimitMessageComponent : public ComponentBase {
  public:
    using OnRetryFn = std::function<void()>;
    using OnOptionsFn = std::function<void()>;

    explicit RateLimitMessageComponent(RateLimitInfo info,
                                       OnRetryFn on_retry = nullptr,
                                       OnOptionsFn on_options = nullptr)
        : info_(std::move(info)),
          on_retry_(std::move(on_retry)),
          on_options_(std::move(on_options)) {}

    Element Render() override {
        const Color accent = info_.is_hard_limit ? Color::Red : Color::Yellow;

        auto icon = hbox({
            text(info_.is_overloaded ? "🔥 " : "⚠️ "),
            text(" "),
            text(info_.is_hard_limit ? "Rate limit reached (429)"
                                     : "Approaching rate limit")
                | bold | color(accent),
        });

        auto meta = [&]() -> Elements {
            Elements m;
            if (!info_.model.empty()) {
                m.push_back(text("model: " + info_.model) | dim);
            }
            if (info_.retry_after) {
                auto secs = info_.retry_after->count();
                m.push_back(
                    text(std::format("retry-after: {}s", secs)) | color(accent) | dim);
            }
            if (info_.tokens_remaining) {
                m.push_back(text(std::format("tokens remaining: {:.0f}",
                                             *info_.tokens_remaining)) | dim);
            }
            if (info_.requests_remaining) {
                m.push_back(text(std::format("requests remaining: {:.0f}",
                                             *info_.requests_remaining)) | dim);
            }
            return m;
        }();

        // Action bar
        auto retry_btn = on_retry_
            ? button(" 🔁 Retry now ", [this] { if (on_retry_) on_retry_(); })
                  | color(accent)
            : text("") | nothing;
        auto options_btn = on_options_
            ? button(" ⚙ Options ", [this] { if (on_options_) on_options_(); })
                  | dim
            : text("") | nothing;
        auto actions = hbox({
            retry_btn, text("  "),
            options_btn,
        });

        Elements rows;
        rows.push_back(icon);
        for (auto& r : meta) {
            rows.push_back(hbox({ text("  · ") | dim, std::move(r) }));
        }
        rows.push_back(separatorEmpty());
        rows.push_back(std::move(actions));

        return vbox(std::move(rows))
             | borderStyled(BorderStyle::ROUNDED, accent)
             | padding(1);
    }

    bool OnEvent(Event event) override {
        if (event == Event::Character('r') || event == Event::Character('R')) {
            if (on_retry_) on_retry_();
            return true;
        }
        if (event == Event::Character('o') || event == Event::Character('O')) {
            if (on_options_) on_options_();
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    RateLimitInfo info_;
    OnRetryFn on_retry_;
    OnOptionsFn on_options_;
};

[[nodiscard]] inline Component MakeRateLimitMessage(
    RateLimitInfo info,
    RateLimitMessageComponent::OnRetryFn on_retry = nullptr,
    RateLimitMessageComponent::OnOptionsFn on_options = nullptr)
{
    return Make<RateLimitMessageComponent>(
        std::move(info), std::move(on_retry), std::move(on_options));
}

} // namespace cc::ui::messages
