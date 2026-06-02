module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>

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

inline std::string render_error_message(const ErrorMessageData& data) {
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
    return result;
}

inline std::string render_system_message(const SystemMessageData& data) {
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
