module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <utility>

export module cc.commands.mcp.add_command;

export namespace cc::commands {

// MCP 服务器配置
struct McpServerConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
};

auto validate_server_config(McpServerConfig config) -> std::vector<std::string>;
auto list_configured_servers() -> std::vector<McpServerConfig>;
auto mcp_servers_path() -> std::filesystem::path;
auto write_configured_servers(const std::vector<McpServerConfig>& servers) -> std::expected<void, std::string>;

// 添加 MCP 服务器配置
auto add_mcp_server(McpServerConfig config) -> std::expected<void, std::string> {
    auto errors = validate_server_config(config);
    if (!errors.empty()) {
        return std::unexpected("Validation failed: " + errors.front());
    }
    auto servers = list_configured_servers();
    bool replaced = false;
    for (auto& server : servers) {
        if (server.name == config.name) {
            server = std::move(config);
            replaced = true;
            break;
        }
    }
    if (!replaced) servers.push_back(std::move(config));
    return write_configured_servers(servers);
}

// 移除 MCP 服务器配置
auto remove_mcp_server(std::string_view name) -> std::expected<void, std::string> {
    if (name.empty()) {
        return std::unexpected("Server name cannot be empty");
    }
    auto servers = list_configured_servers();
    auto old_size = servers.size();
    std::erase_if(servers, [&](const auto& server) { return server.name == name; });
    if (servers.size() == old_size) return std::unexpected("Server not found: " + std::string(name));
    return write_configured_servers(servers);
}

// 列出所有已配置的 MCP 服务器
auto list_configured_servers() -> std::vector<McpServerConfig> {
    std::vector<McpServerConfig> servers;
    std::ifstream input{mcp_servers_path()};
    std::string line;
    while (std::getline(input, line)) {
        std::stringstream ss{line};
        std::string name;
        std::string command;
        if (!std::getline(ss, name, '|') || !std::getline(ss, command, '|')) continue;
        McpServerConfig config{.name = name, .command = command, .args = {}, .env = {}};
        std::string arg;
        while (std::getline(ss, arg, ',')) if (!arg.empty()) config.args.push_back(arg);
        servers.push_back(std::move(config));
    }
    return servers;
}

// 验证服务器配置的有效性，返回错误列表
auto validate_server_config(McpServerConfig config) -> std::vector<std::string> {
    std::vector<std::string> errors;

    if (config.name.empty()) {
        errors.push_back("Server name is required");
    }
    if (config.command.empty()) {
        errors.push_back("Command is required");
    }

    // 名称不能包含特殊字符
    for (char c : config.name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            errors.push_back("Server name contains invalid characters");
            break;
        }
    }

    return errors;
}

auto mcp_servers_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".claude" / "mcp_servers.txt";
    return std::filesystem::path{".claude"} / "mcp_servers.txt";
}

auto write_configured_servers(const std::vector<McpServerConfig>& servers) -> std::expected<void, std::string> {
    auto path = mcp_servers_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::trunc};
    if (!output) return std::unexpected("Cannot write MCP server configuration");
    for (const auto& server : servers) {
        output << server.name << '|' << server.command << '|';
        for (std::size_t i = 0; i < server.args.size(); ++i) {
            if (i != 0) output << ',';
            output << server.args[i];
        }
        output << '\n';
    }
    return {};
}

} // namespace cc::commands
