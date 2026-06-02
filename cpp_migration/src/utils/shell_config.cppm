module;

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>
#include <cstdlib>
#include <algorithm>

export module cc.utils.shell_config;

export namespace cc::utils {

namespace fs = std::filesystem;

// Supported shell types
enum class ShellType {
    Bash,
    Zsh,
    Fish,
    PowerShell,
    Cmd,
    Unknown
};

// Detect the current user's shell from environment
inline ShellType detect_shell() {
    const char* shell_env = std::getenv("SHELL");
    if (!shell_env) return ShellType::Unknown;

    std::string shell(shell_env);
    if (shell.find("zsh") != std::string::npos) return ShellType::Zsh;
    if (shell.find("bash") != std::string::npos) return ShellType::Bash;
    if (shell.find("fish") != std::string::npos) return ShellType::Fish;
    if (shell.find("pwsh") != std::string::npos || shell.find("powershell") != std::string::npos)
        return ShellType::PowerShell;
    return ShellType::Unknown;
}

// Get the path to the user's default shell
inline fs::path get_shell_path() {
    const char* shell_env = std::getenv("SHELL");
    if (shell_env) return fs::path(shell_env);
    return fs::path("/bin/sh");
}

// Get the RC/config file path for a given shell type
inline std::optional<fs::path> get_shell_rc_file(ShellType type) {
    const char* home = std::getenv("HOME");
    if (!home) return std::nullopt;

    fs::path home_dir(home);
    switch (type) {
        case ShellType::Bash: {
            // Prefer .bashrc, fallback to .bash_profile
            auto bashrc = home_dir / ".bashrc";
            if (fs::exists(bashrc)) return bashrc;
            return home_dir / ".bash_profile";
        }
        case ShellType::Zsh:
            return home_dir / ".zshrc";
        case ShellType::Fish:
            return home_dir / ".config" / "fish" / "config.fish";
        case ShellType::PowerShell:
            return home_dir / ".config" / "powershell" / "Microsoft.PowerShell_profile.ps1";
        default:
            return std::nullopt;
    }
}

// Escape a string for safe use in a shell command
inline std::string shell_escape(std::string_view input, ShellType type = detect_shell()) {
    std::string result;
    result.reserve(input.size() + 10);

    switch (type) {
        case ShellType::PowerShell:
            // PowerShell uses single quotes with doubled single quotes for escaping
            result += '\'';
            for (char c : input) {
                if (c == '\'') result += "''";
                else result += c;
            }
            result += '\'';
            break;

        case ShellType::Fish:
            // Fish uses single quotes, backslash-escapes single quotes
            result += '\'';
            for (char c : input) {
                if (c == '\'') result += "\\'";
                else if (c == '\\') result += "\\\\";
                else result += c;
            }
            result += '\'';
            break;

        default:
            // POSIX shells (bash, zsh): single-quote the string
            // Replace internal single quotes with '\''
            result += '\'';
            for (char c : input) {
                if (c == '\'') result += "'\\''";
                else result += c;
            }
            result += '\'';
            break;
    }

    return result;
}

// Get the shell-specific prompt command (used for tracking CWD changes)
inline std::string get_shell_prompt_command(ShellType type) {
    switch (type) {
        case ShellType::Bash:
            return R"cmd(PROMPT_COMMAND='echo "CWD:$(pwd)"')cmd";
        case ShellType::Zsh:
            return R"cmd(precmd() { echo "CWD:$(pwd)" })cmd";
        case ShellType::Fish:
            return R"cmd(function fish_prompt; echo "CWD:$(pwd)"; end)cmd";
        case ShellType::PowerShell:
            return R"cmd(function prompt { "CWD:$(Get-Location)`n" })cmd";
        default:
            return R"cmd(PS1='CWD:$(pwd)\n$ ')cmd";
    }
}

} // namespace cc::utils
