/// @file message_advisor.cppm
/// @brief Advisor/suggestion message rendering  (+ dismiss button)
///
/// Mirrors AdvisorMessage.tsx (157 lines):
///   ┌ 💡 Tip ───────────────────────────────────────── [✕] ┐
///   │ You can enable auto-save in settings → /config        │
///   │   → Configure now                                     │
///   └───────────────────────────────────────────────────────┘
module;

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.message_advisor;

import cc.ui.messages.message_components;

export namespace cc::ui::messages {

using namespace ftxui;

/// Advisor message severity
enum class AdvisorSeverity {
    Info,
    Suggestion,
    Warning,
    Error,
};

/// Advisor message entry
struct AdvisorMessage {
    std::string content;
    AdvisorSeverity severity{AdvisorSeverity::Info};
    std::optional<std::string> action_label;
    std::optional<std::string> source;
};

/// ─── Stateless element renderer ────────────────────────────────────────
[[nodiscard]] inline Element render_advisor_message(const AdvisorMessage& msg) {
    auto [prefix, prefix_color] = [&]() -> std::pair<std::string, Color> {
        switch (msg.severity) {
            case AdvisorSeverity::Info: return {"ℹ", Color::Cyan};
            case AdvisorSeverity::Suggestion: return {"💡", Color::Green};
            case AdvisorSeverity::Warning: return {"⚠", Color::Yellow};
            case AdvisorSeverity::Error: return {"✗", Color::Red};
        }
        return {"?", Color::White};
    }();

    std::vector<Element> elements;
    elements.push_back(hbox({
        text(prefix) | color(prefix_color),
        text(" " + msg.content),
    }));

    if (msg.action_label) {
        elements.push_back(text("  → " + *msg.action_label) | dim);
    }

    return vbox(elements);
}

// ─── Severity → palette helper ─────────────────────────────────────────

inline auto SeverityPalette(AdvisorSeverity s) -> std::tuple<std::string_view, std::string_view, Color> {
    switch (s) {
        case AdvisorSeverity::Info:       return {"ℹ",  "Info",     Color::Cyan};
        case AdvisorSeverity::Suggestion: return {"💡", "Tip",      Color::Green};
        case AdvisorSeverity::Warning:    return {"⚠️", "Warning",  Color::Yellow};
        case AdvisorSeverity::Error:      return {"✖",  "Error",    Color::Red};
    }
    return {"ℹ", "Info", Color::Cyan};
}

// ─── Interactive component: pill + dismiss + optional action ───────────

class AdvisorMessageComponent : public ComponentBase {
  public:
    using OnDismissFn = std::function<void()>;
    using OnActionFn  = std::function<void()>;

    explicit AdvisorMessageComponent(AdvisorMessage msg,
                                     OnDismissFn on_dismiss = nullptr,
                                     OnActionFn  on_action  = nullptr)
        : msg_(std::move(msg)),
          on_dismiss_(std::move(on_dismiss)),
          on_action_(std::move(on_action)),
          dismissed_(false)
    {
        if (on_dismiss_) {
            dismiss_btn_ = Button(" ✕ ", [this] {
                dismissed_ = true;
                if (on_dismiss_) on_dismiss_();
            }) | dim;
            Add(dismiss_btn_);
        }
        if (on_action_ && msg_.action_label) {
            auto accent = std::get<2>(SeverityPalette(msg_.severity));
            action_btn_ = Button(" → " + *msg_.action_label + " ", [this] {
                if (on_action_) on_action_();
            }) | color(accent);
            Add(action_btn_);
        }
    }

    Element OnRender() override {
        if (dismissed_) return text("") | size(HEIGHT, EQUAL, 0);

        auto [icon, title, accent] = SeverityPalette(msg_.severity);

        Element dismiss_el = on_dismiss_ && dismiss_btn_
                                 ? dismiss_btn_->Render()
                                 : text("");
        auto header = hbox({
            text(std::string(icon) + " ") | color(accent),
            text(std::string(title) + "  ") | bold | color(accent),
            text(msg_.content) | flex,
            filler(),
            dismiss_el,
        });

        Elements extras;
        if (msg_.source) {
            extras.push_back(text("  source: " + *msg_.source)
                                 | dim | color(Color::GrayDark));
        }
        if (msg_.action_label) {
            Element act_el = action_btn_
                ? action_btn_->Render()
                : text(" → " + *msg_.action_label) | dim | color(accent);
            extras.push_back(hbox({ text("  "), act_el }));
        }

        Elements rows;
        rows.push_back(header);
        for (auto& e : extras) rows.push_back(std::move(e));

        // Light-colored background to make the tip "pop"
        Color bg = Color::Blue;
        switch (msg_.severity) {
            case AdvisorSeverity::Error:   bg = Color(Color::DarkRed); break;
            case AdvisorSeverity::Warning: bg = Color(Color::Yellow);  break;
            default: break;
        }
        (void)bg;
        return vbox(std::move(rows))
             | borderStyled(BorderStyle::ROUNDED, accent)
             | padding(1);
    }

    bool OnEvent(Event event) override {
        if (event == Event::Character('d') || event == Event::Character('D') ||
            event == Event::Escape) {
            dismissed_ = true;
            if (on_dismiss_) on_dismiss_();
            return true;
        }
        if (event == Event::Character('a') || event == Event::Character('A')) {
            if (on_action_) on_action_();
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    AdvisorMessage msg_;
    OnDismissFn on_dismiss_;
    OnActionFn  on_action_;
    bool dismissed_;
    Component dismiss_btn_;
    Component action_btn_;
};

[[nodiscard]] inline Component MakeAdvisorMessage(
    AdvisorMessage msg,
    AdvisorMessageComponent::OnDismissFn on_dismiss = nullptr,
    AdvisorMessageComponent::OnActionFn  on_action  = nullptr)
{
    return Make<AdvisorMessageComponent>(
        std::move(msg), std::move(on_dismiss), std::move(on_action));
}

} // namespace cc::ui::messages
