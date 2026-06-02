module;

#include <string>
#include <string_view>
#include <expected>
#include <cstdint>
#include <charconv>
#include <algorithm>
#include <cstring>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

export module cc.utils.peer_address;

export namespace cc::utils {

// Parsed network peer address
struct PeerAddress {
    std::string host;
    uint16_t port;
    bool is_tls;
};

// Parse a URL into a PeerAddress
inline std::expected<PeerAddress, std::string> parse_url(std::string_view url) {
    PeerAddress addr;

    // Determine scheme
    std::string_view remaining = url;
    if (remaining.starts_with("https://")) {
        addr.is_tls = true;
        addr.port = 443; // Default HTTPS port
        remaining = remaining.substr(8);
    } else if (remaining.starts_with("http://")) {
        addr.is_tls = false;
        addr.port = 80; // Default HTTP port
        remaining = remaining.substr(7);
    } else {
        return std::unexpected("Unsupported URL scheme: " + std::string(url));
    }

    // Find end of host:port (before path/query)
    auto path_start = remaining.find('/');
    auto host_port = remaining.substr(0, path_start);

    // Handle IPv6 addresses [host]:port
    if (host_port.starts_with("[")) {
        auto bracket_end = host_port.find(']');
        if (bracket_end == std::string_view::npos) {
            return std::unexpected("Invalid IPv6 address in URL");
        }
        addr.host = std::string(host_port.substr(1, bracket_end - 1));

        // Check for port after the bracket
        if (bracket_end + 1 < host_port.size() && host_port[bracket_end + 1] == ':') {
            auto port_str = host_port.substr(bracket_end + 2);
            uint16_t port = 0;
            auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
            if (ec != std::errc()) {
                return std::unexpected("Invalid port number in URL");
            }
            addr.port = port;
        }
    } else {
        // Regular host:port or just host
        auto colon = host_port.rfind(':');
        if (colon != std::string_view::npos) {
            addr.host = std::string(host_port.substr(0, colon));
            auto port_str = host_port.substr(colon + 1);
            uint16_t port = 0;
            auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
            if (ec != std::errc()) {
                return std::unexpected("Invalid port number in URL");
            }
            addr.port = port;
        } else {
            addr.host = std::string(host_port);
        }
    }

    if (addr.host.empty()) {
        return std::unexpected("Empty host in URL");
    }

    return addr;
}

// Convert PeerAddress back to a URL-like string
inline std::string to_string(const PeerAddress& addr) {
    std::string result;
    result += addr.is_tls ? "https://" : "http://";

    // IPv6 addresses need brackets
    if (addr.host.find(':') != std::string::npos) {
        result += "[" + addr.host + "]";
    } else {
        result += addr.host;
    }

    // Only include port if non-default
    bool is_default_port = (addr.is_tls && addr.port == 443) ||
                           (!addr.is_tls && addr.port == 80);
    if (!is_default_port) {
        result += ":" + std::to_string(addr.port);
    }

    return result;
}

// Resolve a hostname to its IP address
inline std::expected<std::string, std::string> resolve_host(std::string_view hostname) {
    std::string host(hostname);

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int status = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (status != 0) {
        return std::unexpected("DNS resolution failed for '" + host + "': " +
                             std::string(gai_strerror(status)));
    }

    // Get the first result
    char ip_str[INET6_ADDRSTRLEN]{};
    if (result->ai_family == AF_INET) {
        auto* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
    } else if (result->ai_family == AF_INET6) {
        auto* addr = reinterpret_cast<struct sockaddr_in6*>(result->ai_addr);
        inet_ntop(AF_INET6, &addr->sin6_addr, ip_str, sizeof(ip_str));
    }

    freeaddrinfo(result);

    if (ip_str[0] == '\0') {
        return std::unexpected("No valid address found for: " + host);
    }

    return std::string(ip_str);
}

} // namespace cc::utils
