// Shutdown message - displayed when the REPL is exiting
//
// Mirrors ShutdownMessage.tsx (131 lines):
//   ─────────── Session ended ───────────
//   ✓ Session complete · 12 turns · 24k tokens · $0.0312
//   [Resume session]   [Start new]
module;

#include <functional>
#include <string>
#include <optional>
#include <format>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.message_shutdown;

export namespace cc::ui::messages::shutdown {
using namespace ftxui;

/// Reason for shutdown
enum class ShutdownReason {
    UserExit,       // User typed /exit or Ctrl+D
    SignalReceived,  // SIGINT/SIGTERM
    Error,          // Fatal error
    SessionEnd,     // Session ended normally
    Timeout         // Idle timeout
};

/// Data for a shutdown message
struct ShutdownMessageData {
    ShutdownReason reason = ShutdownReason::UserExit;
    std::optional<std::string> detail;
    int total_tokens_used = 0;
    int total_turns = 0;
    double total_cost = 0.0;
};

/// ─── Stateless element renderer ────────────────────────────────────────
[[nodiscard]] inline Element render(const ShutdownMessageData& data) {
    Elements parts;

    // Reason-specific message
    std::string icon;
    std::string message;
    Color msg_color = Color::GrayLight;

    switch (data.reason) {
        case ShutdownReason::UserExit:
            icon = "👋";
            message = "Goodbye!";
            break;
        case ShutdownReason::SignalReceived:
            icon = "⚡";
            message = "Interrupted";
            msg_color = Color::Yellow;
            break;
        case ShutdownReason::Error:
            icon = "❌";
            message = "Session ended due to error";
            msg_color = Color::Red;
            break;
        case ShutdownReason::SessionEnd:
            icon = "✓";
            message = "Session complete";
            msg_color = Color::Green;
            break;
        case ShutdownReason::Timeout:
            icon = "⏰";
            message = "Session timed out";
            msg_color = Color::Yellow;
            break;
    }

    parts.push_back(text(icon + " " + message) | color(msg_color));

    if (data.detail && !data.detail->empty()) {
        parts.push_back(text("  " + *data.detail) | dim);
    }

    // Session stats
    if (data.total_turns > 0 || data.total_tokens_used > 0) {
        std::string stats;
        if (data.total_turns > 0) {
            stats += std::format("{} turns", data.total_turns);
        }
        if (data.total_tokens_used > 0) {
            if (!stats.empty()) stats += " | ";
            if (data.total_tokens_used >= 1000) {
                stats += std::format("{:.1f}k tokens", data.total_tokens_used / 1000.0);
            } else {
                stats += std::format("{} tokens", data.total_tokens_used);
            }
        }
        if (data.total_cost > 0.0) {
            stats += std::format(" | ${:.4f}", data.total_cost);
        }
        parts.push_back(text("  [" + stats + "]") | dim | color(Color::GrayDark));
    }

    return vbox(parts);
}

// ─── Interactive component with Resume / New buttons ───────────────────

class ShutdownMessageComponent : public ComponentBase {
  public:
    using OnResumeFn = std::function<void()>;
    using OnNewSessionFn = std::function<void()>;

    explicit ShutdownMessageComponent(ShutdownMessageData data,
                                      OnResumeFn on_resume = nullptr,
                                      OnNewSessionFn on_new = nullptr)
        : data_(std::move(data)),
          on_resume_(std::move(on_resume)),
          on_new_session_(std::move(on_new)) {}

    Element Render() override {
        // Grey separator line with centered title
        auto divider = hbox({
            separator() | color(Color::GrayDark),
            text("── Session ended ──") | center | dim,
            separator() | color(Color::GrayDark),
        });

        auto body = render(data_);

        // Action row
        Elements actions;
        if (on_resume_) {
            actions.push_back(button(" ↻ Resume ", [this] {
                if (on_resume_) on_resume_();
            }) | color(Color::Cyan));
            actions.push_back(text("  "));
        }
        if (on_new_session_) {
            actions.push_back(button(" ✨ New session ", [this] {
                if (on_new_session_) on_new_session_();
            }) | color(Color::Green));
        }
        auto action_row = actions.empty()
            ? Element{text("")}
            : Element{hbox(std::move(actions)) | center};

        Elements rows;
        rows.push_back(divider);
        rows.push_back(separatorEmpty());
        rows.push_back(body | center);
        rows.push_back(separatorEmpty());
        rows.push_back(action_row);
        rows.push_back(separator() | color(Color::GrayDark));

        return vbox(std::move(rows)) | center;
    }

    bool OnEvent(Event event) override {
        if (event == Event::Character('r') || event == Event::Character('R')) {
            if (on_resume_) { on_resume_(); return true; }
        }
        if (event == Event::Character('n') || event == Event::Character('N')) {
            if (on_new_session_) { on_new_session_(); return true; }
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    ShutdownMessageData data_;
    OnResumeFn on_resume_;
    OnNewSessionFn on_new_session_;
};

[[nodiscard]] inline Component MakeShutdownMessage(
    ShutdownMessageData data,
    ShutdownMessageComponent::OnResumeFn on_resume = nullptr,
    ShutdownMessageComponent::OnNewSessionFn on_new = nullptr)
{
    return Make<ShutdownMessageComponent>(
        std::move(data), std::move(on_resume), std::move(on_new));
}

} // namespace cc::ui::messages::shutdown
