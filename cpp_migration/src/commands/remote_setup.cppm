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
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

export module cc.commands.remote_setup;

export namespace cc::commands {

auto remote_setup_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "remote-setup.txt";
    return std::filesystem::path{".cc-repl"} / "remote-setup.txt";
}


struct RemoteSetupConfig {
    std::string host;
    uint16_t port;
    std::string auth_method; // "ssh_key", "token", "oauth"
};


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

namespace detail {

auto can_connect_tcp(const std::string& host, uint16_t port) -> std::expected<void, std::string> {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const auto port_string = std::to_string(port);
    const auto gai = ::getaddrinfo(host.c_str(), port_string.c_str(), &hints, &result);
    if (gai != 0) {
        return std::unexpected(std::string{"DNS lookup failed: "} + ::gai_strerror(gai));
    }

    int connected_fd = -1;
    for (auto* addr = result; addr != nullptr; addr = addr->ai_next) {
        const int fd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            connected_fd = fd;
            break;
        }
        ::close(fd);
    }

    ::freeaddrinfo(result);
    if (connected_fd < 0) {
        return std::unexpected("Remote host is not reachable");
    }
    ::close(connected_fd);
    return {};
}

} // namespace detail


auto test_remote_connection(RemoteSetupConfig config) -> std::expected<void, std::string> {
    if (config.host.empty()) {
        return std::unexpected("Host cannot be empty");
    }
    if (config.port == 0) return std::unexpected("Port must be non-zero");
    if (config.auth_method.empty()) return std::unexpected("Authentication method is required");
    return detail::can_connect_tcp(config.host, config.port);
}


auto get_remote_status() -> std::string {
    std::ifstream input{remote_setup_path()};
    if (input) return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return "No active remote connection";
}

} // namespace cc::commands
