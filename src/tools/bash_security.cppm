module;
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <algorithm>
#include <regex>
#include <cctype>

export module cc.tools.bash_security;

export namespace cc::tools {


struct SecurityCheck {
    bool passed;
    std::string reason;
    std::optional<std::string> suggestion;
};


// Normalize a command before security pattern matching so trivial
// obfuscations cannot bypass detection. Collapses whitespace runs, decodes
// backslash escapes (\X -> X), drops empty quote pairs that fragment a token
// (r""m, su''do), and lowercases the result so case variants (RM, Sudo, Mkfs)
// are caught. This is NOT full shell parsing — it only removes the simple
// evasions that pure substring matching misses. String contents inside quotes
// may be altered; that is acceptable because security checks care about
// command structure, not data payloads.
inline auto normalize_for_security(std::string_view command) -> std::string {
    std::string out;
    out.reserve(command.size());
    bool last_was_space = true;  // collapse leading whitespace
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        // Backslash escape: keep the next char (drop the backslash).
        if (c == '\\' && i + 1 < command.size()) {
            out.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(command[i + 1]))));
            ++i;
            last_was_space = false;
            continue;
        }
        // Whitespace run -> single space.
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_was_space) {
                out.push_back(' ');
                last_was_space = true;
            }
            continue;
        }
        // Empty quote pair ("", '') fragments a token; drop both chars.
        if ((c == '"' || c == '\'') && i + 1 < command.size() && command[i + 1] == c) {
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        last_was_space = false;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}


inline auto is_destructive_command(std::string_view command) -> bool {
    static const std::vector<std::string_view> destructive_patterns = {
        "rm -rf", "rm -r", "rmdir", "mkfs", "format", "fdisk",
        "dd if=", "shred", "wipefs", "> /dev/", "truncate",
        "git clean -fd", "git reset --hard", "git push --force", "git push -f",
        "drop database", "drop table", "delete from", "truncate table"
    };

    const auto normalized = normalize_for_security(command);
    return std::any_of(destructive_patterns.begin(), destructive_patterns.end(),
        [&](std::string_view pattern) {
            return normalized.find(pattern) != std::string::npos;
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

    const auto normalized = normalize_for_security(command);
    return std::any_of(critical_injections.begin(), critical_injections.end(),
        [&](std::string_view pattern) {
            return normalized.find(pattern) != std::string::npos;
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

    const auto normalized = normalize_for_security(command);
    return std::any_of(escalation_patterns.begin(), escalation_patterns.end(),
        [&](std::string_view pattern) {
            return normalized.find(pattern) != std::string::npos;
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
