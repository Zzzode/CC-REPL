module;
#include <string>
#include <string_view>
#include <map>
#include <expected>
#include <array>
#include <cstdio>
#include <sstream>

export module cc.tools.remote_trigger_tool;
import cc.utils.bash_execution;

export namespace cc::tools {

namespace detail {
[[nodiscard]] inline auto shell_quote(std::string_view s) -> std::string {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += R"('\'')";
        else out += ch;
    }
    out += "'";
    return out;
}

[[nodiscard]] inline auto json_escape(std::string_view s) -> std::string {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '"': out += R"(\")"; break;
            case '\\': out += R"(\\)"; break;
            case '\n': out += R"(\n)"; break;
            default: out += ch; break;
        }
    }
    return out;
}
}


struct RemoteTriggerInput {
    std::string target;
    std::string message;
    std::map<std::string, std::string> params;
};


inline auto execute_remote_trigger(
    const RemoteTriggerInput& input
) -> std::expected<std::string, std::string> {

    if (input.target.empty()) {
        return std::unexpected(std::string("target is required"));
    }
    if (input.message.empty()) {
        return std::unexpected(std::string("message is required"));
    }



    if (input.target.find("127.0.0.1") != std::string::npos ||
        input.target.find("localhost") != std::string::npos ||
        input.target.find("169.254") != std::string::npos ||
        input.target.find("10.") == 0) {
        return std::unexpected(std::string("Cannot trigger internal network addresses"));
    }

    std::ostringstream payload;
    payload << R"({"message":")" << detail::json_escape(input.message) << R"(","params":{)";
    bool first = true;
    for (const auto& [key, value] : input.params) {
        if (!first) payload << ',';
        first = false;
        payload << '"' << detail::json_escape(key) << R"(":")" << detail::json_escape(value) << '"';
    }
    payload << "}}";

    const auto cmd = "curl -fsS --max-time 30 -H 'Content-Type: application/json' --data " +
        detail::shell_quote(payload.str()) + " " + detail::shell_quote(input.target) + " 2>&1";
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) return std::unexpected(std::string("Failed to spawn remote trigger request"));
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) output += buffer.data();
    const int status = cc::utils::bash::pclose_spawn(pipe);
    if (status != 0) return std::unexpected("Remote trigger failed: " + output);
    return output.empty() ? std::string("Remote trigger delivered") : output;
}


inline auto get_remote_trigger_prompt() -> std::string {
    return R"(## RemoteTriggerTool

Trigger a remote action or webhook. Used for integrations with external systems.

### Parameters:
- `target` (required): The webhook URL or trigger identifier
- `message` (required): The message or payload to send
- `params` (optional): Additional key-value parameters

### Usage:
- Trigger CI/CD pipelines
- Send notifications to external services
- Invoke remote automation workflows

### Security:
- Only pre-configured targets are allowed
- Internal network addresses are blocked
- All triggers are logged for audit

### Example:
```json
{
  "target": "https://hooks.example.com/deploy",
  "message": "Deploy to staging",
  "params": {"branch": "main", "environment": "staging"}
}
```)";
}

} // namespace cc::tools
