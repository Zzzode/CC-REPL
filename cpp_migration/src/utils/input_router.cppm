// C++23 Module: User input processing/routing
// User input classification and routing
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.input_router;


export namespace cc::utils::input_router {

// Input type enumeration
enum class InputType : uint8_t {
    SlashCommand,
    BashCommand,
    Prompt,
    Empty,
    File
};

// Parsed slash command
struct ParsedCommand {
    std::string name;
    std::vector<std::string> args;
    std::string raw;

    [[nodiscard]] bool has_args() const { return !args.empty(); }
    [[nodiscard]] std::string args_joined() const {
        std::string result;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) result += ' ';
            result += args[i];
        }
        return result;
    }
};

// --- Helper functions (must be defined before callers) ---

// Trim leading/trailing whitespace
inline std::string_view trim(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) return "";
    auto end = sv.find_last_not_of(" \t\n\r");
    return sv.substr(start, end - start + 1);
}

// Check if character is valid in file paths
inline bool is_path_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) ||
           c == '/' || c == '.' || c == '_' || c == '-' || c == '~';
}

// Split arguments by spaces (supports quotes)
inline std::vector<std::string> split_args(std::string_view sv) {
    std::vector<std::string> args;
    size_t i = 0;
    while (i < sv.size()) {
        while (i < sv.size() && sv[i] == ' ') ++i;
        if (i >= sv.size()) break;

        std::string arg;
        if (sv[i] == '"' || sv[i] == '\'') {
            char quote = sv[i++];
            while (i < sv.size() && sv[i] != quote) {
                if (sv[i] == '\\' && i + 1 < sv.size()) { arg += sv[++i]; }
                else { arg += sv[i]; }
                ++i;
            }
            if (i < sv.size()) ++i;
        } else {
            while (i < sv.size() && sv[i] != ' ') { arg += sv[i++]; }
        }
        if (!arg.empty()) args.push_back(std::move(arg));
    }
    return args;
}

// Check if text looks like a file path
inline bool looks_like_file_path(std::string_view text) {
    if (text.empty()) return false;
    if (text.starts_with('/') && text.find(' ') == std::string_view::npos) return true;
    if (text.starts_with("./") && text.find(' ') == std::string_view::npos) return true;
    if (text.starts_with("~/") && text.find(' ') == std::string_view::npos) return true;
    if (text.find('.') != std::string_view::npos &&
        text.find(' ') == std::string_view::npos &&
        text.size() < 256) {
        auto dot_pos = text.rfind('.');
        auto ext = text.substr(dot_pos);
        static constexpr std::array<std::string_view, 20> common_exts = {
            ".cpp", ".hpp", ".h", ".c", ".py", ".js", ".ts",
            ".rs", ".go", ".java", ".md", ".txt", ".json", ".yaml",
            ".toml", ".xml", ".html", ".css", ".sh", ".bash"
        };
        return std::ranges::any_of(common_exts, [&](std::string_view e) { return ext == e; });
    }
    return false;
}

// Detect if text looks like a shell command
[[nodiscard]] inline bool detect_bash_intent(std::string_view text) {
    auto trimmed = trim(text);
    if (trimmed.empty()) return false;

    static constexpr std::array<std::string_view, 53> shell_prefixes = {
        "ls ", "cd ", "cat ", "grep ", "find ", "mkdir ", "rm ",
        "cp ", "mv ", "touch ", "chmod ", "chown ", "echo ",
        "git ", "docker ", "npm ", "yarn ", "pip ", "cargo ",
        "make ", "cmake ", "gcc ", "g++ ", "clang ", "python ",
        "node ", "ruby ", "go ", "rustc ", "java ", "javac ",
        "apt ", "brew ", "dnf ", "yum ", "pacman ",
        "ssh ", "scp ", "rsync ", "curl ", "wget ",
        "tar ", "zip ", "unzip ", "gzip ",
        "ps ", "kill ", "top ", "htop ",
        "sudo ", "su ", "env ", "export "
    };

    static constexpr std::array<std::string_view, 10> shell_exact = {
        "ls", "pwd", "whoami", "date", "uptime", "clear",
        "top", "htop", "exit", "logout"
    };

    bool has_prefix = std::ranges::any_of(shell_prefixes, [&](std::string_view prefix) {
        return trimmed.starts_with(prefix);
    });
    if (has_prefix) return true;

    bool exact_match = std::ranges::any_of(shell_exact, [&](std::string_view cmd) {
        return trimmed == cmd;
    });
    if (exact_match) return true;

    if (trimmed.starts_with("./")) return true;
    if (trimmed.find(" | ") != std::string_view::npos) return true;
    if (trimmed.find(" > ") != std::string_view::npos ||
        trimmed.find(" >> ") != std::string_view::npos) return true;

    return false;
}

// --- Public API functions ---

// Classify user input
[[nodiscard]] inline InputType classify_input(std::string_view text) {
    auto trimmed = trim(text);

    if (trimmed.empty()) return InputType::Empty;
    if (trimmed.starts_with('/')) return InputType::SlashCommand;
    if (looks_like_file_path(trimmed)) return InputType::File;
    if (detect_bash_intent(trimmed)) return InputType::BashCommand;

    return InputType::Prompt;
}

// Parse slash command
[[nodiscard]] inline ParsedCommand parse_slash_command(std::string_view text) {
    ParsedCommand cmd;
    cmd.raw = std::string(text);

    auto trimmed = trim(text);
    if (!trimmed.starts_with('/')) return cmd;

    auto content = trimmed.substr(1);

    auto first_space = content.find(' ');
    if (first_space == std::string_view::npos) {
        cmd.name = std::string(content);
    } else {
        cmd.name = std::string(content.substr(0, first_space));
        auto args_str = content.substr(first_space + 1);
        cmd.args = split_args(args_str);
    }

    std::ranges::transform(cmd.name, cmd.name.begin(), [](unsigned char c) {
        return std::tolower(c);
    });

    return cmd;
}

// Extract file path references from text
[[nodiscard]] inline std::vector<std::filesystem::path> extract_file_references(
    std::string_view text) {
    std::vector<std::filesystem::path> paths;

    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '/' || (text[i] == '.' && i + 1 < text.size() && text[i + 1] == '/') ||
            (text[i] == '~' && i + 1 < text.size() && text[i + 1] == '/')) {
            size_t start = i;
            while (i < text.size() && is_path_char(text[i])) ++i;
            auto path_str = text.substr(start, i - start);
            if (path_str.size() > 2 && path_str.find('/') != std::string_view::npos) {
                paths.emplace_back(path_str);
            }
        } else {
            ++i;
        }
    }

    return paths;
}

// Preprocess prompt text (collapse whitespace)
[[nodiscard]] inline std::string preprocess_prompt(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    auto trimmed = trim(text);

    bool prev_space = false;
    for (char c : trimmed) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space) {
                result += ' ';
                prev_space = true;
            }
        } else {
            result += c;
            prev_space = false;
        }
    }

    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }

    return result;
}

} // namespace cc::utils::input_router
