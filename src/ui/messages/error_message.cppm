module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>

export module cc.ui.error_message;

export namespace cc::ui::messages {

// ─── Error types ─────────────────────────────────────────────────────

enum class ErrorSeverity {
    Info,
    Warning,
    Error,
    Fatal
};

struct ErrorContext {
    std::string file;
    int line;
    std::optional<std::string> function_name;
    std::optional<std::string> stack_trace;
};

struct ErrorMessageData {
    std::string message;
    ErrorSeverity severity;
    std::optional<ErrorContext> context;
    std::optional<std::string> suggestion;
    std::optional<std::string> doc_url;
};

// ─── System message types ────────────────────────────────────────────

enum class SystemMessageType {
    Info,
    Warning,
    RateLimit,
    ModelSwitch,
    SessionStart,
    SessionEnd
};

struct SystemMessageData {
    SystemMessageType type;
    std::string content;
    std::optional<std::string> details;
};

// ─── Helper functions ────────────────────────────────────────────

inline std::string_view get_severity_icon(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::Info:    return "i";
        case ErrorSeverity::Warning: return "!";
        case ErrorSeverity::Error:   return "x";
        case ErrorSeverity::Fatal:   return "X";
    }
    return "?";
}

inline std::string get_severity_color(ErrorSeverity) {
    return "";
}

inline std::string format_stack_trace(std::string_view trace) {
    return "  " + std::string(trace);
}

// ─── Rendering functions ─────────────────────────────────────────────

/// Render an error message as a plain string.
///
/// @param data       — error message payload (message, severity, context, etc.)
/// @param on_retry   — optional retry callback; when set, a "[r] Retry" hint
///                     is appended so the user knows the action is available.
///                     TS REF: src/components/messages/SystemAPIErrorMessage.tsx
///                     — the Retry pill button (L256-259 in TS).
/// @param on_clear   — optional clear-session callback; when set alongside a
///                     session-expired / auth error, a "[c] Clear session" hint
///                     is appended.  TS REF: SystemAPIErrorMessage.tsx L260-264
///                     (onClearSession prop rendered as "Clear session" pill).
inline std::string render_error_message(
    const ErrorMessageData& data,
    std::optional<std::function<void()>> on_retry = std::nullopt,
    std::optional<std::function<void()>> on_clear = std::nullopt)
{
    std::string result;
    result += get_severity_icon(data.severity);
    result += " ";
    result += get_severity_color(data.severity);
    result += data.message;
    if (data.context.has_value()) {
        const auto& ctx = *data.context;
        result += "\n  at " + ctx.file + ":" + std::to_string(ctx.line);
        if (ctx.function_name.has_value()) {
            result += " in " + *ctx.function_name;
        }
        if (ctx.stack_trace.has_value()) {
            result += "\n" + format_stack_trace(*ctx.stack_trace);
        }
    }
    if (data.suggestion.has_value()) {
        result += "\n💡 " + *data.suggestion;
    }
    if (data.doc_url.has_value()) {
        result += "\n📖 " + *data.doc_url;
    }
    // Retry / clear-session action hints (TS REF: SystemAPIErrorMessage.tsx
    // action button row — Retry / Clear session / Diagnose / Dismiss).
    // The interactive FTXUI card (api_error_message.cppm) renders full pill
    // buttons; this plain-string fallback appends keyboard hints so the
    // message_row.cppm fallback path can still surface retry affordance.
    if (on_retry.has_value()) {
        result += "\n[r] Retry";
    }
    if (on_clear.has_value()) {
        result += "\n[c] Clear session";
    }
    return result;
}

/// Render a system message as a plain string.
///
/// @param data       — system message payload (type, content, details)
/// @param on_retry   — optional retry callback; when set, a "[r] Retry" hint
///                     is appended.  Useful for rate-limit / transient system
///                     messages where retrying the last action makes sense.
///                     TS REF: SystemAPIErrorMessage.tsx Retry action.
inline std::string render_system_message(
    const SystemMessageData& data,
    std::optional<std::function<void()>> on_retry = std::nullopt)
{
    std::string result = "-- ";
    switch (data.type) {
        case SystemMessageType::Info:         result += "i "; break;
        case SystemMessageType::Warning:      result += "! "; break;
        case SystemMessageType::RateLimit:    result += "T "; break;
        case SystemMessageType::ModelSwitch:  result += "M "; break;
        case SystemMessageType::SessionStart: result += "> "; break;
        case SystemMessageType::SessionEnd:   result += "| "; break;
    }
    result += data.content;
    if (data.details.has_value()) {
        result += "\n   " + *data.details;
    }
    if (on_retry.has_value()) {
        result += "\n[r] Retry";
    }
    result += " --";
    return result;
}

inline std::string render_rate_limit_message(int retry_after_seconds) {
    std::string result = "🕐 Rate limited. ";
    if (retry_after_seconds > 0) {
        result += "Retrying in " + std::to_string(retry_after_seconds) + "s...";
    } else {
        result += "Please wait before retrying.";
    }
    return result;
}

} // namespace cc::ui::messages
