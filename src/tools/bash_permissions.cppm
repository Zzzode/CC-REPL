module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <span>
#include <algorithm>

export module cc.tools.bash_permissions;

import cc.tools.command_semantics;  // migrated: shared classifiers
import cc.tools.tool;  // ToolPermission for default_bash_level_for bridge

export namespace cc::tools {


enum class BashPermissionLevel {
    Blocked,
    NeedsApproval,
    Allowed
};

// Bridge between the two permission models. ToolPermission classifies a tool's
// static capability (read/write/execute/network); BashPermissionLevel is a
// per-call decision. This mapping derives a sensible *default* per-call level
// from the capability so a tool's ToolPermission can seed its initial
// BashPermissionLevel before the live permission hook (or fail-closed rule)
// weighs in. Read-only -> Allowed; anything that mutates state or reaches the
// network -> NeedsApproval.
[[nodiscard]] constexpr BashPermissionLevel default_bash_level_for(
    cc::core::ToolPermission perm) noexcept {
    switch (perm) {
        case cc::core::ToolPermission::ReadOnly:
            return BashPermissionLevel::Allowed;
        case cc::core::ToolPermission::Network:
        case cc::core::ToolPermission::Write:
        case cc::core::ToolPermission::Execute:
            return BashPermissionLevel::NeedsApproval;
    }
    return BashPermissionLevel::NeedsApproval;
}


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

    // migrated: fast-classify using the shared CommandType enum produced by
    // command_semantics.  Pure reads are auto-allowed; everything else is
    // either auto-allowed (Permissive was already handled) or NeedsApproval
    // under Strict, or NeedsApproval only for "risky" types under Normal.
    const auto cmd_type = classify_command(command);

    if (cmd_type == CommandType::Read) {
        return BashPermissionLevel::Allowed;
    }

    // migrated: finer-grained git classification — read-only git subcommands
    // (status, log, diff, branch...) are auto-allowed even if the generic
    // classify_command happened to miss a prefix.
    if (is_git_command(command)) {
        switch (classify_git_operation(command)) {
            case CommandType::Read:
                return BashPermissionLevel::Allowed;
            case CommandType::Write:
            case CommandType::Network:
                if (mode == PermissionMode::Strict) {
                    return BashPermissionLevel::NeedsApproval;
                }
                return BashPermissionLevel::Allowed;
            case CommandType::Destructive:
                return BashPermissionLevel::NeedsApproval;
            default:
                break;
        }
    }

    // migrated: package-manager classification
    if (is_package_manager_command(command)) {
        switch (classify_package_manager_operation(command)) {
            case CommandType::Destructive:
                // uninstall / remove / purge — always approve
                return BashPermissionLevel::NeedsApproval;
            case CommandType::Network:
            case CommandType::Execute:
            case CommandType::Write:
                if (mode == PermissionMode::Strict) {
                    return BashPermissionLevel::NeedsApproval;
                }
                return BashPermissionLevel::Allowed;
            default:
                break;
        }
    }

    if (mode == PermissionMode::Strict) {
        return BashPermissionLevel::NeedsApproval;
    }

    // Under Normal mode, anything Destructive/Network/Write/Execute that
    // wasn't handled above needs explicit approval.
    switch (cmd_type) {
        case CommandType::Destructive:
        case CommandType::Network:
            return BashPermissionLevel::NeedsApproval;
        case CommandType::Write:
        case CommandType::Execute:
        case CommandType::Unknown:
            // Allow by default; the security layer (bash_security) will still
            // catch injections / privilege escalations.
            return BashPermissionLevel::Allowed;
        case CommandType::Read:
            return BashPermissionLevel::Allowed;
    }

    return BashPermissionLevel::Allowed;
}

} // namespace cc::tools
