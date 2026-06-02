module;
#include <optional>
#include <string>
#include <string_view>
#include <cstdlib>

export module cc.hooks.ssh_session;

export namespace cc::hooks {

// SSH 会话信息
struct SshInfo {
    std::string client_ip;
    std::string user;
};

// 检测当前是否在 SSH 会话中运行
inline bool is_ssh_session() {
    // SSH_CLIENT 或 SSH_CONNECTION 环境变量存在表明是 SSH 会话
    const char* ssh_client = std::getenv("SSH_CLIENT");
    const char* ssh_conn = std::getenv("SSH_CONNECTION");
    return (ssh_client != nullptr) || (ssh_conn != nullptr);
}

// 获取 SSH 连接信息
inline std::optional<SshInfo> get_ssh_info() {
    if (!is_ssh_session()) {
        return std::nullopt;
    }
    const char* ssh_client = std::getenv("SSH_CLIENT");
    const char* user = std::getenv("USER");

    std::string client_ip;
    if (ssh_client) {
        // SSH_CLIENT 格式: "client_ip client_port server_port"
        std::string_view sv(ssh_client);
        auto space = sv.find(' ');
        client_ip = std::string(sv.substr(0, space));
    }

    return SshInfo{
        .client_ip = client_ip,
        .user = user ? user : ""
    };
}

// 为 SSH 会话调整配置（如禁用颜色、减少带宽消耗）
inline void configure_for_ssh() {
    // 在 SSH 环境下可能需要：
    // - 降级颜色输出（256 色 → 16 色）
    // - 禁用动画和闪烁效果
    // - 减少不必要的网络请求
}

// 检测当前是否在 tmux 会话中
inline bool is_tmux_session() {
    const char* tmux = std::getenv("TMUX");
    return tmux != nullptr && tmux[0] != '\0';
}

} // namespace cc::hooks
