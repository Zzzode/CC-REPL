module;

#include <cmath>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.semantic_number;

export namespace cc::utils::semantic_number {

[[nodiscard]] inline bool is_decimal_number_literal(std::string_view value) noexcept {
    if (value.empty()) return false;
    std::size_t i = 0;
    if (value[i] == '-') {
        ++i;
        if (i == value.size()) return false;
    }

    bool saw_integer_digit = false;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
        saw_integer_digit = true;
        ++i;
    }
    if (!saw_integer_digit) return false;

    if (i < value.size() && value[i] == '.') {
        ++i;
        bool saw_fraction_digit = false;
        while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
            saw_fraction_digit = true;
            ++i;
        }
        if (!saw_fraction_digit) return false;
    }

    return i == value.size();
}

[[nodiscard]] inline std::optional<double> coerce_semantic_number(std::string_view value) {
    if (!is_decimal_number_literal(value)) return std::nullopt;
    try {
        const double parsed = std::stod(std::string(value));
        if (std::isfinite(parsed)) return parsed;
    } catch (...) {
    }
    return std::nullopt;
}

} // namespace cc::utils::semantic_number
