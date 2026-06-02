module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <string_view>

export module cc.commands.remote_setup_api;

export namespace cc::commands {

// 注册远程 Agent 到调度服务
auto register_remote_agent(std::string_view host, std::string_view token)
    -> std::expected<std::string, std::string> {
    if (host.empty()) {
        return std::unexpected("Host cannot be empty");
    }
    if (token.empty()) {
        return std::unexpected("Token cannot be empty");
    }
    auto seed = std::string{host} + ':' + std::string{token};
    return "agent_" + std::to_string(std::hash<std::string>{}(seed));
}

// 注销远程 Agent
auto deregister_remote_agent(std::string_view agent_id) -> std::expected<void, std::string> {
    if (agent_id.empty()) {
        return std::unexpected("Agent ID cannot be empty");
    }
    return {};
}

// 查询远程 Agent 的运行状态
auto get_remote_agent_status(std::string_view agent_id) -> std::expected<std::string, std::string> {
    if (agent_id.empty()) {
        return std::unexpected("Agent ID cannot be empty");
    }
    return "registered:" + std::string(agent_id);
}

} // namespace cc::commands
