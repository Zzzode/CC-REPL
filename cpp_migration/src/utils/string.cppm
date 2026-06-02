/// @file string.cppm
/// @brief String utility functions - equivalent to src/utils/string.ts
module;

#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>
#include <cctype>

export module cc.utils.string;

export namespace cc::utils {

/// Trim whitespace from start of string
[[nodiscard]] inline std::string LTrim(std::string_view str) {
    auto result = std::string(str);
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    return result;
}

/// Trim whitespace from end of string
[[nodiscard]] inline std::string RTrim(std::string_view str) {
    auto result = std::string(str);
    result.erase(std::find_if(result.rbegin(), result.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), result.end());
    return result;
}

/// Trim whitespace from both ends
[[nodiscard]] inline std::string Trim(std::string_view str) {
    return LTrim(RTrim(str));
}

/// Split string by delimiter
[[nodiscard]] inline std::vector<std::string> Split(std::string_view str, char delimiter = ' ') {
    std::vector<std::string> result;
    if (str.empty()) {
        result.emplace_back();
        return result;
    }
    std::stringstream ss{std::string(str)};
    std::string token;

    while (std::getline(ss, token, delimiter)) {
        result.push_back(token);
    }

    return result;
}

/// Join strings with delimiter
[[nodiscard]] inline std::string Join(const std::vector<std::string>& parts, std::string_view delimiter = ", ") {
    if (parts.empty()) return "";

    std::string result = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        result += delimiter;
        result += parts[i];
    }
    return result;
}

/// Check if string starts with prefix
[[nodiscard]] inline bool StartsWith(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) return false;
    return str.substr(0, prefix.size()) == prefix;
}

/// Check if string ends with suffix
[[nodiscard]] inline bool EndsWith(std::string_view str, std::string_view suffix) {
    if (str.size() < suffix.size()) return false;
    return str.substr(str.size() - suffix.size(), suffix.size()) == suffix;
}

/// Convert string to lowercase
[[nodiscard]] inline std::string ToLower(std::string_view str) {
    std::string result(str);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

/// Convert string to uppercase
[[nodiscard]] inline std::string ToUpper(std::string_view str) {
    std::string result(str);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return result;
}

/// Replace all occurrences of substring
[[nodiscard]] inline std::string ReplaceAll(std::string_view str, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(str);

    std::string result(str);
    size_t pos = 0;

    while ((pos = result.find(from.data(), pos, from.size())) != std::string::npos) {
        result.replace(pos, from.size(), to.data(), to.size());
        pos += to.size();
    }

    return result;
}

/// Check if string contains substring
[[nodiscard]] inline bool Contains(std::string_view str, std::string_view substr) {
    return str.find(substr) != std::string_view::npos;
}

/// Truncate string with ellipsis
[[nodiscard]] inline std::string Truncate(std::string_view str, size_t max_length = 100, std::string_view ellipsis = "...") {
    if (str.size() <= max_length) {
        return std::string(str);
    }

    if (max_length <= ellipsis.size()) {
        return std::string(ellipsis.substr(0, max_length));
    }

    return std::string(str.substr(0, max_length - ellipsis.size())) + std::string(ellipsis);
}

/// Check if string is empty or all whitespace
[[nodiscard]] inline bool IsBlank(std::string_view str) {
    return str.empty() || std::all_of(str.begin(), str.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
}

/// Escape string for JSON output
[[nodiscard]] inline std::string EscapeJson(std::string_view str) {
    std::string result;
    result.reserve(str.size() * 2);

    for (char ch : str) {
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += ch;
        }
    }

    return result;
}

namespace string {
[[nodiscard]] inline std::string trim(std::string_view str) { return Trim(str); }
[[nodiscard]] inline std::vector<std::string> split(std::string_view str, char delimiter = ' ') { return Split(str, delimiter); }
[[nodiscard]] inline std::string join(const std::vector<std::string>& parts, std::string_view delimiter = ", ") { return Join(parts, delimiter); }
[[nodiscard]] inline bool starts_with(std::string_view str, std::string_view prefix) { return StartsWith(str, prefix); }
[[nodiscard]] inline bool ends_with(std::string_view str, std::string_view suffix) { return EndsWith(str, suffix); }
[[nodiscard]] inline std::string to_lower(std::string_view str) { return ToLower(str); }
[[nodiscard]] inline std::string to_upper(std::string_view str) { return ToUpper(str); }
[[nodiscard]] inline std::string replace_all(std::string_view str, std::string_view from, std::string_view to) { return ReplaceAll(str, from, to); }
[[nodiscard]] inline std::string truncate(std::string_view str, size_t max_length = 100, std::string_view ellipsis = "...") { return Truncate(str, max_length, ellipsis); }
}

} // namespace cc::utils
