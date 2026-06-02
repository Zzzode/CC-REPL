module;
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>

export module cc.utils.text_highlighting;

export namespace cc::utils {

// Highlight all occurrences of query in text with specified ANSI color
std::string highlight_matches(std::string_view text, std::string_view query, std::string_view color) {
    if (query.empty()) return std::string(text);

    static constexpr std::string_view reset = "\033[0m";
    std::string result;
    result.reserve(text.size() * 2);

    std::size_t pos = 0;
    while (pos < text.size()) {
        auto found = text.find(query, pos);
        if (found == std::string_view::npos) {
            result += text.substr(pos);
            break;
        }
        // Append text before match
        result += text.substr(pos, found - pos);
        // Append colored match
        result += color;
        result += query;
        result += reset;
        pos = found + query.size();
    }

    return result;
}

// Highlight regex pattern matches in text
std::string highlight_regex(std::string_view text, std::string_view pattern, std::string_view color) {
    static constexpr std::string_view reset = "\033[0m";

    try {
        std::regex re(pattern.begin(), pattern.end());
        std::string input(text);
        std::string result;
        result.reserve(input.size() * 2);

        std::sregex_iterator it(input.begin(), input.end(), re);
        std::sregex_iterator end;

        std::size_t last_pos = 0;
        for (; it != end; ++it) {
            auto& match = *it;
            std::size_t match_start = static_cast<std::size_t>(match.position());
            std::size_t match_len = static_cast<std::size_t>(match.length());

            // Text before match
            result += input.substr(last_pos, match_start - last_pos);
            // Colored match
            result += color;
            result += match.str();
            result += reset;

            last_pos = match_start + match_len;
        }
        // Remaining text
        result += input.substr(last_pos);
        return result;
    } catch (const std::regex_error&) {
        // If regex is invalid, return text unmodified
        return std::string(text);
    }
}

// Count non-overlapping occurrences of query in text
std::size_t count_matches(std::string_view text, std::string_view query) {
    if (query.empty()) return 0;

    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto found = text.find(query, pos);
        if (found == std::string_view::npos) break;
        ++count;
        pos = found + query.size();
    }
    return count;
}

} // namespace cc::utils
