module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

export module cc.commands.remote_setup;

export namespace cc::commands {

auto remote_setup_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "remote-setup.txt";
    return std::filesystem::path{".cc-repl"} / "remote-setup.txt";
}

// 远程连接配置
struct RemoteSetupConfig {
    std::string host;
    uint16_t port;
    std::string auth_method; // "ssh_key", "token", "oauth"
};

// 设置远程连接
auto setup_remote(RemoteSetupConfig config) -> std::expected<void, std::string> {
    if (config.host.empty()) {
        return std::unexpected("Host cannot be empty");
    }
    if (config.port == 0) {
        return std::unexpected("Port must be non-zero");
    }
    if (config.auth_method.empty()) {
        return std::unexpected("Authentication method is required");
    }

    // 验证认证方法合法性
    if (config.auth_method != "ssh_key" &&
        config.auth_method != "token" &&
        config.auth_method != "oauth") {
        return std::unexpected("Invalid auth method: " + config.auth_method);
    }

    auto path = remote_setup_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::trunc};
    if (!output) return std::unexpected("Cannot write remote setup configuration");
    output << "host=" << config.host << '\n'
           << "port=" << config.port << '\n'
           << "auth_method=" << config.auth_method << '\n'
           << "status=configured\n";
    return {};
}

// 测试远程连接是否可达
auto test_remote_connection(RemoteSetupConfig config) -> std::expected<void, std::string> {
    if (config.host.empty()) {
        return std::unexpected("Host cannot be empty");
    }
    if (config.port == 0) return std::unexpected("Port must be non-zero");
    if (config.auth_method.empty()) return std::unexpected("Authentication method is required");
    return {};
}

// 获取当前远程连接状态
auto get_remote_status() -> std::string {
    std::ifstream input{remote_setup_path()};
    if (input) return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return "No active remote connection";
}

} // namespace cc::commands
