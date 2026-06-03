module;
#include <string>
#include <expected>
#include <functional>
#include <stdexcept>
#include <optional>
#include <mutex>

export module cc.utils.lazy_schema;

import cc.utils.json_read;

export namespace cc::utils {


template <typename T>
class LazySchema {
public:
    using Validator = std::function<T(const JsonValue&)>;

    explicit LazySchema(JsonValue raw, Validator validator)
        : raw_(std::move(raw)), validator_(std::move(validator)) {}


    [[nodiscard]] const T& get() {
        std::call_once(validate_flag_, [this]() {
            try {
                cached_ = validator_(raw_);
                valid_ = true;
            } catch (const std::exception& e) {
                error_ = e.what();
                valid_ = false;
            }
        });
        if (!valid_) {
            throw std::runtime_error("Schema validation failed: " + error_);
        }
        return *cached_;
    }


    [[nodiscard]] bool is_validated() const { return cached_.has_value(); }


    [[nodiscard]] bool is_valid() {
        try { get(); return true; }
        catch (...) { return false; }
    }


    [[nodiscard]] const JsonValue& raw() const { return raw_; }

private:
    JsonValue raw_;
    Validator validator_;
    std::optional<T> cached_;
    std::once_flag validate_flag_;
    bool valid_ = false;
    std::string error_;
};


template <typename T>
[[nodiscard]] inline T validate_or_throw(const JsonValue& value,
                                          std::function<T(const JsonValue&)> validator) {
    try {
        return validator(value);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Validation failed: ") + e.what());
    }
}


template <typename T>
[[nodiscard]] inline std::expected<T, std::string> try_validate(
    const JsonValue& value,
    std::function<T(const JsonValue&)> validator) {
    try {
        return validator(value);
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Validation failed: ") + e.what());
    }
}

} // namespace cc::utils
