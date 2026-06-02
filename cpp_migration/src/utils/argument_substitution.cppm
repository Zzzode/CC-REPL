module;

#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.argument_substitution;

export namespace cc::utils::argument_substitution {

[[nodiscard]] inline bool is_blank(std::string_view value) noexcept {
    for (unsigned char ch : value) {
        if (!std::isspace(ch)) return false;
    }
    return true;
}

[[nodiscard]] inline bool is_word_char(char ch) noexcept {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

[[nodiscard]] inline bool is_numeric_only(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (unsigned char ch : value) {
        if (!std::isdigit(ch)) return false;
    }
    return true;
}

[[nodiscard]] inline std::vector<std::string> split_whitespace(std::string_view value) {
    std::vector<std::string> result;
    std::size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        const auto start = i;
        while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        if (start < i) result.emplace_back(value.substr(start, i - start));
    }
    return result;
}

[[nodiscard]] inline std::optional<std::vector<std::string>> parse_shell_like_arguments(std::string_view args) {
    std::vector<std::string> result;
    std::size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i]))) ++i;
        if (i >= args.size()) break;

        std::string current;
        bool has_token = false;
        while (i < args.size() && !std::isspace(static_cast<unsigned char>(args[i]))) {
            if (args[i] == '"' || args[i] == '\'') {
                const char quote = args[i++];
                has_token = true;
                while (i < args.size() && args[i] != quote) {
                    if (args[i] == '\\' && i + 1 < args.size()) {
                        current.push_back(args[i + 1]);
                        i += 2;
                    } else {
                        current.push_back(args[i++]);
                    }
                }
                if (i >= args.size()) return std::nullopt;
                ++i;
                continue;
            }
            if (args[i] == '\\' && i + 1 < args.size()) {
                current.push_back(args[i + 1]);
                i += 2;
                has_token = true;
                continue;
            }
            current.push_back(args[i++]);
            has_token = true;
        }
        if (has_token) result.push_back(std::move(current));
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string> parse_arguments(std::string_view args) {
    if (args.empty() || is_blank(args)) return {};
    auto parsed = parse_shell_like_arguments(args);
    if (!parsed.has_value()) return split_whitespace(args);
    return *parsed;
}

[[nodiscard]] inline bool is_valid_argument_name(std::string_view name) noexcept {
    return !is_blank(name) && !is_numeric_only(name);
}

[[nodiscard]] inline std::vector<std::string> parse_argument_names(std::optional<std::string_view> argument_names) {
    if (!argument_names.has_value()) return {};
    std::vector<std::string> result;
    for (auto&& part : split_whitespace(*argument_names)) {
        if (is_valid_argument_name(part)) result.push_back(std::move(part));
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string> parse_argument_names(const std::optional<std::string>& argument_names) {
    if (!argument_names.has_value()) return {};
    return parse_argument_names(std::optional<std::string_view>{std::string_view(*argument_names)});
}

[[nodiscard]] inline std::vector<std::string> parse_argument_names(std::span<const std::string> argument_names) {
    std::vector<std::string> result;
    for (const auto& name : argument_names) {
        if (is_valid_argument_name(name)) result.push_back(name);
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string> parse_argument_names(const std::vector<std::string>& argument_names) {
    return parse_argument_names(std::span<const std::string>(argument_names.data(), argument_names.size()));
}

[[nodiscard]] inline std::optional<std::string> generate_progressive_argument_hint(
    std::span<const std::string> argument_names,
    std::span<const std::string> typed_args
) {
    if (typed_args.size() >= argument_names.size()) return std::nullopt;
    std::string result;
    for (std::size_t i = typed_args.size(); i < argument_names.size(); ++i) {
        if (!result.empty()) result += ' ';
        result += '[';
        result += argument_names[i];
        result += ']';
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> generate_progressive_argument_hint(
    const std::vector<std::string>& argument_names,
    const std::vector<std::string>& typed_args
) {
    return generate_progressive_argument_hint(
        std::span<const std::string>(argument_names.data(), argument_names.size()),
        std::span<const std::string>(typed_args.data(), typed_args.size())
    );
}

inline void replace_named_argument(std::string& content, std::string_view name, std::string_view value) {
    if (name.empty()) return;
    const std::string placeholder = "$" + std::string(name);
    std::size_t pos = 0;
    while ((pos = content.find(placeholder, pos)) != std::string::npos) {
        const auto next = pos + placeholder.size();
        if (next < content.size() && (content[next] == '[' || is_word_char(content[next]))) {
            pos = next;
            continue;
        }
        content.replace(pos, placeholder.size(), value);
        pos += value.size();
    }
}

inline void replace_all(std::string& content, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = content.find(needle, pos)) != std::string::npos) {
        content.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

[[nodiscard]] inline std::string replace_indexed_arguments(std::string content, std::string_view prefix, const std::vector<std::string>& parsed_args) {
    std::size_t pos = 0;
    while ((pos = content.find(prefix, pos)) != std::string::npos) {
        auto index_start = pos + prefix.size();
        bool bracketed = false;
        if (prefix == std::string_view("$ARGUMENTS")) {
            if (index_start >= content.size() || content[index_start] != '[') {
                pos = index_start;
                continue;
            }
            bracketed = true;
            ++index_start;
        }

        auto index_end = index_start;
        while (index_end < content.size() && std::isdigit(static_cast<unsigned char>(content[index_end]))) ++index_end;
        if (index_end == index_start) {
            pos = index_start;
            continue;
        }
        if (bracketed) {
            if (index_end >= content.size() || content[index_end] != ']') {
                pos = index_end;
                continue;
            }
        } else if (index_end < content.size() && is_word_char(content[index_end])) {
            pos = index_end;
            continue;
        }

        std::size_t index = 0;
        for (std::size_t i = index_start; i < index_end; ++i) index = index * 10 + static_cast<std::size_t>(content[i] - '0');
        const auto replacement = index < parsed_args.size() ? std::string_view(parsed_args[index]) : std::string_view{};
        const auto replace_end = bracketed ? index_end + 1 : index_end;
        content.replace(pos, replace_end - pos, replacement);
        pos += replacement.size();
    }
    return content;
}

[[nodiscard]] inline std::string substitute_arguments_impl(
    std::string_view content,
    std::optional<std::string_view> args,
    bool append_if_no_placeholder = true,
    const std::vector<std::string>& argument_names = {}
) {
    if (!args.has_value()) return std::string(content);

    std::string result(content);
    const std::string original(result);
    const auto parsed_args = parse_arguments(*args);

    for (std::size_t i = 0; i < argument_names.size(); ++i) {
        replace_named_argument(result, argument_names[i], i < parsed_args.size() ? std::string_view(parsed_args[i]) : std::string_view{});
    }

    result = replace_indexed_arguments(std::move(result), "$ARGUMENTS", parsed_args);
    result = replace_indexed_arguments(std::move(result), "$", parsed_args);
    replace_all(result, "$ARGUMENTS", *args);

    if (result == original && append_if_no_placeholder && !args->empty()) {
        result += "\n\nARGUMENTS: ";
        result += *args;
    }
    return result;
}

[[nodiscard]] inline std::string substitute_arguments(
    std::string_view content,
    const std::optional<std::string>& args,
    bool append_if_no_placeholder = true,
    const std::vector<std::string>& argument_names = {}
) {
    if (!args.has_value()) return std::string(content);
    return substitute_arguments_impl(content, std::optional<std::string_view>{std::string_view(*args)}, append_if_no_placeholder, argument_names);
}

[[nodiscard]] inline std::string substitute_arguments(
    std::string_view content,
    std::nullopt_t,
    bool append_if_no_placeholder = true,
    const std::vector<std::string>& argument_names = {}
) {
    return substitute_arguments(content, std::optional<std::string>{}, append_if_no_placeholder, argument_names);
}

[[nodiscard]] inline std::string substitute_arguments(
    std::string_view content,
    const char* args,
    bool append_if_no_placeholder = true,
    const std::vector<std::string>& argument_names = {}
) {
    if (args == nullptr) return std::string(content);
    return substitute_arguments_impl(content, std::optional<std::string_view>{std::string_view(args)}, append_if_no_placeholder, argument_names);
}

[[nodiscard]] inline std::string substitute_arguments(
    std::string_view content,
    std::string_view args,
    bool append_if_no_placeholder = true,
    const std::vector<std::string>& argument_names = {}
) {
    return substitute_arguments_impl(content, std::optional<std::string_view>{args}, append_if_no_placeholder, argument_names);
}

} // namespace cc::utils::argument_substitution
