// C++23 Module: Shell command parsing

module;
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.shell_parser;


export namespace cc::utils::shell_parser {


enum class TokenType : uint8_t {
    Command,
    Arg,
    Pipe,       // |
    Redirect,   // > >> < 2>
    Semicolon,  // ;
    And,        // &&
    Or,         // ||
    Background, // &
    Subshell
};


struct ShellToken {
    TokenType type;
    std::string value;
    size_t position{0};

    [[nodiscard]] bool is_operator() const {
        return type == TokenType::Pipe || type == TokenType::Redirect ||
               type == TokenType::Semicolon || type == TokenType::And ||
               type == TokenType::Or || type == TokenType::Background;
    }
};


struct HeredocInfo {
    std::string delimiter;
    std::string content;
    bool strip_tabs{false};
};


struct PipelineCommand {
    std::string command;
    std::vector<std::string> args;
    std::optional<std::string> input_redirect;
    std::optional<std::string> output_redirect;
    bool append_output{false};  // >> vs >
};


struct Pipeline {
    std::vector<PipelineCommand> commands;
    bool background{false};

    [[nodiscard]] size_t stage_count() const { return commands.size(); }
    [[nodiscard]] bool is_simple() const { return commands.size() == 1; }
};


struct Warning {
    std::string message;
    std::string pattern_matched;
    size_t position{0};
};


[[nodiscard]] inline std::vector<ShellToken> tokenize(std::string_view command) {
    std::vector<ShellToken> tokens;
    size_t i = 0;
    const size_t len = command.size();

    while (i < len) {

        if (std::isspace(static_cast<unsigned char>(command[i]))) { ++i; continue; }

        size_t start = i;


        if (i + 1 < len) {
            auto two = command.substr(i, 2);
            if (two == "&&") {
                tokens.push_back({TokenType::And, "&&", start});
                i += 2; continue;
            }
            if (two == "||") {
                tokens.push_back({TokenType::Or, "||", start});
                i += 2; continue;
            }
            if (two == ">>" || two == "2>") {
                tokens.push_back({TokenType::Redirect, std::string(two), start});
                i += 2; continue;
            }
        }


        if (command[i] == '|') {
            tokens.push_back({TokenType::Pipe, "|", start});
            ++i; continue;
        }
        if (command[i] == ';') {
            tokens.push_back({TokenType::Semicolon, ";", start});
            ++i; continue;
        }
        if (command[i] == '&') {
            tokens.push_back({TokenType::Background, "&", start});
            ++i; continue;
        }
        if (command[i] == '>' || command[i] == '<') {
            tokens.push_back({TokenType::Redirect, std::string(1, command[i]), start});
            ++i; continue;
        }


        if (i + 1 < len && command[i] == '$' && command[i + 1] == '(') {
            size_t depth = 1;
            size_t j = i + 2;
            while (j < len && depth > 0) {
                if (command[j] == '(') ++depth;
                else if (command[j] == ')') --depth;
                ++j;
            }
            tokens.push_back({TokenType::Subshell, std::string(command.substr(i, j - i)), start});
            i = j; continue;
        }


        if (command[i] == '"' || command[i] == '\'') {
            char quote = command[i];
            size_t j = i + 1;
            while (j < len && command[j] != quote) {
                if (command[j] == '\\' && j + 1 < len) ++j;
                ++j;
            }
            if (j < len) ++j;
            auto value = std::string(command.substr(i + 1, j - i - 2));
            auto type = tokens.empty() ? TokenType::Command : TokenType::Arg;
            tokens.push_back({type, std::move(value), start});
            i = j; continue;
        }


        size_t j = i;
        while (j < len && !std::isspace(static_cast<unsigned char>(command[j])) &&
               command[j] != '|' && command[j] != ';' && command[j] != '>' &&
               command[j] != '<' && command[j] != '&') {
            ++j;
        }

        auto value = std::string(command.substr(i, j - i));

        bool is_command = tokens.empty() || tokens.back().is_operator();
        tokens.push_back({is_command ? TokenType::Command : TokenType::Arg, std::move(value), start});
        i = j;
    }

    return tokens;
}


[[nodiscard]] inline Pipeline parse_pipeline(const std::vector<ShellToken>& tokens) {
    Pipeline pipeline;
    PipelineCommand current_cmd;
    bool expect_command = true;

    for (std::size_t idx = 0; idx < tokens.size(); ++idx) {
        const auto& token = tokens[idx];
        switch (token.type) {
            case TokenType::Command:
                if (!expect_command && !current_cmd.command.empty()) {

                    pipeline.commands.push_back(std::move(current_cmd));
                    current_cmd = {};
                }
                current_cmd.command = token.value;
                expect_command = false;
                break;
            case TokenType::Arg:
                current_cmd.args.push_back(token.value);
                break;
            case TokenType::Pipe:
                pipeline.commands.push_back(std::move(current_cmd));
                current_cmd = {};
                expect_command = true;
                break;
            case TokenType::Redirect:
                if (idx + 1 < tokens.size()) {
                    const auto& target = tokens[idx + 1];
                    if (target.type == TokenType::Arg || target.type == TokenType::Command) {
                        if (token.value == "<") {
                            current_cmd.input_redirect = target.value;
                        } else {
                            current_cmd.output_redirect = target.value;
                            current_cmd.append_output = (token.value == ">>");
                        }
                        ++idx;
                    }
                }
                break;
            case TokenType::Background:
                pipeline.background = true;
                break;
            default:
                break;
        }
    }


    if (!current_cmd.command.empty()) {
        pipeline.commands.push_back(std::move(current_cmd));
    }

    return pipeline;
}


[[nodiscard]] inline std::optional<HeredocInfo> detect_heredoc(std::string_view command) {
    auto pos = command.find("<<");
    if (pos == std::string_view::npos) return std::nullopt;

    HeredocInfo info;
    size_t delim_start = pos + 2;


    if (delim_start < command.size() && command[delim_start] == '-') {
        info.strip_tabs = true;
        ++delim_start;
    }


    while (delim_start < command.size() &&
           std::isspace(static_cast<unsigned char>(command[delim_start]))) {
        ++delim_start;
    }

    size_t delim_end = delim_start;
    while (delim_end < command.size() &&
           !std::isspace(static_cast<unsigned char>(command[delim_end]))) {
        ++delim_end;
    }

    if (delim_start == delim_end) return std::nullopt;

    info.delimiter = std::string(command.substr(delim_start, delim_end - delim_start));

    if (info.delimiter.size() >= 2 &&
        ((info.delimiter.front() == '\'' && info.delimiter.back() == '\'') ||
         (info.delimiter.front() == '"' && info.delimiter.back() == '"'))) {
        info.delimiter = info.delimiter.substr(1, info.delimiter.size() - 2);
    }

    return info;
}


[[nodiscard]] inline std::vector<Warning> detect_dangerous_patterns(std::string_view command) {
    std::vector<Warning> warnings;


    static const std::vector<std::pair<std::string, std::string>> patterns = {
        {"rm -rf /",    "Recursive forced removal of root filesystem"},
        {"rm -rf /*",   "Recursive forced removal of all root entries"},
        {":(){:|:&};:", "Fork bomb detected"},
        {"mkfs",        "Filesystem format command"},
        {"dd if=",      "Direct disk write - potential data loss"},
        {"> /dev/sd",   "Direct write to block device"},
        {"chmod 777 /", "Insecure permissions on root"},
        {"curl|sh",     "Piping remote content to shell"},
        {"wget|sh",     "Piping remote content to shell"},
    };

    for (const auto& [pattern, message] : patterns) {
        auto pos = command.find(pattern);
        if (pos != std::string_view::npos) {
            warnings.push_back({message, pattern, pos});
        }
    }

    return warnings;
}


[[nodiscard]] inline std::vector<std::filesystem::path> extract_file_paths(std::string_view command) {
    std::vector<std::filesystem::path> paths;
    auto tokens = tokenize(command);

    for (const auto& token : tokens) {
        if (token.type != TokenType::Arg) continue;

        if (token.value.starts_with('/') || token.value.starts_with("./") ||
            token.value.starts_with("~/") || token.value.starts_with("../")) {
            paths.emplace_back(token.value);
        }

        else if (token.value.find('/') != std::string::npos &&
                 !token.value.starts_with('-')) {
            paths.emplace_back(token.value);
        }
    }

    return paths;
}


[[nodiscard]] inline bool is_read_only(std::string_view command) {
    auto tokens = tokenize(command);
    if (tokens.empty()) return true;


    static constexpr std::array safe_commands = {
        "ls", "cat", "head", "tail", "grep", "awk", "sed",
        "wc", "sort", "uniq", "diff", "find", "which",
        "whoami", "pwd", "echo", "date", "env", "printenv",
        "file", "stat", "du", "df", "uname", "id", "ps"
    };


    const auto& first_cmd = tokens.front().value;
    bool first_is_safe = std::ranges::any_of(safe_commands, [&](auto cmd) {
        return first_cmd == cmd;
    });

    if (!first_is_safe) return false;


    bool has_write_redirect = std::ranges::any_of(tokens, [](const auto& t) {
        return t.type == TokenType::Redirect && (t.value == ">" || t.value == ">>");
    });

    return !has_write_redirect;
}

} // namespace cc::utils::shell_parser
