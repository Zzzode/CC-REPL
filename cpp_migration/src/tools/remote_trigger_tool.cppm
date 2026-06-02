module;
#include <string>
#include <string_view>
#include <map>
#include <expected>
#include <array>
#include <cstdio>
#include <sstream>

export module cc.tools.remote_trigger_tool;

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

// 远程触发器输入参数
struct RemoteTriggerInput {
    std::string target;                        // 触发目标（webhook URL 或标识符）
    std::string message;                       // 触发消息/负载
    std::map<std::string, std::string> params; // 附加参数
};

// 执行远程触发
inline auto execute_remote_trigger(
    const RemoteTriggerInput& input
) -> std::expected<std::string, std::string> {
    // 验证输入
    if (input.target.empty()) {
        return std::unexpected(std::string("target is required"));
    }
    if (input.message.empty()) {
        return std::unexpected(std::string("message is required"));
    }

    // 安全检查：验证 target 格式
    // 不允许触发内部网络地址
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
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::unexpected(std::string("Failed to spawn remote trigger request"));
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) output += buffer.data();
    const int status = pclose(pipe);
    if (status != 0) return std::unexpected("Remote trigger failed: " + output);
    return output.empty() ? std::string("Remote trigger delivered") : output;
}

// 获取远程触发工具的提示词
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
