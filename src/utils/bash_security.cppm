// C++23 Bash Security Module
// Provides utilities for command safety checking, sandboxing, and deny-listing
// Migrates: bashDangerous.ts, bashSafetyCheck.ts, bashSandbox.ts,
//           commandAnalyzer.ts, commandDenylist.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <algorithm>
#include <array>
#include <regex>

export module cc.utils.bash_security;

export namespace cc::utils::bash {

/// How dangerous a command is assessed to be
enum class DangerLevel {
    Safe,
    Caution,
    Dangerous,
    Forbidden
};

/// Category of a command based on what system resources it interacts with
enum class CommandCategory {
    FileSystem,
    Network,
    Process,
    System,
    Package,
    Git,
    Docker,
    Database,
    Crypto,
    Unknown
};

/// Result of a safety check on a command
struct SafetyCheckResult {
    DangerLevel level;
    CommandCategory category;
    std::string explanation;
    std::vector<std::string> risks;
    std::optional<std::string> safe_alternative;
};

/// An entry in the command denylist
struct DenylistEntry {
    std::string pattern;
    std::string reason;
    bool is_regex{false};
};

/// Configuration for a sandboxed execution environment
struct SandboxConfig {
    std::vector<std::string> allowed_paths;
    std::vector<std::string> denied_commands;
    bool allow_network{false};
    bool allow_env_modification{false};
    std::size_t max_output_bytes{1048576};
    std::chrono::seconds max_duration{60};
};

[[nodiscard]] inline auto is_command_denied(std::string_view command) -> std::optional<std::string>;
[[nodiscard]] inline auto get_command_category(std::string_view command) -> CommandCategory;
[[nodiscard]] inline auto get_default_denylist() -> std::vector<DenylistEntry>;
[[nodiscard]] inline auto suggest_safe_alternative(std::string_view dangerous_command) -> std::optional<std::string>;
[[nodiscard]] inline auto extract_file_paths_from_command(std::string_view command) -> std::vector<std::string>;

namespace detail {

// Extract the base command (first token) from a command string
inline std::string extract_base_command(std::string_view command) {
    // Skip leading whitespace
    auto start = command.find_first_not_of(" \t");
    if (start == std::string_view::npos) return "";

    // Skip environment variable assignments (e.g., "VAR=val cmd")
    auto cmd_view = command.substr(start);
    while (!cmd_view.empty()) {
        auto space_pos = cmd_view.find(' ');
        auto eq_pos = cmd_view.find('=');
        if (eq_pos != std::string_view::npos && (space_pos == std::string_view::npos || eq_pos < space_pos)) {
            if (space_pos == std::string_view::npos) return "";
            cmd_view = cmd_view.substr(space_pos + 1);
            auto next_start = cmd_view.find_first_not_of(" \t");
            if (next_start == std::string_view::npos) return "";
            cmd_view = cmd_view.substr(next_start);
        } else {
            break;
        }
    }

    // Handle sudo/env prefix
    if (cmd_view.starts_with("sudo ")) {
        cmd_view = cmd_view.substr(5);
        auto next = cmd_view.find_first_not_of(" \t");
        if (next != std::string_view::npos) cmd_view = cmd_view.substr(next);
    }

    // Get first token
    auto end = cmd_view.find_first_of(" \t|;&><(");
    if (end == std::string_view::npos) end = cmd_view.size();

    std::string base(cmd_view.substr(0, end));
    // Strip path prefix
    auto slash = base.rfind('/');
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    return base;
}

// Check if command involves network operations
inline bool involves_network(std::string_view command) {
    static constexpr std::array network_cmds = {
        "curl", "wget", "nc", "ncat", "netcat", "ssh", "scp", "sftp",
        "rsync", "ftp", "telnet", "nmap", "ping", "traceroute",
        "dig", "nslookup", "host", "ifconfig", "ip"
    };
    std::string base = extract_base_command(command);
    for (auto cmd : network_cmds) {
        if (base == cmd) return true;
    }
    return false;
}

// Check if command involves process management
inline bool involves_process(std::string_view command) {
    static constexpr std::array process_cmds = {
        "kill", "killall", "pkill", "ps", "top", "htop",
        "nice", "renice", "nohup", "disown", "bg", "fg"
    };
    std::string base = extract_base_command(command);
    for (auto cmd : process_cmds) {
        if (base == cmd) return true;
    }
    return false;
}

// Check if command involves system-level operations
inline bool involves_system(std::string_view command) {
    static constexpr std::array system_cmds = {
        "shutdown", "reboot", "halt", "poweroff", "systemctl",
        "service", "init", "mount", "umount", "fdisk", "mkfs",
        "sysctl", "modprobe", "insmod", "rmmod", "dmesg"
    };
    std::string base = extract_base_command(command);
    for (auto cmd : system_cmds) {
        if (base == cmd) return true;
    }
    return false;
}

} // namespace detail

/// Perform a comprehensive safety check on a command
[[nodiscard]] inline auto check_command_safety(
    std::string_view command
) -> SafetyCheckResult {
    SafetyCheckResult result;
    result.category = get_command_category(command);
    result.level = DangerLevel::Safe;

    std::string cmd_str(command);

    // Check denylist first
    auto denied = is_command_denied(command);
    if (denied) {
        result.level = DangerLevel::Forbidden;
        result.explanation = "Command matches denylist: " + *denied;
        result.risks.push_back(*denied);
        return result;
    }

    // Check for dangerous patterns
    std::vector<std::pair<std::string_view, std::string>> danger_patterns = {
        {"rm -rf /", "Recursive deletion of root filesystem"},
        {"rm -rf /*", "Recursive deletion of all root entries"},
        {"> /dev/sd", "Direct write to disk device"},
        {"dd if=/dev/zero of=/dev/sd", "Overwriting disk with zeros"},
        {"mkfs", "Formatting a filesystem"},
        {"chmod -R 777 /", "Removing all permission restrictions on root"},
        {":(){ :|:& };:", "Fork bomb - will exhaust system resources"},
    };

    for (auto& [pattern, risk] : danger_patterns) {
        if (cmd_str.find(pattern) != std::string::npos) {
            result.level = DangerLevel::Dangerous;
            result.risks.push_back(std::string(risk));
        }
    }

    if (result.level == DangerLevel::Dangerous) {
        result.explanation = "Command contains dangerous operations";
        result.safe_alternative = suggest_safe_alternative(command);
        return result;
    }

    // Check for caution-level patterns
    std::vector<std::pair<std::string_view, std::string>> caution_patterns = {
        {"rm -rf", "Recursive forced deletion"},
        {"rm -r", "Recursive deletion"},
        {"sudo", "Elevated privileges"},
        {"chmod", "Permission modification"},
        {"chown", "Ownership change"},
        {"mv /", "Moving root-level files"},
        {"cp -r /", "Copying from root"},
        {"|sh", "Piping to shell execution"},
        {"|bash", "Piping to bash execution"},
        {"eval ", "Dynamic code evaluation"},
        {"exec ", "Process replacement"},
        {"> /etc/", "Writing to system config"},
        {"curl.*|", "Downloading and piping to another command"},
        {"wget.*|", "Downloading and piping to another command"},
    };

    for (auto& [pattern, risk] : caution_patterns) {
        if (cmd_str.find(pattern) != std::string::npos) {
            if (result.level < DangerLevel::Caution) {
                result.level = DangerLevel::Caution;
            }
            result.risks.push_back(std::string(risk));
        }
    }

    if (result.level == DangerLevel::Caution) {
        result.explanation = "Command requires caution - review before executing";
    } else {
        result.explanation = "Command appears safe";
    }

    return result;
}

/// Check if a command is on the denylist; returns the reason if denied
[[nodiscard]] inline auto is_command_denied(
    std::string_view command
) -> std::optional<std::string> {
    auto denylist = get_default_denylist();
    std::string cmd_str(command);

    for (const auto& entry : denylist) {
        if (entry.is_regex) {
            try {
                std::regex re(entry.pattern, std::regex::icase);
                if (std::regex_search(cmd_str, re)) {
                    return entry.reason;
                }
            } catch (...) {
                // Invalid regex, skip
            }
        } else {
            if (cmd_str.find(entry.pattern) != std::string::npos) {
                return entry.reason;
            }
        }
    }

    return std::nullopt;
}

/// Determine the category of a command
[[nodiscard]] inline auto get_command_category(
    std::string_view command
) -> CommandCategory {
    std::string base = detail::extract_base_command(command);

    // Git commands
    if (base == "git") return CommandCategory::Git;

    // Docker commands
    if (base == "docker" || base == "docker-compose" || base == "podman") {
        return CommandCategory::Docker;
    }

    // Package managers
    static constexpr std::array pkg_cmds = {
        "npm", "yarn", "pnpm", "pip", "pip3", "gem", "cargo",
        "apt", "apt-get", "yum", "dnf", "brew", "pacman", "apk"
    };
    for (auto cmd : pkg_cmds) {
        if (base == cmd) return CommandCategory::Package;
    }

    // Database
    static constexpr std::array db_cmds = {
        "psql", "mysql", "sqlite3", "mongosh", "redis-cli", "pg_dump"
    };
    for (auto cmd : db_cmds) {
        if (base == cmd) return CommandCategory::Database;
    }

    // Crypto
    static constexpr std::array crypto_cmds = {
        "openssl", "gpg", "ssh-keygen", "age", "certbot"
    };
    for (auto cmd : crypto_cmds) {
        if (base == cmd) return CommandCategory::Crypto;
    }

    // Network
    if (detail::involves_network(command)) return CommandCategory::Network;

    // Process
    if (detail::involves_process(command)) return CommandCategory::Process;

    // System
    if (detail::involves_system(command)) return CommandCategory::System;

    // Filesystem (default for most file operations)
    static constexpr std::array fs_cmds = {
        "ls", "cat", "cp", "mv", "rm", "mkdir", "rmdir", "touch",
        "chmod", "chown", "find", "grep", "sed", "awk", "head",
        "tail", "wc", "sort", "uniq", "tar", "zip", "unzip",
        "ln", "stat", "du", "df", "file"
    };
    for (auto cmd : fs_cmds) {
        if (base == cmd) return CommandCategory::FileSystem;
    }

    return CommandCategory::Unknown;
}

/// Get the danger level of a command
[[nodiscard]] inline auto get_danger_level(
    std::string_view command
) -> DangerLevel {
    // Quick denylist check
    if (is_command_denied(command)) return DangerLevel::Forbidden;

    std::string cmd_str(command);

    // Forbidden patterns
    static constexpr std::array forbidden_patterns = {
        ":(){ :|:& };:",
        "rm -rf /",
        "rm -rf /*",
        "dd if=/dev/zero of=/dev/sd",
        "mkfs.",
        "> /dev/sd",
    };
    for (auto p : forbidden_patterns) {
        if (cmd_str.find(p) != std::string::npos) return DangerLevel::Forbidden;
    }

    // Dangerous patterns
    static constexpr std::array dangerous_patterns = {
        "rm -rf",
        "chmod -R 777",
        "dd if=",
        "format ",
        "fdisk",
    };
    for (auto p : dangerous_patterns) {
        if (cmd_str.find(p) != std::string::npos) return DangerLevel::Dangerous;
    }

    // Caution patterns
    static constexpr std::array caution_patterns = {
        "sudo",
        "rm -r",
        "chmod",
        "chown",
        "mv /",
        "|sh",
        "|bash",
        "eval ",
        "exec ",
        "> /etc/",
    };
    for (auto p : caution_patterns) {
        if (cmd_str.find(p) != std::string::npos) return DangerLevel::Caution;
    }

    return DangerLevel::Safe;
}

/// Get the default list of denied command patterns
[[nodiscard]] inline auto get_default_denylist() -> std::vector<DenylistEntry> {
    return {
        {"rm -rf /", "Recursive deletion of root filesystem", false},
        {"rm -rf /*", "Recursive deletion of root entries", false},
        {"mkfs", "Filesystem formatting", false},
        {"dd if=/dev/zero", "Disk overwrite with zeros", false},
        {"dd if=/dev/random", "Disk overwrite with random data", false},
        {":(){ :|:& };:", "Fork bomb", false},
        {"chmod -R 777 /", "Removing all file permission restrictions", false},
        {"> /dev/sda", "Direct overwrite of disk device", false},
        {"mv /* /dev/null", "Moving all files to /dev/null", false},
        {"shutdown", "System shutdown", false},
        {"reboot", "System reboot", false},
        {"halt", "System halt", false},
        {"init 0", "System poweroff via init", false},
        {"init 6", "System reboot via init", false},
    };
}

/// Validate whether a command is allowed within a sandbox configuration
[[nodiscard]] inline auto validate_sandbox_command(
    std::string_view command,
    const SandboxConfig& config
) -> std::expected<void, std::string> {
    std::string cmd_str(command);
    std::string base = detail::extract_base_command(command);

    // Check denied commands list
    for (const auto& denied : config.denied_commands) {
        if (base == denied || cmd_str.find(denied) != std::string::npos) {
            return std::unexpected("Command '" + denied + "' is not allowed in sandbox");
        }
    }

    // Check network access
    if (!config.allow_network && detail::involves_network(command)) {
        return std::unexpected("Network access is not allowed in sandbox");
    }

    // Check environment modification
    if (!config.allow_env_modification) {
        if (cmd_str.find("export ") != std::string::npos ||
            cmd_str.find("unset ") != std::string::npos ||
            cmd_str.find("source ") != std::string::npos ||
            cmd_str.find(". /") != std::string::npos) {
            return std::unexpected("Environment modification is not allowed in sandbox");
        }
    }

    // Check file path restrictions
    if (!config.allowed_paths.empty()) {
        auto paths = extract_file_paths_from_command(command);
        for (const auto& path : paths) {
            bool allowed = false;
            for (const auto& allowed_path : config.allowed_paths) {
                if (path.starts_with(allowed_path) || path == "." || path == "..") {
                    allowed = true;
                    break;
                }
            }
            if (!allowed && path.starts_with("/")) {
                return std::unexpected("Access to path '" + path + "' is not allowed in sandbox");
            }
        }
    }

    return {};
}

/// Suggest a safer alternative to a dangerous command
[[nodiscard]] inline auto suggest_safe_alternative(
    std::string_view dangerous_command
) -> std::optional<std::string> {
    std::string cmd(dangerous_command);

    // rm -rf suggestions
    if (cmd.find("rm -rf /") != std::string::npos && cmd.find("rm -rf /*") == std::string::npos) {
        return "Use 'rm -rf ./' to delete contents of current directory instead";
    }
    if (cmd.find("rm -rf") != std::string::npos) {
        return "Use 'rm -ri' for interactive deletion, or specify exact paths";
    }
    if (cmd.find("rm -r") != std::string::npos) {
        return "Use 'rm -ri' for interactive confirmation before each removal";
    }

    // chmod suggestions
    if (cmd.find("chmod -R 777") != std::string::npos) {
        return "Use specific permissions (e.g., 'chmod -R 755' for dirs, 'chmod -R 644' for files)";
    }
    if (cmd.find("chmod 777") != std::string::npos) {
        return "Use 'chmod 755' for executables or 'chmod 644' for regular files";
    }

    // curl/wget pipe suggestions
    if ((cmd.find("curl") != std::string::npos || cmd.find("wget") != std::string::npos) &&
        (cmd.find("|sh") != std::string::npos || cmd.find("|bash") != std::string::npos)) {
        return "Download the script first, review it, then execute: 'curl -o script.sh URL && cat script.sh && bash script.sh'";
    }

    // dd suggestions
    if (cmd.find("dd if=/dev/zero") != std::string::npos || cmd.find("dd if=/dev/random") != std::string::npos) {
        return "Verify the output device (of=) carefully. Consider using 'shred' for secure deletion";
    }

    // sudo suggestions
    if (cmd.find("sudo rm") != std::string::npos) {
        return "Consider running without sudo first; add sudo only if permission denied";
    }

    return std::nullopt;
}

/// Extract file paths referenced in a command
[[nodiscard]] inline auto extract_file_paths_from_command(
    std::string_view command
) -> std::vector<std::string> {
    std::vector<std::string> paths;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    // Simple tokenizer - extract tokens that look like paths
    std::vector<std::string> tokens;
    std::string token;

    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (escaped) { token += c; escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '\'' && !in_double_quote) { in_single_quote = !in_single_quote; continue; }
        if (c == '"' && !in_single_quote) { in_double_quote = !in_double_quote; continue; }

        if ((c == ' ' || c == '\t') && !in_single_quote && !in_double_quote) {
            if (!token.empty()) { tokens.push_back(std::move(token)); token.clear(); }
        } else {
            token += c;
        }
    }
    if (!token.empty()) tokens.push_back(std::move(token));

    // Filter tokens that look like file paths
    for (const auto& t : tokens) {
        if (t.empty()) continue;
        // Skip flags
        if (t.starts_with("-")) continue;
        // Skip known commands
        if (t == "sudo" || t == "env") continue;

        // It's a path if it starts with /, ./, ../, ~/ or contains /
        if (t.starts_with("/") || t.starts_with("./") || t.starts_with("../") ||
            t.starts_with("~/") || t.find('/') != std::string::npos) {
            paths.push_back(t);
        }
        // Also consider tokens with file extensions
        else if (t.find('.') != std::string::npos && t.size() > 2) {
            auto dot_pos = t.rfind('.');
            auto ext = t.substr(dot_pos);
            if (ext.size() <= 5) { // reasonable extension length
                paths.push_back(t);
            }
        }
    }

    return paths;
}

/// Get a human-readable label for a command category
[[nodiscard]] inline constexpr auto get_category_label(
    CommandCategory category
) -> std::string_view {
    switch (category) {
        case CommandCategory::FileSystem: return "File System";
        case CommandCategory::Network:    return "Network";
        case CommandCategory::Process:    return "Process";
        case CommandCategory::System:     return "System";
        case CommandCategory::Package:    return "Package";
        case CommandCategory::Git:        return "Git";
        case CommandCategory::Docker:     return "Docker";
        case CommandCategory::Database:   return "Database";
        case CommandCategory::Crypto:     return "Crypto";
        case CommandCategory::Unknown:    return "Unknown";
    }
    return "Unknown";
}

/// Get a human-readable label for a danger level
[[nodiscard]] inline constexpr auto get_danger_label(
    DangerLevel level
) -> std::string_view {
    switch (level) {
        case DangerLevel::Safe:      return "Safe";
        case DangerLevel::Caution:   return "Caution";
        case DangerLevel::Dangerous: return "Dangerous";
        case DangerLevel::Forbidden: return "Forbidden";
    }
    return "Unknown";
}

} // namespace cc::utils::bash
