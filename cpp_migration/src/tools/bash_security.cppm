module;
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <algorithm>
#include <regex>

export module cc.tools.bash_security;

export namespace cc::tools {


struct SecurityCheck {
    bool passed;
    std::string reason;
    std::optional<std::string> suggestion;
};


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


inline auto detect_injection(std::string_view command) -> bool {

    static const std::vector<std::string_view> injection_patterns = {
        "$(", "`",
        "&&", "||", ";",
        "| bash", "| sh",
        "eval ", "exec ",
        "\\x", "\\u",
        "${IFS}",
        "<<<",
    };



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


inline auto sanitize_command_for_display(std::string_view command) -> std::string {
    std::string result(command);



    static const std::regex secret_patterns[] = {
        std::regex(R"((API_KEY|SECRET|TOKEN|PASSWORD|PASSWD|KEY)=['"]?)[^\s'"]+)", std::regex::icase),
        std::regex(R"((sk-[a-zA-Z0-9]{20,}))"),          // OpenAI/Anthropic style keys
        std::regex(R"((ghp_[a-zA-Z0-9]{36,}))"),         // GitHub PAT
        std::regex(R"((Bearer\s+)[^\s]+)", std::regex::icase),  // Bearer tokens
        std::regex(R"((-p\s+|--password[= ])\S+)"),
    };

    for (const auto& pattern : secret_patterns) {
        result = std::regex_replace(result, pattern, "$1****");
    }

    return result;
}


inline auto check_command_security(std::string_view command) -> SecurityCheck {

    if (command.empty()) {
        return SecurityCheck{false, "Empty command", std::nullopt};
    }


    if (is_destructive_command(command)) {
        return SecurityCheck{
            false,
            "Command is potentially destructive",
            "Consider using a safer alternative or adding --dry-run flag"
        };
    }


    if (detect_injection(command)) {
        return SecurityCheck{
            false,
            "Potential command injection detected",
            "Avoid using command substitution or eval in tool-executed commands"
        };
    }


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
