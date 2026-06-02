module;

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.bash_shell_quoting;

export namespace cc::utils::bash_shell_quoting {

namespace detail {
    [[nodiscard]] inline bool is_shell_safe(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
    }

    [[nodiscard]] inline std::string shell_quote_one(std::string_view arg) {
        if (arg.empty()) return "''";
        bool needs_quote = false;
        for (char c : arg) {
            if (!is_shell_safe(c)) {
                needs_quote = true;
                break;
            }
        }
        if (!needs_quote) return std::string(arg);
        std::string out = "'";
        for (char c : arg) {
            if (c == '\'') out += "'\"'\"'";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    [[nodiscard]] inline bool contains_heredoc(std::string_view command) {
        const std::string s(command);
        static const std::regex bitshift1(R"(\d\s*<<\s*\d)");
        static const std::regex bitshift2(R"(\[\[\s*\d+\s*<<\s*\d+\s*\]\])");
        static const std::regex bitshift3(R"(\$\(\(.*<<.*\)\))");
        if (std::regex_search(s, bitshift1) || std::regex_search(s, bitshift2) || std::regex_search(s, bitshift3)) return false;
        static const std::regex heredoc(R"(<<-?\s*(?:(['"]?)(\w+)\1|\\(\w+)))");
        return std::regex_search(s, heredoc);
    }

    [[nodiscard]] inline bool contains_multiline_string(std::string_view command) {
        bool in_single = false;
        bool in_double = false;
        for (std::size_t i = 0; i < command.size(); ++i) {
            const char c = command[i];
            if (c == '\\' && !in_single) {
                ++i;
                continue;
            }
            if (c == '\'' && !in_double) in_single = !in_single;
            else if (c == '"' && !in_single) in_double = !in_double;
            else if (c == '\n' && (in_single || in_double)) return true;
        }
        return false;
    }

    [[nodiscard]] inline std::vector<std::string> split_compound(std::string_view command) {
        std::vector<std::string> parts;
        std::string current;
        bool in_single = false;
        bool in_double = false;
        for (std::size_t i = 0; i < command.size(); ++i) {
            const char c = command[i];
            if (c == '\\' && !in_single) {
                current.push_back(c);
                if (i + 1 < command.size()) current.push_back(command[++i]);
                continue;
            }
            if (c == '\'' && !in_double) in_single = !in_single;
            else if (c == '"' && !in_single) in_double = !in_double;
            if (!in_single && !in_double) {
                if (c == ';' || (c == '&' && i + 1 < command.size() && command[i + 1] == '&') || (c == '|' && i + 1 < command.size() && command[i + 1] == '|')) {
                    parts.push_back(current);
                    current.clear();
                    if (c == '&' || c == '|') ++i;
                    continue;
                }
            }
            current.push_back(c);
        }
        parts.push_back(current);
        return parts;
    }

    [[nodiscard]] inline std::string trim(std::string s) {
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    }
} // namespace detail

[[nodiscard]] inline bool has_shell_quote_single_quote_bug(std::string_view command) {
    bool in_single = false;
    bool in_double = false;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '\\' && !in_single) {
            ++i;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            if (!in_single) {
                std::size_t backslash_count = 0;
                std::size_t j = i;
                while (j > 0 && command[j - 1] == '\\') {
                    ++backslash_count;
                    --j;
                }
                if (backslash_count > 0 && backslash_count % 2 == 1) return true;
                if (backslash_count > 0 && backslash_count % 2 == 0 && command.find('\'', i + 1) != std::string_view::npos) return true;
            }
        }
    }
    return false;
}

[[nodiscard]] inline std::string quote(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(' ');
        out += detail::shell_quote_one(args[i]);
    }
    return out;
}

[[nodiscard]] inline std::string quote_shell_command(std::string_view command, bool add_stdin_redirect = true) {
    const bool heredoc = detail::contains_heredoc(command);
    if (heredoc || detail::contains_multiline_string(command)) {
        std::string quoted = detail::shell_quote_one(command);
        if (heredoc) return quoted;
        return add_stdin_redirect ? quoted + " < /dev/null" : quoted;
    }
    if (add_stdin_redirect) return quote({std::string(command), "<", "/dev/null"});
    return quote({std::string(command)});
}

[[nodiscard]] inline bool has_stdin_redirect(std::string_view command) {
    const std::string s(command);
    static const std::regex redirect(R"((^|[\s;&|])<(?![<(])\s*\S+)");
    return std::regex_search(s, redirect);
}

[[nodiscard]] inline bool should_add_stdin_redirect(std::string_view command) {
    if (detail::contains_heredoc(command)) return false;
    if (has_stdin_redirect(command)) return false;
    return true;
}

[[nodiscard]] inline std::string rewrite_windows_null_redirect(std::string command) {
    static const std::regex nul_redirect(R"((\d?&?>+\s*)[Nn][Uu][Ll](?=\s|$|[|&;)\n]))");
    return std::regex_replace(command, nul_redirect, "$1/dev/null");
}

[[nodiscard]] inline std::string longest_common_prefix(const std::vector<std::string>& strings) {
    if (strings.empty()) return "";
    if (strings.size() == 1) return strings[0];
    auto split_words = [](std::string_view s) {
        std::vector<std::string> out;
        std::string current;
        for (char c : s) {
            if (c == ' ') {
                if (!current.empty()) {
                    out.push_back(current);
                    current.clear();
                }
            } else current.push_back(c);
        }
        if (!current.empty()) out.push_back(current);
        return out;
    };
    const auto first_words = split_words(strings[0]);
    std::size_t common_words = first_words.size();
    for (std::size_t i = 1; i < strings.size(); ++i) {
        const auto other = split_words(strings[i]);
        std::size_t shared = 0;
        while (shared < common_words && shared < other.size() && first_words[shared] == other[shared]) ++shared;
        common_words = shared;
    }
    common_words = std::max<std::size_t>(1, common_words);
    std::string out;
    for (std::size_t i = 0; i < common_words && i < first_words.size(); ++i) {
        if (i) out.push_back(' ');
        out += first_words[i];
    }
    return out;
}

[[nodiscard]] inline std::vector<std::string> compound_command_prefixes(std::string_view command) {
    std::vector<std::string> subcommands = detail::split_compound(command);
    if (subcommands.size() <= 1) {
        std::string trimmed = detail::trim(std::string(command));
        return trimmed.empty() ? std::vector<std::string>{} : std::vector<std::string>{trimmed};
    }
    std::vector<std::string> prefixes;
    for (auto& subcommand : subcommands) {
        auto trimmed = detail::trim(std::move(subcommand));
        if (!trimmed.empty()) prefixes.push_back(std::move(trimmed));
    }
    if (prefixes.empty()) return {};

    std::vector<std::string> collapsed;
    std::vector<bool> used(prefixes.size(), false);
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        if (used[i]) continue;
        const std::string root = prefixes[i].substr(0, prefixes[i].find(' '));
        std::vector<std::string> group;
        for (std::size_t j = i; j < prefixes.size(); ++j) {
            const std::string other_root = prefixes[j].substr(0, prefixes[j].find(' '));
            if (other_root == root) {
                used[j] = true;
                group.push_back(prefixes[j]);
            }
        }
        collapsed.push_back(longest_common_prefix(group));
    }
    return collapsed;
}

} // namespace cc::utils::bash_shell_quoting

