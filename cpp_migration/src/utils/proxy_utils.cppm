module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdlib>
#include <algorithm>
#include <sstream>

export module cc.utils.proxy_utils;

export namespace cc::utils {

// Proxy configuration resolved from environment
struct ProxyConfig {
    std::string url;
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::vector<std::string> no_proxy;
};

namespace detail {

// Parse username:password from a proxy URL
inline void parse_proxy_auth(const std::string& url, ProxyConfig& config) {
    // Look for http://user:pass@host:port format
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return;

    auto auth_start = scheme_end + 3;
    auto at_pos = url.find('@', auth_start);
    if (at_pos == std::string::npos) return;

    std::string auth = url.substr(auth_start, at_pos - auth_start);
    auto colon = auth.find(':');
    if (colon != std::string::npos) {
        config.username = auth.substr(0, colon);
        config.password = auth.substr(colon + 1);
    } else {
        config.username = auth;
    }
}

// Parse NO_PROXY into a list of patterns
inline std::vector<std::string> parse_no_proxy(const char* no_proxy_env) {
    std::vector<std::string> entries;
    if (!no_proxy_env) return entries;

    std::istringstream stream(no_proxy_env);
    std::string entry;
    while (std::getline(stream, entry, ',')) {
        // Trim whitespace
        auto start = entry.find_first_not_of(" \t");
        auto end = entry.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            entries.push_back(entry.substr(start, end - start + 1));
        }
    }
    return entries;
}

} // namespace detail

// Resolve proxy configuration from environment variables
inline std::optional<ProxyConfig> resolve_proxy() {
    // Check in order: HTTPS_PROXY, HTTP_PROXY, ALL_PROXY (case-insensitive)
    const char* proxy_url = nullptr;

    // Check uppercase first, then lowercase
    if (!proxy_url) proxy_url = std::getenv("HTTPS_PROXY");
    if (!proxy_url) proxy_url = std::getenv("https_proxy");
    if (!proxy_url) proxy_url = std::getenv("HTTP_PROXY");
    if (!proxy_url) proxy_url = std::getenv("http_proxy");
    if (!proxy_url) proxy_url = std::getenv("ALL_PROXY");
    if (!proxy_url) proxy_url = std::getenv("all_proxy");

    if (!proxy_url || std::string_view(proxy_url).empty()) return std::nullopt;

    ProxyConfig config;
    config.url = proxy_url;
    detail::parse_proxy_auth(config.url, config);

    // Parse NO_PROXY
    const char* no_proxy = std::getenv("NO_PROXY");
    if (!no_proxy) no_proxy = std::getenv("no_proxy");
    config.no_proxy = detail::parse_no_proxy(no_proxy);

    return config;
}

// Check if a URL should bypass the proxy (based on NO_PROXY patterns)
inline bool should_use_proxy(std::string_view url) {
    auto config = resolve_proxy();
    if (!config) return false; // No proxy configured

    // Extract hostname from URL
    std::string host;
    auto scheme_end = url.find("://");
    if (scheme_end != std::string_view::npos) {
        auto host_start = scheme_end + 3;
        auto host_end = url.find_first_of(":/", host_start);
        host = std::string(url.substr(host_start, host_end - host_start));
    } else {
        auto host_end = url.find_first_of(":/");
        host = std::string(url.substr(0, host_end));
    }

    // Check against NO_PROXY entries
    for (const auto& pattern : config->no_proxy) {
        if (pattern == "*") return false;

        // Match exact hostname or suffix (with leading dot)
        std::string lower_host = host;
        std::string lower_pattern = pattern;
        std::transform(lower_host.begin(), lower_host.end(), lower_host.begin(), ::tolower);
        std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(), ::tolower);

        if (lower_host == lower_pattern) return false;
        if (lower_pattern[0] == '.' && lower_host.ends_with(lower_pattern)) return false;
        if (lower_host.ends_with("." + lower_pattern)) return false;
    }

    return true; // Should use proxy
}

// Get the proxy URL appropriate for a given target URL
inline std::optional<std::string> get_proxy_for_url(std::string_view url) {
    if (!should_use_proxy(url)) return std::nullopt;

    // HTTPS URLs use HTTPS_PROXY, HTTP uses HTTP_PROXY
    bool is_https = url.starts_with("https://");

    const char* proxy = nullptr;
    if (is_https) {
        proxy = std::getenv("HTTPS_PROXY");
        if (!proxy) proxy = std::getenv("https_proxy");
    } else {
        proxy = std::getenv("HTTP_PROXY");
        if (!proxy) proxy = std::getenv("http_proxy");
    }
    if (!proxy) {
        proxy = std::getenv("ALL_PROXY");
        if (!proxy) proxy = std::getenv("all_proxy");
    }

    if (proxy && std::string_view(proxy).size() > 0) return std::string(proxy);
    return std::nullopt;
}

} // namespace cc::utils
