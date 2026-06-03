module;
#include <filesystem>
#include <string>
#include <string_view>
#include <optional>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <typeinfo>
#include <cerrno>

export module cc.utils.errors_utils;

export namespace cc::utils {


enum class ErrorSeverity {
    Info,
    Warning,
    Error,
    Fatal
};


enum class ErrorCategory {
    Network,
    FileSystem,
    Permission,
    Parse,
    Api,
    Internal
};


struct ClassifiedError {
    ErrorSeverity severity;
    ErrorCategory category;
    std::string message;
    std::string code;
    std::optional<std::string> suggestion;
};


[[nodiscard]] inline std::string_view severity_to_string(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::Info: return "info";
        case ErrorSeverity::Warning: return "warning";
        case ErrorSeverity::Error: return "error";
        case ErrorSeverity::Fatal: return "fatal";
    }
    return "unknown";
}


[[nodiscard]] inline std::string_view category_to_string(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::Network: return "network";
        case ErrorCategory::FileSystem: return "filesystem";
        case ErrorCategory::Permission: return "permission";
        case ErrorCategory::Parse: return "parse";
        case ErrorCategory::Api: return "api";
        case ErrorCategory::Internal: return "internal";
    }
    return "unknown";
}


[[nodiscard]] inline ClassifiedError classify_error(std::exception_ptr ep) {
    ClassifiedError result{
        .severity = ErrorSeverity::Error,
        .category = ErrorCategory::Internal,
        .message = "Unknown error",
        .code = "INTERNAL_ERROR",
        .suggestion = std::nullopt
    };

    if (!ep) return result;

    try {
        std::rethrow_exception(ep);
    } catch (const std::filesystem::filesystem_error& e) {
        result.category = ErrorCategory::FileSystem;
        result.message = e.what();
        result.code = "FS_ERROR";
        auto ec = e.code();
        if (ec == std::errc::permission_denied) {
            result.category = ErrorCategory::Permission;
            result.code = "PERMISSION_DENIED";
            result.suggestion = "Check file permissions or run with elevated privileges";
        } else if (ec == std::errc::no_such_file_or_directory) {
            result.code = "FILE_NOT_FOUND";
            result.suggestion = "Verify the file path exists";
        } else if (ec == std::errc::no_space_on_device) {
            result.code = "DISK_FULL";
            result.suggestion = "Free up disk space";
        }
    } catch (const std::system_error& e) {
        auto ec = e.code();
        result.message = e.what();

        if (ec.category() == std::generic_category()) {
            switch (static_cast<std::errc>(ec.value())) {
                case std::errc::connection_refused:
                case std::errc::connection_reset:
                case std::errc::network_unreachable:
                case std::errc::host_unreachable:
                case std::errc::timed_out:
                    result.category = ErrorCategory::Network;
                    result.code = "NETWORK_ERROR";
                    result.suggestion = "Check network connectivity and try again";
                    break;
                case std::errc::permission_denied:
                    result.category = ErrorCategory::Permission;
                    result.code = "PERMISSION_DENIED";
                    result.suggestion = "Check permissions";
                    break;
                default:
                    result.code = "SYSTEM_ERROR";
                    break;
            }
        }
    } catch (const std::invalid_argument& e) {
        result.category = ErrorCategory::Parse;
        result.message = e.what();
        result.code = "PARSE_ERROR";
        result.suggestion = "Check input format";
    } catch (const std::runtime_error& e) {
        result.message = e.what();

        std::string_view msg(e.what());
        if (msg.find("API") != std::string_view::npos ||
            msg.find("api") != std::string_view::npos ||
            msg.find("401") != std::string_view::npos ||
            msg.find("429") != std::string_view::npos) {
            result.category = ErrorCategory::Api;
            result.code = "API_ERROR";
            if (msg.find("429") != std::string_view::npos) {
                result.suggestion = "Rate limited. Wait and retry";
            } else if (msg.find("401") != std::string_view::npos) {
                result.suggestion = "Check your API key";
            }
        } else if (msg.find("parse") != std::string_view::npos ||
                   msg.find("JSON") != std::string_view::npos) {
            result.category = ErrorCategory::Parse;
            result.code = "PARSE_ERROR";
        }
    } catch (const std::exception& e) {
        result.message = e.what();
    } catch (...) {
        result.message = "Unknown non-standard exception";
        result.severity = ErrorSeverity::Fatal;
    }

    return result;
}


[[nodiscard]] inline std::string format_error(const ClassifiedError& error) {
    std::string result;
    result += "[";
    result += severity_to_string(error.severity);
    result += "] [";
    result += category_to_string(error.category);
    result += "] ";
    result += error.code;
    result += ": ";
    result += error.message;
    if (error.suggestion.has_value()) {
        result += "\n  → ";
        result += error.suggestion.value();
    }
    return result;
}


[[nodiscard]] inline bool is_retryable(const ClassifiedError& error) {

    if (error.category == ErrorCategory::Network) return true;
    if (error.category == ErrorCategory::Api) {

        return error.code == "API_ERROR" &&
               (error.message.find("429") != std::string::npos ||
                error.message.find("500") != std::string::npos ||
                error.message.find("502") != std::string::npos ||
                error.message.find("503") != std::string::npos);
    }
    return false;
}

} // namespace cc::utils
