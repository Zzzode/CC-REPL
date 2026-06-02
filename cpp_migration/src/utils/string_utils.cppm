module;
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <optional>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstdint>

export module cc.utils.string_utils;

export namespace cc::utils {

// 去除字符串首尾空白字符
[[nodiscard]] inline std::string trim(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string_view::npos) return {};
    auto end = sv.find_last_not_of(" \t\n\r\f\v");
    return std::string(sv.substr(start, end - start + 1));
}

// 按分隔符拆分字符串
[[nodiscard]] inline std::vector<std::string> split(std::string_view sv, char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= sv.size()) {
        auto pos = sv.find(delimiter, start);
        if (pos == std::string_view::npos) {
            result.emplace_back(sv.substr(start));
            break;
        }
        result.emplace_back(sv.substr(start, pos - start));
        start = pos + 1;
    }
    return result;
}

// 用分隔符连接字符串数组
[[nodiscard]] inline std::string join(std::span<const std::string> parts, std::string_view separator) {
    if (parts.empty()) return {};
    std::string result;
    // 预估总长度以减少内存分配
    size_t total = 0;
    for (const auto& p : parts) total += p.size();
    total += separator.size() * (parts.size() - 1);
    result.reserve(total);

    result += parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        result += separator;
        result += parts[i];
    }
    return result;
}

// 不区分大小写的前缀匹配
[[nodiscard]] inline bool starts_with_ignore_case(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// 不区分大小写的包含检测
[[nodiscard]] inline bool contains_ignore_case(std::string_view str, std::string_view needle) {
    if (needle.empty()) return true;
    if (str.size() < needle.size()) return false;
    for (size_t i = 0; i <= str.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(str[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// 重复字符串n次
[[nodiscard]] inline std::string repeat(std::string_view sv, int count) {
    if (count <= 0) return {};
    std::string result;
    result.reserve(sv.size() * static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        result += sv;
    }
    return result;
}

// 左填充到指定宽度
[[nodiscard]] inline std::string pad_left(std::string_view sv, size_t width, char fill = ' ') {
    if (sv.size() >= width) return std::string(sv);
    return std::string(width - sv.size(), fill) + std::string(sv);
}

// 右填充到指定宽度
[[nodiscard]] inline std::string pad_right(std::string_view sv, size_t width, char fill = ' ') {
    if (sv.size() >= width) return std::string(sv);
    return std::string(sv) + std::string(width - sv.size(), fill);
}

// 转换为小写
[[nodiscard]] inline std::string to_lower(std::string_view sv) {
    std::string result(sv);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// 转换为大写
[[nodiscard]] inline std::string to_upper(std::string_view sv) {
    std::string result(sv);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

// 替换所有匹配子串
[[nodiscard]] inline std::string replace_all(std::string str, std::string_view from, std::string_view to) {
    if (from.empty()) return str;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
    return str;
}

// Escape regex metacharacters so the result can be used as a literal pattern.
[[nodiscard]] inline std::string escape_regex(std::string_view sv) {
    std::string result;
    result.reserve(sv.size() * 2);
    for (char ch : sv) {
        switch (ch) {
            case '.': case '*': case '+': case '?': case '^': case '$':
            case '{': case '}': case '(': case ')': case '|': case '[':
            case ']': case '\\':
                result.push_back('\\');
                break;
            default:
                break;
        }
        result.push_back(ch);
    }
    return result;
}

// Uppercase the first byte/ASCII character while preserving the rest unchanged.
[[nodiscard]] inline std::string capitalize(std::string_view sv) {
    if (sv.empty()) return {};
    std::string result(sv);
    result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

// Return singular or plural word form based on count.
[[nodiscard]] inline std::string plural(
    std::int64_t n,
    std::string_view word,
    std::optional<std::string_view> plural_word = std::nullopt
) {
    if (n == 1) return std::string(word);
    return std::string(plural_word.value_or(std::string_view{}).empty()
        ? std::string(word) + "s"
        : std::string(plural_word.value()));
}

// Return the first line without allocating a split vector.
[[nodiscard]] inline std::string first_line_of(std::string_view sv) {
    auto pos = sv.find('\n');
    return std::string(pos == std::string_view::npos ? sv : sv.substr(0, pos));
}

// Count occurrences of a character starting at a byte offset.
[[nodiscard]] inline std::size_t count_char_in_string(
    std::string_view sv,
    char needle,
    std::size_t start = 0
) {
    std::size_t count = 0;
    auto pos = sv.find(needle, start);
    while (pos != std::string_view::npos) {
        ++count;
        pos = sv.find(needle, pos + 1);
    }
    return count;
}

// Normalize UTF-8 full-width digits U+FF10..U+FF19 to ASCII 0..9.
[[nodiscard]] inline std::string normalize_full_width_digits(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (std::size_t i = 0; i < sv.size();) {
        const auto byte = static_cast<unsigned char>(sv[i]);
        if (i + 2 < sv.size() && byte == 0xEF &&
            static_cast<unsigned char>(sv[i + 1]) == 0xBC) {
            const auto third = static_cast<unsigned char>(sv[i + 2]);
            if (third >= 0x90 && third <= 0x99) {
                result.push_back(static_cast<char>('0' + (third - 0x90)));
                i += 3;
                continue;
            }
        }
        result.push_back(sv[i]);
        ++i;
    }
    return result;
}

// Normalize UTF-8 full-width space U+3000 to ASCII space.
[[nodiscard]] inline std::string normalize_full_width_space(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (std::size_t i = 0; i < sv.size();) {
        if (i + 2 < sv.size() &&
            static_cast<unsigned char>(sv[i]) == 0xE3 &&
            static_cast<unsigned char>(sv[i + 1]) == 0x80 &&
            static_cast<unsigned char>(sv[i + 2]) == 0x80) {
            result.push_back(' ');
            i += 3;
            continue;
        }
        result.push_back(sv[i]);
        ++i;
    }
    return result;
}

// Join lines with a delimiter, appending a truncation marker once max_size is exceeded.
[[nodiscard]] inline std::string safe_join_lines(
    std::span<const std::string> lines,
    std::string_view delimiter = ",",
    std::size_t max_size = (std::size_t{1} << 25)
) {
    constexpr std::string_view truncation_marker = "...[truncated]";
    std::string result;
    for (const auto& line : lines) {
        const bool needs_delimiter = !result.empty();
        const std::size_t delimiter_size = needs_delimiter ? delimiter.size() : 0;
        if (result.size() + delimiter_size + line.size() <= max_size) {
            if (needs_delimiter) result += delimiter;
            result += line;
            continue;
        }

        const auto remaining = static_cast<std::int64_t>(max_size)
            - static_cast<std::int64_t>(result.size())
            - static_cast<std::int64_t>(delimiter_size)
            - static_cast<std::int64_t>(truncation_marker.size());
        if (remaining > 0) {
            if (needs_delimiter) result += delimiter;
            result += line.substr(0, static_cast<std::size_t>(remaining));
            result += truncation_marker;
        } else {
            result += truncation_marker;
        }
        return result;
    }
    return result;
}

} // namespace cc::utils
