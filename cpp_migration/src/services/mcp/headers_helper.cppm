module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <map>

export module cc.services.mcp.headers_helper;

export namespace cc::services::mcp {

/// HTTP header pair
using HeaderMap = std::map<std::string, std::string>;

/// Build default headers for MCP requests
inline HeaderMap build_default_headers(std::string_view auth_token = "") {
    HeaderMap headers;
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "application/json";
    if (!auth_token.empty()) {
        headers["Authorization"] = "Bearer " + std::string(auth_token);
    }
    return headers;
}

/// Merge custom headers with defaults (custom takes priority)
inline HeaderMap merge_headers(const HeaderMap& defaults, const HeaderMap& custom) {
    HeaderMap result = defaults;
    for (const auto& [key, value] : custom) {
        result[key] = value;
    }
    return result;
}

/// Extract a specific header value (case-insensitive key)
inline std::optional<std::string> get_header(
    const HeaderMap& headers, std::string_view key) {
    std::string lower_key(key);
    for (auto& c : lower_key) {
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    }
    for (const auto& [k, v] : headers) {
        std::string lower_k = k;
        for (auto& c : lower_k) {
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        }
        if (lower_k == lower_key) return v;
    }
    return std::nullopt;
}

/// Parse OAuth port from URL or config string.
/// Accepts formats: "http://localhost:8080/callback", ":8080", "8080"
inline std::optional<int> parse_oauth_port(std::string_view url_or_config) {
    if (url_or_config.empty()) return std::nullopt;

    // Try format: just a number
    bool all_digits = true;
    for (char c : url_or_config) {
        if (c < '0' || c > '9') { all_digits = false; break; }
    }
    if (all_digits && !url_or_config.empty()) {
        int port = 0;
        for (char c : url_or_config) {
            port = port * 10 + (c - '0');
        }
        if (port > 0 && port <= 65535) return port;
        return std::nullopt;
    }

    // Try format: ":PORT"
    if (url_or_config[0] == ':') {
        auto num_part = url_or_config.substr(1);
        int port = 0;
        for (char c : num_part) {
            if (c < '0' || c > '9') break;
            port = port * 10 + (c - '0');
        }
        if (port > 0 && port <= 65535) return port;
        return std::nullopt;
    }

    // Try URL format: find port after "://host:PORT"
    auto scheme_end = url_or_config.find("://");
    if (scheme_end != std::string_view::npos) {
        auto host_start = scheme_end + 3;
        auto colon_pos = url_or_config.find(':', host_start);
        if (colon_pos != std::string_view::npos) {
            auto port_start = colon_pos + 1;
            int port = 0;
            for (size_t i = port_start; i < url_or_config.size(); ++i) {
                char c = url_or_config[i];
                if (c < '0' || c > '9') break;
                port = port * 10 + (c - '0');
            }
            if (port > 0 && port <= 65535) return port;
        }
    }

    return std::nullopt;
}

} // namespace cc::services::mcp
