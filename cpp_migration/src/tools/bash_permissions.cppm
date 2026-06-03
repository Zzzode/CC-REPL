module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <span>
#include <algorithm>

export module cc.tools.bash_permissions;

export namespace cc::tools {


enum class BashPermissionLevel {
    Blocked,
    NeedsApproval,
    Allowed
};


enum class PermissionMode {
    Strict,
    Normal,
    Permissive
};


inline auto get_blocked_commands() -> std::vector<std::string> {
    return {
        "rm -rf /",
        "rm -rf /*",
        "mkfs",
        "dd if=/dev/zero",
        ":(){ :|:& };:",  // fork bomb
        "> /dev/sda",
        "chmod -R 777 /",
        "chown -R",
        "shutdown",
        "reboot",
        "halt",
        "poweroff",
        "init 0",
        "init 6"
    };
}


inline auto is_safe_read_command(std::string_view command) -> bool {

    static const std::vector<std::string_view> safe_prefixes = {
        "ls", "cat", "head", "tail", "wc", "file", "stat",
        "which", "where", "type", "echo", "printf", "date",
        "whoami", "pwd", "env", "printenv", "uname",
        "git status", "git log", "git diff", "git show",
        "git branch", "git remote -v", "git rev-parse",
        "find", "locate", "grep", "rg", "ag", "fd",
        "tree", "du", "df", "free", "top -l 1",
        "ps", "id", "groups", "hostname"
    };


    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }

    return std::any_of(safe_prefixes.begin(), safe_prefixes.end(),
        [&](std::string_view prefix) {
            return trimmed.starts_with(prefix) &&
                   (trimmed.size() == prefix.size() ||
                    trimmed[prefix.size()] == ' ');
        });
}


inline auto is_in_allowed_directory(
    const std::filesystem::path& cmd_cwd,
    std::span<const std::filesystem::path> allowed_dirs
) -> bool {
    if (allowed_dirs.empty()) {
        return true;
    }


    auto normalized_cwd = std::filesystem::weakly_canonical(cmd_cwd);

    return std::any_of(allowed_dirs.begin(), allowed_dirs.end(),
        [&](const std::filesystem::path& allowed) {
            auto normalized_allowed = std::filesystem::weakly_canonical(allowed);

            auto [mismatch_cwd, mismatch_allowed] = std::mismatch(
                normalized_cwd.begin(), normalized_cwd.end(),
                normalized_allowed.begin(), normalized_allowed.end()
            );
            return mismatch_allowed == normalized_allowed.end();
        });
}


inline auto check_bash_permission(
    std::string_view command,
    PermissionMode mode
) -> BashPermissionLevel {

    if (command.empty()) {
        return BashPermissionLevel::Blocked;
    }


    const auto blocked = get_blocked_commands();
    for (const auto& blocked_cmd : blocked) {
        if (command.find(blocked_cmd) != std::string_view::npos) {
            return BashPermissionLevel::Blocked;
        }
    }


    if (mode == PermissionMode::Permissive) {
        return BashPermissionLevel::Allowed;
    }


    if (is_safe_read_command(command)) {
        return BashPermissionLevel::Allowed;
    }


    if (mode == PermissionMode::Strict) {
        return BashPermissionLevel::NeedsApproval;
    }


    static const std::vector<std::string_view> needs_approval_keywords = {
        "rm", "mv", "cp", "chmod", "chown",
        "sudo", "su", "apt", "brew", "npm install",
        "pip install", "docker", "kubectl",
        "git push", "git reset", "git checkout",
        "curl", "wget", "ssh", "scp"
    };

    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }

    for (const auto& keyword : needs_approval_keywords) {
        if (trimmed.starts_with(keyword) &&
            (trimmed.size() == keyword.size() || trimmed[keyword.size()] == ' ')) {
            return BashPermissionLevel::NeedsApproval;
        }
    }


    return BashPermissionLevel::Allowed;
}

} // namespace cc::tools
