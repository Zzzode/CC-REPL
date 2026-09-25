/// @file parse_int.cppm
/// @brief Portable integer parsing with std::from_chars semantics.
///
/// Apple's libc++ gates the integral std::from_chars overloads behind a newer
/// macOS SDK (macOS 26) than CI targets, so including <charconv> for integer
/// parsing fails to compile on macos-14. This is a small header-only
/// replacement with the same observable contract:
///   - no leading whitespace or '+' accepted (optional leading '-' only),
///   - every character in [first,last) must be a digit consumed fully,
///   - out-of-range returns errc::result_out_of_range, malformed ->
///     invalid_argument,
///   - on success ec == errc{} and ptr == last.
/// It handles both signed and unsigned integer types.
module;

#include <cstdint>

export module cc.utils.parse_int;

import std;

namespace cc::utils::parse_int_detail {

template <typename T>
struct ParseResult {
    const char* ptr;
    std::errc ec;
};

/// Signed integer parse.
template <typename T>
    requires std::is_signed_v<T> && std::is_integral_v<T>
[[nodiscard]] inline ParseResult<T> from_chars_int(const char* first,
                                                    const char* last,
                                                    T& value) noexcept {
    if (first == last) return {first, std::errc::invalid_argument};
    const char* p = first;
    bool negative = false;
    if (*p == '-') { negative = true; ++p; }
    if (p == last) return {first, std::errc::invalid_argument};

    using U = std::make_unsigned_t<T>;
    constexpr U kMax = static_cast<U>(std::numeric_limits<T>::max());
    // |T::min| is 2^(N-1); build magnitude in the unsigned type.
    constexpr U kNegLimit = static_cast<U>(std::numeric_limits<T>::max()) + U{1};
    U magnitude = 0;
    for (; p != last; ++p) {
        if (*p < '0' || *p > '9') {
            return {first, std::errc::invalid_argument};
        }
        unsigned digit = static_cast<unsigned>(*p - '0');
        U digit_val = static_cast<U>(digit);
        U limit = negative ? kNegLimit : kMax;
        if (magnitude > (limit - digit_val) / 10u) {
            return {last, std::errc::result_out_of_range};
        }
        magnitude = magnitude * 10u + digit_val;
    }
    // Negate in the unsigned domain (wraps mod 2^N), then cast: 2^63 becomes
    // LLONG_MIN, which is well-defined for the two's-complement cast in C++20+.
    value = negative ? static_cast<T>(U{0} - magnitude)
                     : static_cast<T>(magnitude);
    return {last, std::errc{}};
}

/// Unsigned integer parse.
template <typename T>
    requires std::is_unsigned_v<T> && std::is_integral_v<T>
[[nodiscard]] inline ParseResult<T> from_chars_int(const char* first,
                                                    const char* last,
                                                    T& value) noexcept {
    if (first == last) return {first, std::errc::invalid_argument};
    const char* p = first;
    T magnitude = 0;
    for (; p != last; ++p) {
        if (*p < '0' || *p > '9') {
            return {first, std::errc::invalid_argument};
        }
        unsigned digit = static_cast<unsigned>(*p - '0');
        T digit_val = static_cast<T>(digit);
        if (magnitude > (std::numeric_limits<T>::max() - digit_val) / T{10}) {
            return {last, std::errc::result_out_of_range};
        }
        magnitude = magnitude * T{10} + digit_val;
    }
    value = magnitude;
    return {last, std::errc{}};
}

} // namespace cc::utils::parse_int_detail

export namespace cc::utils {

/// Portable integer from_chars replacement. Accepts any integral T.
/// Returns {ptr, errc} with std::from_chars semantics.
template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] inline auto from_chars(const char* first, const char* last,
                                     T& value) noexcept {
    return parse_int_detail::from_chars_int(first, last, value);
}

} // namespace cc::utils
