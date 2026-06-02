module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <span>
#include <algorithm>

export module cc.tools.bash_permissions;

export namespace cc::tools {

// 命令权限级别：阻止、需要批准、允许
enum class BashPermissionLevel {
    Blocked,       // 命令被完全禁止执行
    NeedsApproval, // 命令需要用户确认后才能执行
    Allowed        // 命令可以直接执行
};

// 权限模式，控制审批策略的严格程度
enum class PermissionMode {
    Strict,     // 严格模式：大多数命令需要审批
    Normal,     // 普通模式：仅危险命令需要审批
    Permissive  // 宽松模式：仅阻止已知危险命令
};

// 获取被阻止的命令列表
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

// 判断命令是否为安全的只读命令
inline auto is_safe_read_command(std::string_view command) -> bool {
    // 安全的只读命令前缀列表
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

    // 提取实际命令（去除前导空格）
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

// 检查当前工作目录是否在允许的目录范围内
inline auto is_in_allowed_directory(
    const std::filesystem::path& cmd_cwd,
    std::span<const std::filesystem::path> allowed_dirs
) -> bool {
    if (allowed_dirs.empty()) {
        return true; // 未配置限制时默认允许
    }

    // 规范化路径后判断是否位于某个允许目录下
    auto normalized_cwd = std::filesystem::weakly_canonical(cmd_cwd);

    return std::any_of(allowed_dirs.begin(), allowed_dirs.end(),
        [&](const std::filesystem::path& allowed) {
            auto normalized_allowed = std::filesystem::weakly_canonical(allowed);
            // 检查 cmd_cwd 是否以 allowed 为前缀
            auto [mismatch_cwd, mismatch_allowed] = std::mismatch(
                normalized_cwd.begin(), normalized_cwd.end(),
                normalized_allowed.begin(), normalized_allowed.end()
            );
            return mismatch_allowed == normalized_allowed.end();
        });
}

// 核心权限检查函数：根据命令内容和权限模式，判断命令的执行权限
inline auto check_bash_permission(
    std::string_view command,
    PermissionMode mode
) -> BashPermissionLevel {
    // 空命令直接阻止
    if (command.empty()) {
        return BashPermissionLevel::Blocked;
    }

    // 检查是否命中阻止列表
    const auto blocked = get_blocked_commands();
    for (const auto& blocked_cmd : blocked) {
        if (command.find(blocked_cmd) != std::string_view::npos) {
            return BashPermissionLevel::Blocked;
        }
    }

    // 宽松模式下，非阻止命令一律允许
    if (mode == PermissionMode::Permissive) {
        return BashPermissionLevel::Allowed;
    }

    // 安全只读命令在所有模式下均允许
    if (is_safe_read_command(command)) {
        return BashPermissionLevel::Allowed;
    }

    // 严格模式下，非只读命令均需审批
    if (mode == PermissionMode::Strict) {
        return BashPermissionLevel::NeedsApproval;
    }

    // 普通模式下的高风险命令需要审批
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

    // 默认允许执行
    return BashPermissionLevel::Allowed;
}

} // namespace cc::tools
