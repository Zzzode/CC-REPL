module;
#include <optional>
#include <string>
#include <string_view>
#include <cstdlib>

export module cc.hooks.ssh_session;

export namespace cc::hooks {


struct SshInfo {
    std::string client_ip;
    std::string user;
};


inline bool is_ssh_session() {

    const char* ssh_client = std::getenv("SSH_CLIENT");
    const char* ssh_conn = std::getenv("SSH_CONNECTION");
    return (ssh_client != nullptr) || (ssh_conn != nullptr);
}


inline std::optional<SshInfo> get_ssh_info() {
    if (!is_ssh_session()) {
        return std::nullopt;
    }
    const char* ssh_client = std::getenv("SSH_CLIENT");
    const char* user = std::getenv("USER");

    std::string client_ip;
    if (ssh_client) {

        std::string_view sv(ssh_client);
        auto space = sv.find(' ');
        client_ip = std::string(sv.substr(0, space));
    }

    return SshInfo{
        .client_ip = client_ip,
        .user = user ? user : ""
    };
}


inline void configure_for_ssh() {




}


inline bool is_tmux_session() {
    const char* tmux = std::getenv("TMUX");
    return tmux != nullptr && tmux[0] != '\0';
}

} // namespace cc::hooks
