module;
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <algorithm>
#include <regex>

export module cc.tools.bash_security;

export namespace cc::tools {

// 安全检查结果
struct SecurityCheck {
    bool passed;                    // 检查是否通过
    std::string reason;             // 拒绝或通过的原因
    std::optional<std::string> suggestion; // 替代建议（可选）
};

// 检测命令是否为破坏性操作
inline auto is_destructive_command(std::string_view command) -> bool {
    static const std::vector<std::string_view> destructive_patterns = {
        "rm -rf",
        "rm -r",
        "rmdir",
        "mkfs",
        "format",
        "fdisk",
        "dd if=",
        "shred",
        "wipefs",
        "> /dev/",
        "truncate",
        "git clean -fd",
        "git reset --hard",
        "git push --force",
        "git push -f",
        "drop database",
        "drop table",
        "DELETE FROM",
        "TRUNCATE TABLE"
    };

    return std::any_of(destructive_patterns.begin(), destructive_patterns.end(),
        [&](std::string_view pattern) {
            return command.find(pattern) != std::string_view::npos;
        });
}

// 检测是否存在命令注入风险
inline auto detect_injection(std::string_view command) -> bool {
    // 检测常见注入模式
    static const std::vector<std::string_view> injection_patterns = {
        "$(", "`",           // 命令替换
        "&&", "||", ";",     // 命令链（可能绕过审查）
        "| bash", "| sh",   // 管道到 shell
        "eval ", "exec ",   // 动态执行
        "\\x", "\\u",       // 编码绕过
        "${IFS}",           // 分隔符绕过
        "<<<",              // here-string 注入
    };

    // 简单命令不检测链接符号（&&, ||, ;）
    // 但仍需检测危险的子命令执行
    static const std::vector<std::string_view> critical_injections = {
        "$(", "`",
        "| bash", "| sh", "| zsh",
        "eval ", "exec ",
        "${IFS}",
    };

    return std::any_of(critical_injections.begin(), critical_injections.end(),
        [&](std::string_view pattern) {
            return command.find(pattern) != std::string_view::npos;
        });
}

// 检测是否存在提权尝试
inline auto detect_privilege_escalation(std::string_view command) -> bool {
    static const std::vector<std::string_view> escalation_patterns = {
        "sudo ",
        "su ",
        "doas ",
        "pkexec",
        "chmod u+s",       // setuid
        "chmod g+s",       // setgid
        "chmod 4",         // setuid via numeric
        "/etc/sudoers",
        "/etc/passwd",
        "/etc/shadow",
        "visudo",
        "usermod",
        "adduser",
        "useradd",
        "passwd "
    };

    return std::any_of(escalation_patterns.begin(), escalation_patterns.end(),
        [&](std::string_view pattern) {
            return command.find(pattern) != std::string_view::npos;
        });
}

// 对命令进行脱敏处理，遮蔽敏感信息（密钥、token 等）
inline auto sanitize_command_for_display(std::string_view command) -> std::string {
    std::string result(command);

    // 用正则匹配并遮蔽 API 密钥、token 等敏感信息
    // 模式：常见环境变量赋值中的密钥值
    static const std::regex secret_patterns[] = {
        std::regex(R"((API_KEY|SECRET|TOKEN|PASSWORD|PASSWD|KEY)=['"]?)[^\s'"]+)", std::regex::icase),
        std::regex(R"((sk-[a-zA-Z0-9]{20,}))"),          // OpenAI/Anthropic style keys
        std::regex(R"((ghp_[a-zA-Z0-9]{36,}))"),         // GitHub PAT
        std::regex(R"((Bearer\s+)[^\s]+)", std::regex::icase),  // Bearer tokens
        std::regex(R"((-p\s+|--password[= ])\S+)"),      // 密码参数
    };

    for (const auto& pattern : secret_patterns) {
        result = std::regex_replace(result, pattern, "$1****");
    }

    return result;
}

// 综合安全检查入口
inline auto check_command_security(std::string_view command) -> SecurityCheck {
    // 空命令不安全
    if (command.empty()) {
        return SecurityCheck{false, "Empty command", std::nullopt};
    }

    // 检查破坏性命令
    if (is_destructive_command(command)) {
        return SecurityCheck{
            false,
            "Command is potentially destructive",
            "Consider using a safer alternative or adding --dry-run flag"
        };
    }

    // 检查注入风险
    if (detect_injection(command)) {
        return SecurityCheck{
            false,
            "Potential command injection detected",
            "Avoid using command substitution or eval in tool-executed commands"
        };
    }

    // 检查提权尝试
    if (detect_privilege_escalation(command)) {
        return SecurityCheck{
            false,
            "Privilege escalation attempt detected",
            "Run commands with current user privileges only"
        };
    }

    return SecurityCheck{true, "Command passed security checks", std::nullopt};
}

} // namespace cc::tools
