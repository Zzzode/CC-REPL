// Security Review command - moved to plugin, provides embedded security audit prompt
module;
#include <string>
#include <string_view>
export module cc.commands.security_review;
export namespace cc::commands::security_review {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "security-review"; }

[[nodiscard]] inline auto run(std::string_view target = {}) -> CommandResponse {
    std::string msg = "This command has moved to a plugin.\n\n"
        "To perform a security review, you can ask me directly:\n\n"
        "  \"Review the security of ";
    
    if (!target.empty()) {
        msg += std::string(target);
    } else {
        msg += "this codebase";
    }
    
    msg += "\"\n\n"
        "I will analyze for:\n"
        "- OWASP Top 10 vulnerabilities\n"
        "- Injection flaws (SQL, command, XSS)\n"
        "- Authentication/authorization weaknesses\n"
        "- Sensitive data exposure\n"
        "- Dependency vulnerabilities\n"
        "- Cryptographic issues\n"
        "- Access control problems\n\n"
        "Or install the security-review skill: `npx skills install security-review`";
    
    return {.ok = true, .message = msg};
}

}
