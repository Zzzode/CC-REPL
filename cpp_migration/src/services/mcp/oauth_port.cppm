/// @file oauth_port.cppm
/// @brief OAuth callback port management for MCP auth
///
/// Mirrors src/services/mcp/oauthPort.ts: picks a random port in the
/// dynamic/private range, bind-probes it for availability, retries up to 100
/// times, and falls back to a fixed port. Honors the MCP_OAUTH_CALLBACK_PORT
/// environment override. RFC 8252 §7.3 (Native Apps): loopback redirect URIs
/// match any port as long as the path matches.
module;
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <netinet/in.h>
#include <random>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
export module cc.services.mcp.oauth_port;
export namespace cc::services::mcp {

// Final fallback when random probing fails. Kept as a named constant so the
// behavior is discoverable even though the happy path no longer uses it.
inline constexpr uint16_t kDefaultOAuthPort = 8912;

// RFC 6056 ephemeral/dynamic port range. Windows reserves 49152-65535, so the
// TS original uses 39152-49151 there; we use the non-Windows range and do not
// branch on platform (C++ migration does not currently detect Windows).
inline constexpr uint16_t kRedirectPortRangeStart = 49152;
inline constexpr uint16_t kRedirectPortRangeEnd = 65535;
inline constexpr uint16_t kRedirectPortFallback = 3118;
inline constexpr int kRedirectPortMaxAttempts = 100;

/// Build a redirect URI on localhost with the given port and a fixed
/// `/callback` path (parity with buildRedirectUri in oauthPort.ts).
[[nodiscard]] inline std::string build_redirect_uri(uint16_t port = kRedirectPortFallback) {
    return "http://localhost:" + std::to_string(port) + "/callback";
}

namespace detail {

/// Read the MCP_OAUTH_CALLBACK_PORT override; returns 0 when unset/invalid.
[[nodiscard]] inline uint16_t configured_callback_port() noexcept {
    const char* raw = std::getenv("MCP_OAUTH_CALLBACK_PORT");
    if (raw == nullptr || *raw == '\0') return 0;
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value <= 0 || value > 65535) {
        return 0;
    }
    return static_cast<uint16_t>(value);
}

/// Bind-probe a single TCP port: open a listening socket, succeed if bind
/// works, always close. Mirrors the createServer().listen() check in TS.
[[nodiscard]] inline bool is_port_available(uint16_t port) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int opt = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    bool ok = ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

} // namespace detail

/// Find an available port for the OAuth redirect.
///
/// Order (matches findAvailablePort in oauthPort.ts):
///   1. MCP_OAUTH_CALLBACK_PORT env override, if set and > 0.
///   2. A random port in [kRedirectPortRangeStart, kRedirectPortRangeEnd],
///      bind-probed, retried up to kRedirectPortMaxAttempts times.
///   3. The fallback port kRedirectPortFallback, if bindable.
///   4. An error otherwise.
[[nodiscard]] inline std::expected<uint16_t, std::string> find_available_oauth_port() {
    if (uint16_t cfg = detail::configured_callback_port(); cfg != 0) {
        return cfg;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    const uint32_t range =
        static_cast<uint32_t>(kRedirectPortRangeEnd) - kRedirectPortRangeStart + 1u;
    std::uniform_int_distribution<uint32_t> dist(0u, range - 1u);

    for (int attempt = 0; attempt < kRedirectPortMaxAttempts; ++attempt) {
        uint16_t port = static_cast<uint16_t>(kRedirectPortRangeStart + dist(gen));
        if (detail::is_port_available(port)) {
            return port;
        }
    }

    if (detail::is_port_available(kRedirectPortFallback)) {
        return kRedirectPortFallback;
    }
    return std::unexpected(std::string{"No available ports for OAuth redirect"});
}

} // namespace cc::services::mcp
