// C++23 Error Infrastructure Module
// Provides unified error handling with std::expected
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <expected>
#include <format>
#include <utility>
#include <vector>

export module cc.utils.error;

export namespace cc::utils {

// 错误码枚举 - 覆盖常见错误场景
enum class ErrorCode : uint32_t {
    unknown = 0,
    io_error,
    network_error,
    parse_error,
    permission_denied,
    timeout,
    not_found,
    already_exists,
    invalid_argument,
    out_of_range,
    resource_exhausted,
    cancelled,
    internal_error,
    unimplemented,
    unavailable,
};

// 错误码转字符串
constexpr std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::unknown:            return "unknown";
        case ErrorCode::io_error:           return "io_error";
        case ErrorCode::network_error:      return "network_error";
        case ErrorCode::parse_error:        return "parse_error";
        case ErrorCode::permission_denied:  return "permission_denied";
        case ErrorCode::timeout:            return "timeout";
        case ErrorCode::not_found:          return "not_found";
        case ErrorCode::already_exists:     return "already_exists";
        case ErrorCode::invalid_argument:   return "invalid_argument";
        case ErrorCode::out_of_range:       return "out_of_range";
        case ErrorCode::resource_exhausted: return "resource_exhausted";
        case ErrorCode::cancelled:          return "cancelled";
        case ErrorCode::internal_error:     return "internal_error";
        case ErrorCode::unimplemented:      return "unimplemented";
        case ErrorCode::unavailable:        return "unavailable";
    }
    return "unknown";
}

// 核心错误类 - 支持错误链
class Error {
public:
    Error(ErrorCode code, std::string message)
        : code_(code)
        , message_(std::move(message))
        , cause_(nullptr) {}

    // 带 cause 链的构造
    Error(ErrorCode code, std::string message, Error cause)
        : code_(code)
        , message_(std::move(message))
        , cause_(std::make_unique<Error>(std::move(cause))) {}

    // 移动语义
    Error(Error&&) noexcept = default;
    Error& operator=(Error&&) noexcept = default;

    // 拷贝支持（深拷贝 cause 链）
    Error(const Error& other)
        : code_(other.code_)
        , message_(other.message_)
        , cause_(other.cause_ ? std::make_unique<Error>(*other.cause_) : nullptr) {}

    Error& operator=(const Error& other) {
        if (this != &other) {
            code_ = other.code_;
            message_ = other.message_;
            cause_ = other.cause_ ? std::make_unique<Error>(*other.cause_) : nullptr;
        }
        return *this;
    }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] bool has_cause() const noexcept { return cause_ != nullptr; }
    [[nodiscard]] const Error& cause() const noexcept { return *cause_; }

    // 格式化完整错误信息（含 cause 链）
    [[nodiscard]] std::string format() const {
        auto result = std::format("[{}] {}", error_code_name(code_), message_);

        // 递归展开 cause 链
        const Error* current = cause_.get();
        int depth = 1;
        while (current) {
            result += std::format("\n  caused by [{}]: {}",
                error_code_name(current->code_), current->message_);
            current = current->cause_.get();
            if (++depth > 10) { // 防止无限链
                result += "\n  ... (cause chain truncated)";
                break;
            }
        }
        return result;
    }

    // 追加 cause 到已有错误
    [[nodiscard]] Error with_cause(Error cause) const& {
        Error copy = *this;
        copy.cause_ = std::make_unique<Error>(std::move(cause));
        return copy;
    }

    [[nodiscard]] Error with_cause(Error cause) && {
        cause_ = std::make_unique<Error>(std::move(cause));
        return std::move(*this);
    }

private:
    ErrorCode code_;
    std::string message_;
    std::unique_ptr<Error> cause_;
};

// 便捷构造函数
[[nodiscard]] inline Error make_error(
    ErrorCode code, std::string message) {
    return Error(code, std::move(message));
}

// 带 format 参数的构造
template<typename... Args>
[[nodiscard]] Error make_error_fmt(
    ErrorCode code,
    std::format_string<Args...> fmt, Args&&... args) {
    return Error(code, std::format(fmt, std::forward<Args>(args)...));
}

// Result 类型别名
template<typename T>
using Result = std::expected<T, Error>;

// VoidResult 用于无返回值的操作
using VoidResult = std::expected<void, Error>;

namespace error {
    [[nodiscard]] inline Error make(std::string message) {
        return Error(ErrorCode::unknown, std::move(message));
    }

    [[nodiscard]] inline Error wrap(const Error& cause, std::string message) {
        return Error(ErrorCode::unknown, std::move(message), cause);
    }

    template<typename T>
    [[nodiscard]] inline Result<T> expected(T value) {
        return value;
    }

    template<typename T>
    [[nodiscard]] inline Result<T> expected(Error err) {
        return std::unexpected(std::move(err));
    }
}

// TRY 宏 - 用于错误传播（类似 Rust 的 ? 运算符）
// 用法: auto val = TRY(some_function());
#define TRY(expr)                                                    \
    ({                                                               \
        auto&& _try_result = (expr);                                 \
        if (!_try_result.has_value()) {                              \
            return std::unexpected(std::move(_try_result.error()));   \
        }                                                            \
        std::move(*_try_result);                                     \
    })

// 不带值版本（用于 VoidResult）
#define TRY_VOID(expr)                                               \
    do {                                                             \
        auto&& _try_result = (expr);                                 \
        if (!_try_result.has_value()) {                              \
            return std::unexpected(std::move(_try_result.error()));   \
        }                                                            \
    } while (0)

// unexpected 便捷构造
[[nodiscard]] inline auto unexpected_error(
    ErrorCode code, std::string message) {
    return std::unexpected(Error(code, std::move(message)));
}

// Concept: 可转为 Error 的类型（简化版以兼容模块系统）
template<typename T>
concept ErrorLike = true;

} // namespace cc::utils
