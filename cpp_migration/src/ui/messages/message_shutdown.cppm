// Shutdown message - displayed when the REPL is exiting
module;

#include <string>
#include <optional>
#include <format>

#include <ftxui/dom/elements.hpp>

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

/// Render the shutdown message
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

} // namespace cc::ui::messages::shutdown
