module;
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

export module cc.utils.sanitization;

export namespace cc::utils::sanitization {

namespace detail {

[[nodiscard]] inline bool is_continuation(unsigned char byte) noexcept {
    return (byte & 0xC0u) == 0x80u;
}

[[nodiscard]] inline std::uint32_t decode_next(std::string_view input, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(input[i]);
    if (b0 < 0x80u) {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && i + 1 < input.size() && is_continuation(static_cast<unsigned char>(input[i + 1]))) {
        const auto cp = ((b0 & 0x1Fu) << 6u) | (static_cast<unsigned char>(input[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0u) == 0xE0u && i + 2 < input.size() &&
        is_continuation(static_cast<unsigned char>(input[i + 1])) &&
        is_continuation(static_cast<unsigned char>(input[i + 2]))) {
        const auto cp = ((b0 & 0x0Fu) << 12u) |
            ((static_cast<unsigned char>(input[i + 1]) & 0x3Fu) << 6u) |
            (static_cast<unsigned char>(input[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((b0 & 0xF8u) == 0xF0u && i + 3 < input.size() &&
        is_continuation(static_cast<unsigned char>(input[i + 1])) &&
        is_continuation(static_cast<unsigned char>(input[i + 2])) &&
        is_continuation(static_cast<unsigned char>(input[i + 3]))) {
        const auto cp = ((b0 & 0x07u) << 18u) |
            ((static_cast<unsigned char>(input[i + 1]) & 0x3Fu) << 12u) |
            ((static_cast<unsigned char>(input[i + 2]) & 0x3Fu) << 6u) |
            (static_cast<unsigned char>(input[i + 3]) & 0x3Fu);
        i += 4;
        return cp;
    }
    ++i;
    return b0;
}

inline void append_utf8(std::string& output, std::uint32_t cp) {
    if (cp <= 0x7Fu) {
        output.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
        output.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        output.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        output.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

[[nodiscard]] inline bool is_noncharacter(std::uint32_t cp) noexcept {
    return (cp >= 0xFDD0u && cp <= 0xFDEFu) || ((cp & 0xFFFEu) == 0xFFFEu && cp <= 0x10FFFFu);
}

[[nodiscard]] inline bool is_dangerous_code_point(std::uint32_t cp) noexcept {
    return
        (cp >= 0x200Bu && cp <= 0x200Fu) ||
        (cp >= 0x202Au && cp <= 0x202Eu) ||
        (cp >= 0x2066u && cp <= 0x2069u) ||
        cp == 0xFEFFu ||
        (cp >= 0xE000u && cp <= 0xF8FFu) ||
        (cp >= 0xF0000u && cp <= 0xFFFFDu) ||
        (cp >= 0x100000u && cp <= 0x10FFFDu) ||
        (cp >= 0xE0000u && cp <= 0xE007Fu) ||
        is_noncharacter(cp);
}

[[nodiscard]] inline std::string sanitize_once(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const std::uint32_t cp = decode_next(input, i);
        if (!is_dangerous_code_point(cp)) {
            append_utf8(output, cp);
        }
    }
    return output;
}

inline void append_nfkc_compat(std::string& output, std::uint32_t cp) {
    if (cp == 0x3000u) {
        output.push_back(' ');
        return;
    }
    if (cp >= 0xFF01u && cp <= 0xFF5Eu) {
        output.push_back(static_cast<char>(cp - 0xFEE0u));
        return;
    }
    if (cp >= 0x2460u && cp <= 0x2468u) {
        output.push_back(static_cast<char>('1' + (cp - 0x2460u)));
        return;
    }
    if (cp == 0x24EAu) {
        output.push_back('0');
        return;
    }
    if (cp >= 0x24F5u && cp <= 0x24FEu) {
        output.push_back(static_cast<char>('0' + (cp - 0x24F5u)));
        return;
    }
    if (cp == 0x00B2u) {
        output.push_back('2');
        return;
    }
    if (cp == 0x00B3u) {
        output.push_back('3');
        return;
    }
    if (cp == 0x00B9u) {
        output.push_back('1');
        return;
    }
    if (cp == 0x2070u) {
        output.push_back('0');
        return;
    }
    if (cp >= 0x2074u && cp <= 0x2079u) {
        output.push_back(static_cast<char>('4' + (cp - 0x2074u)));
        return;
    }
    switch (cp) {
        case 0x2160u:
            output += "I";
            return;
        case 0x2161u:
            output += "II";
            return;
        case 0x2162u:
            output += "III";
            return;
        case 0x2163u:
            output += "IV";
            return;
        case 0x2164u:
            output += "V";
            return;
        case 0x2165u:
            output += "VI";
            return;
        case 0x2166u:
            output += "VII";
            return;
        case 0x2167u:
            output += "VIII";
            return;
        case 0x2168u:
            output += "IX";
            return;
        case 0x2169u:
            output += "X";
            return;
        case 0x216Au:
            output += "XI";
            return;
        case 0x216Bu:
            output += "XII";
            return;
        case 0x338Fu:
            output += "kg";
            return;
        case 0xFB00u:
            output += "ff";
            return;
        case 0xFB01u:
            output += "fi";
            return;
        case 0xFB02u:
            output += "fl";
            return;
        case 0xFB03u:
            output += "ffi";
            return;
        case 0xFB04u:
            output += "ffl";
            return;
        case 0xFB05u:
        case 0xFB06u:
            output += "st";
            return;
        default:
            append_utf8(output, cp);
            return;
    }
}

[[nodiscard]] inline std::string normalize_nfkc_compat(std::string_view input) {
#ifdef __APPLE__
    CFStringRef source = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(input.data()),
        static_cast<CFIndex>(input.size()),
        kCFStringEncodingUTF8,
        false);
    if (source != nullptr) {
        CFMutableStringRef normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
        CFRelease(source);
        if (normalized != nullptr) {
            const Boolean ok = CFStringTransform(normalized, nullptr, CFSTR("Any-NFKC"), false);
            if (ok) {
                const CFIndex length = CFStringGetLength(normalized);
                const CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
                std::string output(static_cast<std::size_t>(max_size), '\0');
                if (CFStringGetCString(normalized, output.data(), max_size, kCFStringEncodingUTF8)) {
                    output.resize(std::char_traits<char>::length(output.c_str()));
                    CFRelease(normalized);
                    return output;
                }
            }
            CFRelease(normalized);
        }
    }
#endif
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const std::uint32_t cp = decode_next(input, i);
        append_nfkc_compat(output, cp);
    }
    return output;
}

} // namespace detail

struct SanitizedValue {
    using Array = std::vector<SanitizedValue>;
    struct Object {
        std::vector<std::pair<std::string, SanitizedValue>> entries;

        Object() = default;
        Object(std::initializer_list<std::pair<std::string, SanitizedValue>> init) : entries(init) {}
        explicit Object(std::vector<std::pair<std::string, SanitizedValue>> values) : entries(std::move(values)) {}

        [[nodiscard]] bool contains(std::string_view key) const {
            for (const auto& [candidate, _] : entries) {
                if (candidate == key) return true;
            }
            return false;
        }

        [[nodiscard]] const SanitizedValue& at(std::string_view key) const {
            for (const auto& [candidate, value] : entries) {
                if (candidate == key) return value;
            }
            throw std::out_of_range("SanitizedValue object key not found");
        }

        void insert_or_assign(std::string key, SanitizedValue value) {
            for (auto& [candidate, existing] : entries) {
                if (candidate == key) {
                    existing = std::move(value);
                    return;
                }
            }
            entries.emplace_back(std::move(key), std::move(value));
        }
    };
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Storage value = nullptr;

    SanitizedValue() = default;
    SanitizedValue(std::nullptr_t) : value(nullptr) {}
    SanitizedValue(bool v) : value(v) {}
    SanitizedValue(double v) : value(v) {}
    SanitizedValue(const char* v) : value(std::string(v == nullptr ? "" : v)) {}
    SanitizedValue(std::string v) : value(std::move(v)) {}
    SanitizedValue(Array v) : value(std::move(v)) {}
    SanitizedValue(Object v) : value(std::move(v)) {}

    [[nodiscard]] static SanitizedValue array(Array values) {
        return SanitizedValue(std::move(values));
    }

    [[nodiscard]] static SanitizedValue object(Object values) {
        return SanitizedValue(std::move(values));
    }

    [[nodiscard]] const std::string& as_string() const {
        return std::get<std::string>(value);
    }

    [[nodiscard]] const Array& as_array() const {
        return std::get<Array>(value);
    }

    [[nodiscard]] const Object& as_object() const {
        return std::get<Object>(value);
    }
};

[[nodiscard]] inline std::string partially_sanitize_unicode(std::string_view prompt) {
    std::string current(prompt);
    std::string previous;
    int iterations = 0;
    constexpr int max_iterations = 10;

    while (current != previous && iterations < max_iterations) {
        previous = current;
        current = detail::sanitize_once(detail::normalize_nfkc_compat(current));
        ++iterations;
    }

    if (iterations >= max_iterations) {
        throw std::runtime_error("Unicode sanitization reached maximum iterations");
    }
    return current;
}

[[nodiscard]] inline SanitizedValue recursively_sanitize_unicode(const SanitizedValue& value) {
    return std::visit([](const auto& item) -> SanitizedValue {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return SanitizedValue(partially_sanitize_unicode(item));
        } else if constexpr (std::is_same_v<T, SanitizedValue::Array>) {
            SanitizedValue::Array out;
            out.reserve(item.size());
            for (const auto& child : item) {
                out.push_back(recursively_sanitize_unicode(child));
            }
            return SanitizedValue::array(std::move(out));
        } else if constexpr (std::is_same_v<T, SanitizedValue::Object>) {
            SanitizedValue::Object out;
            for (const auto& [key, child] : item.entries) {
                out.insert_or_assign(partially_sanitize_unicode(key), recursively_sanitize_unicode(child));
            }
            return SanitizedValue::object(std::move(out));
        } else {
            return SanitizedValue(item);
        }
    }, value.value);
}

} // namespace cc::utils::sanitization
