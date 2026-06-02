module;

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.shell_rule_matching;

export namespace cc::utils::shell_rule_matching {

enum class ShellPermissionRuleType : unsigned char {
    Exact,
    Prefix,
    Wildcard,
};

struct ShellPermissionRule {
    ShellPermissionRuleType type = ShellPermissionRuleType::Exact;
    std::string command;
    std::string prefix;
    std::string pattern;
};

struct PermissionUpdateRule {
    std::string tool_name;
    std::string rule_content;
};

struct PermissionUpdate {
    std::string type;
    std::vector<PermissionUpdateRule> rules;
    std::string behavior;
    std::string destination;
};

namespace detail {

[[nodiscard]] inline std::string trim_copy(std::string_view value) {
    auto first = value.begin();
    auto last = value.end();
    while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    while (first != last) {
        auto prev = last;
        --prev;
        if (!std::isspace(static_cast<unsigned char>(*prev))) break;
        last = prev;
    }
    return std::string(first, last);
}

[[nodiscard]] inline bool ends_with(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] inline std::size_t preceding_backslash_count(std::string_view value, std::size_t index) noexcept {
    std::size_t count = 0;
    while (index > 0 && value[index - 1] == '\\') {
        ++count;
        --index;
    }
    return count;
}

[[nodiscard]] inline bool is_unescaped_star(std::string_view value, std::size_t index) noexcept {
    return value[index] == '*' && preceding_backslash_count(value, index) % 2 == 0;
}

[[nodiscard]] inline std::size_t count_unescaped_stars(std::string_view value) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (is_unescaped_star(value, i)) ++count;
    }
    return count;
}

struct PatternToken {
    bool wildcard = false;
    char literal = '\0';
};

[[nodiscard]] inline std::vector<PatternToken> tokenize_pattern(std::string_view pattern) {
    std::vector<PatternToken> tokens;
    for (std::size_t i = 0; i < pattern.size();) {
        const char current = pattern[i];
        if (current == '\\' && i + 1 < pattern.size()) {
            const char next = pattern[i + 1];
            if (next == '*' || next == '\\') {
                tokens.push_back(PatternToken{.wildcard = false, .literal = next});
                i += 2;
                continue;
            }
        }
        if (current == '*') {
            tokens.push_back(PatternToken{.wildcard = true});
        } else {
            tokens.push_back(PatternToken{.wildcard = false, .literal = current});
        }
        ++i;
    }
    return tokens;
}

[[nodiscard]] inline char normalize_case(char value, bool case_insensitive) noexcept {
    if (!case_insensitive) return value;
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

[[nodiscard]] inline bool literal_equals(char left, char right, bool case_insensitive) noexcept {
    return normalize_case(left, case_insensitive) == normalize_case(right, case_insensitive);
}

[[nodiscard]] inline bool match_tokens(const std::vector<PatternToken>& tokens, std::string_view command, bool case_insensitive) {
    std::size_t pi = 0;
    std::size_t ci = 0;
    std::size_t star_index = std::string_view::npos;
    std::size_t star_command_index = 0;

    while (ci < command.size()) {
        if (pi < tokens.size() && tokens[pi].wildcard) {
            star_index = pi++;
            star_command_index = ci;
        } else if (pi < tokens.size() && literal_equals(tokens[pi].literal, command[ci], case_insensitive)) {
            ++pi;
            ++ci;
        } else if (star_index != std::string_view::npos) {
            pi = star_index + 1;
            ci = ++star_command_index;
        } else {
            return false;
        }
    }

    while (pi < tokens.size() && tokens[pi].wildcard) ++pi;
    return pi == tokens.size();
}

} // namespace detail

[[nodiscard]] inline std::optional<std::string> permission_rule_extract_prefix(std::string_view permission_rule) {
    if (!detail::ends_with(permission_rule, ":*") || permission_rule.size() <= 2) return std::nullopt;
    return std::string(permission_rule.substr(0, permission_rule.size() - 2));
}

[[nodiscard]] inline bool has_wildcards(std::string_view pattern) noexcept {
    if (detail::ends_with(pattern, ":*")) return false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (detail::is_unescaped_star(pattern, i)) return true;
    }
    return false;
}

[[nodiscard]] inline bool match_wildcard_pattern(std::string_view pattern, std::string_view command, bool case_insensitive = false) {
    const auto trimmed_pattern = detail::trim_copy(pattern);
    const auto unescaped_star_count = detail::count_unescaped_stars(trimmed_pattern);

    if (trimmed_pattern.size() >= 2 &&
        trimmed_pattern.back() == '*' &&
        trimmed_pattern[trimmed_pattern.size() - 2] == ' ' &&
        detail::is_unescaped_star(trimmed_pattern, trimmed_pattern.size() - 1) &&
        unescaped_star_count == 1) {
        const auto prefix = trimmed_pattern.substr(0, trimmed_pattern.size() - 2);
        if (command.size() == prefix.size()) {
            return detail::match_tokens(detail::tokenize_pattern(prefix), command, case_insensitive);
        }
        if (command.size() > prefix.size() && command[prefix.size()] == ' ') {
            return detail::match_tokens(detail::tokenize_pattern(prefix), command.substr(0, prefix.size()), case_insensitive);
        }
        return false;
    }

    return detail::match_tokens(detail::tokenize_pattern(trimmed_pattern), command, case_insensitive);
}

[[nodiscard]] inline ShellPermissionRule parse_permission_rule(std::string_view permission_rule) {
    if (auto prefix = permission_rule_extract_prefix(permission_rule); prefix.has_value()) {
        return ShellPermissionRule{
            .type = ShellPermissionRuleType::Prefix,
            .command = {},
            .prefix = *prefix,
            .pattern = {},
        };
    }
    if (has_wildcards(permission_rule)) {
        return ShellPermissionRule{
            .type = ShellPermissionRuleType::Wildcard,
            .command = {},
            .prefix = {},
            .pattern = std::string(permission_rule),
        };
    }
    return ShellPermissionRule{
        .type = ShellPermissionRuleType::Exact,
        .command = std::string(permission_rule),
        .prefix = {},
        .pattern = {},
    };
}

[[nodiscard]] inline std::vector<PermissionUpdate> suggestion_for_exact_command(std::string_view tool_name, std::string_view command) {
    return {
        PermissionUpdate{
            .type = "addRules",
            .rules = {PermissionUpdateRule{.tool_name = std::string(tool_name), .rule_content = std::string(command)}},
            .behavior = "allow",
            .destination = "localSettings",
        },
    };
}

[[nodiscard]] inline std::vector<PermissionUpdate> suggestion_for_prefix(std::string_view tool_name, std::string_view prefix) {
    return {
        PermissionUpdate{
            .type = "addRules",
            .rules = {PermissionUpdateRule{.tool_name = std::string(tool_name), .rule_content = std::string(prefix) + ":*"}},
            .behavior = "allow",
            .destination = "localSettings",
        },
    };
}

} // namespace cc::utils::shell_rule_matching
