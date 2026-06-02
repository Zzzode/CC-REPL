// C++23 Module: Shell command parsing
// Shell 命令解析器，负责 tokenize、管道解析和危险模式检测
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

// Shell Token 类型枚举
enum class TokenType : uint8_t {
    Command,    // 命令名
    Arg,        // 参数
    Pipe,       // |
    Redirect,   // > >> < 2>
    Semicolon,  // ;
    And,        // &&
    Or,         // ||
    Background, // &
    Subshell    // $() 或反引号
};

// 单个 Token 结构
struct ShellToken {
    TokenType type;
    std::string value;
    size_t position{0};  // 在原始字符串中的位置

    [[nodiscard]] bool is_operator() const {
        return type == TokenType::Pipe || type == TokenType::Redirect ||
               type == TokenType::Semicolon || type == TokenType::And ||
               type == TokenType::Or || type == TokenType::Background;
    }
};

// Heredoc 信息
struct HeredocInfo {
    std::string delimiter;    // heredoc 终止符
    std::string content;      // heredoc 内容
    bool strip_tabs{false};   // 是否使用 <<- 形式
};

// 管道中的单个命令
struct PipelineCommand {
    std::string command;
    std::vector<std::string> args;
    std::optional<std::string> input_redirect;
    std::optional<std::string> output_redirect;
    bool append_output{false};  // >> vs >
};

// 管道结构
struct Pipeline {
    std::vector<PipelineCommand> commands;
    bool background{false};  // 是否后台执行 (&)

    [[nodiscard]] size_t stage_count() const { return commands.size(); }
    [[nodiscard]] bool is_simple() const { return commands.size() == 1; }
};

// 警告信息
struct Warning {
    std::string message;
    std::string pattern_matched;
    size_t position{0};
};

// 对命令字符串进行 tokenize
[[nodiscard]] inline std::vector<ShellToken> tokenize(std::string_view command) {
    std::vector<ShellToken> tokens;
    size_t i = 0;
    const size_t len = command.size();

    while (i < len) {
        // 跳过空白
        if (std::isspace(static_cast<unsigned char>(command[i]))) { ++i; continue; }

        size_t start = i;

        // 检查双字符操作符
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

        // 单字符操作符
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

        // 子 shell: $(...) 形式
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

        // 带引号的字符串
        if (command[i] == '"' || command[i] == '\'') {
            char quote = command[i];
            size_t j = i + 1;
            while (j < len && command[j] != quote) {
                if (command[j] == '\\' && j + 1 < len) ++j;  // 跳过转义
                ++j;
            }
            if (j < len) ++j;  // 跳过结束引号
            auto value = std::string(command.substr(i + 1, j - i - 2));
            auto type = tokens.empty() ? TokenType::Command : TokenType::Arg;
            tokens.push_back({type, std::move(value), start});
            i = j; continue;
        }

        // 普通词 (命令或参数)
        size_t j = i;
        while (j < len && !std::isspace(static_cast<unsigned char>(command[j])) &&
               command[j] != '|' && command[j] != ';' && command[j] != '>' &&
               command[j] != '<' && command[j] != '&') {
            ++j;
        }

        auto value = std::string(command.substr(i, j - i));
        // 第一个 token 或紧跟操作符之后的 token 为 Command
        bool is_command = tokens.empty() || tokens.back().is_operator();
        tokens.push_back({is_command ? TokenType::Command : TokenType::Arg, std::move(value), start});
        i = j;
    }

    return tokens;
}

// 解析 tokens 为 Pipeline 结构
[[nodiscard]] inline Pipeline parse_pipeline(const std::vector<ShellToken>& tokens) {
    Pipeline pipeline;
    PipelineCommand current_cmd;
    bool expect_command = true;

    for (std::size_t idx = 0; idx < tokens.size(); ++idx) {
        const auto& token = tokens[idx];
        switch (token.type) {
            case TokenType::Command:
                if (!expect_command && !current_cmd.command.empty()) {
                    // 管道分隔，保存当前命令
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

    // 保存最后一个命令
    if (!current_cmd.command.empty()) {
        pipeline.commands.push_back(std::move(current_cmd));
    }

    return pipeline;
}

// 检测 heredoc 语法
[[nodiscard]] inline std::optional<HeredocInfo> detect_heredoc(std::string_view command) {
    auto pos = command.find("<<");
    if (pos == std::string_view::npos) return std::nullopt;

    HeredocInfo info;
    size_t delim_start = pos + 2;

    // 检查是否为 <<- 形式
    if (delim_start < command.size() && command[delim_start] == '-') {
        info.strip_tabs = true;
        ++delim_start;
    }

    // 跳过空白获取 delimiter
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
    // 去除引号
    if (info.delimiter.size() >= 2 &&
        ((info.delimiter.front() == '\'' && info.delimiter.back() == '\'') ||
         (info.delimiter.front() == '"' && info.delimiter.back() == '"'))) {
        info.delimiter = info.delimiter.substr(1, info.delimiter.size() - 2);
    }

    return info;
}

// 检测危险模式
[[nodiscard]] inline std::vector<Warning> detect_dangerous_patterns(std::string_view command) {
    std::vector<Warning> warnings;

    // 危险模式定义
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

// 从命令中提取文件路径
[[nodiscard]] inline std::vector<std::filesystem::path> extract_file_paths(std::string_view command) {
    std::vector<std::filesystem::path> paths;
    auto tokens = tokenize(command);

    for (const auto& token : tokens) {
        if (token.type != TokenType::Arg) continue;
        // 简单启发式：以 / 或 ./ 或 ~/ 开头的参数视为路径
        if (token.value.starts_with('/') || token.value.starts_with("./") ||
            token.value.starts_with("~/") || token.value.starts_with("../")) {
            paths.emplace_back(token.value);
        }
        // 包含路径分隔符的参数也可能是路径
        else if (token.value.find('/') != std::string::npos &&
                 !token.value.starts_with('-')) {
            paths.emplace_back(token.value);
        }
    }

    return paths;
}

// 判断命令是否为只读操作
[[nodiscard]] inline bool is_read_only(std::string_view command) {
    auto tokens = tokenize(command);
    if (tokens.empty()) return true;

    // 只读命令白名单
    static constexpr std::array safe_commands = {
        "ls", "cat", "head", "tail", "grep", "awk", "sed",
        "wc", "sort", "uniq", "diff", "find", "which",
        "whoami", "pwd", "echo", "date", "env", "printenv",
        "file", "stat", "du", "df", "uname", "id", "ps"
    };

    // 获取管道中第一个命令
    const auto& first_cmd = tokens.front().value;
    bool first_is_safe = std::ranges::any_of(safe_commands, [&](auto cmd) {
        return first_cmd == cmd;
    });

    if (!first_is_safe) return false;

    // 检查是否有输出重定向 (意味着写入文件)
    bool has_write_redirect = std::ranges::any_of(tokens, [](const auto& t) {
        return t.type == TokenType::Redirect && (t.value == ">" || t.value == ">>");
    });

    return !has_write_redirect;
}

} // namespace cc::utils::shell_parser
