module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdlib>
#include <algorithm>
#include <cctype>
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

    std::string entry;
    auto flush = [&] {
        if (!entry.empty()) {
            entries.push_back(entry);
            entry.clear();
        }
    };

    for (const char ch : std::string_view(no_proxy_env)) {
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
            flush();
        } else {
            entry.push_back(ch);
        }
    }
    flush();
    return entries;
}

[[nodiscard]] inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

struct TargetHost {
    std::string host;
    std::string port;
};

[[nodiscard]] inline TargetHost parse_target_host(std::string_view url) {
    const bool is_https = url.starts_with("https://");
    const bool is_http = url.starts_with("http://");
    if (auto scheme_end = url.find("://"); scheme_end != std::string_view::npos) {
        url = url.substr(scheme_end + 3);
    }

    if (auto authority_end = url.find_first_of("/?#"); authority_end != std::string_view::npos) {
        url = url.substr(0, authority_end);
    }
    if (auto at = url.rfind('@'); at != std::string_view::npos) {
        url = url.substr(at + 1);
    }

    TargetHost target;
    target.port = is_https ? "443" : (is_http ? "80" : "");

    if (url.starts_with("[")) {
        if (auto close = url.find(']'); close != std::string_view::npos) {
            target.host = std::string(url.substr(1, close - 1));
            if (close + 1 < url.size() && url[close + 1] == ':') {
                target.port = std::string(url.substr(close + 2));
            }
            target.host = lower(std::move(target.host));
            return target;
        }
    }

    const auto first_colon = url.find(':');
    const auto last_colon = url.rfind(':');
    if (first_colon != std::string_view::npos && first_colon == last_colon) {
        target.host = std::string(url.substr(0, first_colon));
        target.port = std::string(url.substr(first_colon + 1));
    } else {
        target.host = std::string(url);
    }
    target.host = lower(std::move(target.host));
    return target;
}

[[nodiscard]] inline bool pattern_has_port(std::string_view pattern) {
    return pattern.find(':') != std::string_view::npos;
}

} // namespace detail

// Resolve proxy configuration from environment variables
inline std::optional<ProxyConfig> resolve_proxy() {
    // Match TS behavior: lowercase variants take precedence, then uppercase.
    const char* proxy_url = nullptr;

    if (!proxy_url) proxy_url = std::getenv("https_proxy");
    if (!proxy_url) proxy_url = std::getenv("HTTPS_PROXY");
    if (!proxy_url) proxy_url = std::getenv("http_proxy");
    if (!proxy_url) proxy_url = std::getenv("HTTP_PROXY");
    if (!proxy_url) proxy_url = std::getenv("all_proxy");
    if (!proxy_url) proxy_url = std::getenv("ALL_PROXY");

    if (!proxy_url || std::string_view(proxy_url).empty()) return std::nullopt;

    ProxyConfig config;
    config.url = proxy_url;
    detail::parse_proxy_auth(config.url, config);

    // Parse NO_PROXY
    const char* no_proxy = std::getenv("no_proxy");
    if (!no_proxy) no_proxy = std::getenv("NO_PROXY");
    config.no_proxy = detail::parse_no_proxy(no_proxy);

    return config;
}

// Check if a URL should bypass the proxy (based on NO_PROXY patterns)
inline bool should_use_proxy(std::string_view url) {
    auto config = resolve_proxy();
    if (!config) return false; // No proxy configured

    const auto target = detail::parse_target_host(url);
    const auto host_with_port = target.port.empty()
        ? target.host
        : target.host + ":" + target.port;

    // Check against NO_PROXY entries
    for (const auto& pattern : config->no_proxy) {
        auto lower_pattern = detail::lower(pattern);
        if (lower_pattern == "*") return false;

        // Port-specific patterns are exact host:port matches.
        if (detail::pattern_has_port(lower_pattern)) {
            if (host_with_port == lower_pattern) return false;
            continue;
        }

        if (lower_pattern.starts_with(".")) {
            if (target.host == lower_pattern.substr(1) ||
                target.host.ends_with(lower_pattern)) {
                return false;
            }
            continue;
        }

        if (target.host == lower_pattern) return false;
    }

    return true; // Should use proxy
}

// Get the proxy URL appropriate for a given target URL
inline std::optional<std::string> get_proxy_for_url(std::string_view url) {
    if (!should_use_proxy(url)) return std::nullopt;
    auto config = resolve_proxy();
    if (!config) return std::nullopt;
    return config->url;
}

} // namespace cc::utils
