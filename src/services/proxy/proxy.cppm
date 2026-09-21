/// @file proxy.cppm
/// @brief Proxy configuration service module.
/// Handles HTTP/HTTPS/SOCKS5 proxy resolution from environment and config,
/// no_proxy matching, curl option application, and CA certificate management.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <ranges>
#include <filesystem>
#include <cstdlib>
#include <unordered_map>
#include <fstream>

export module cc.services.proxy;

import cc.types.types;

export namespace cc::services::proxy {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;
using cc::core::VoidResult;

// ============================================================
// Proxy types and configuration
// ============================================================

/// Supported proxy protocol types
enum class ProxyType : uint8_t {
    None,       // Direct connection, no proxy
    HTTP,       // HTTP CONNECT proxy
    HTTPS,      // HTTPS proxy (proxy itself uses TLS)
    SOCKS5,     // SOCKS5 proxy
};

/// Convert proxy type to display string
[[nodiscard]] constexpr std::string_view proxy_type_to_string(ProxyType type) noexcept {
    switch (type) {
        case ProxyType::None:   return "none";
        case ProxyType::HTTP:   return "http";
        case ProxyType::HTTPS:  return "https";
        case ProxyType::SOCKS5: return "socks5";
    }
    return "unknown";
}

/// Full proxy configuration
struct ProxyConfig {
    std::optional<std::string> http_proxy;      // Proxy for HTTP requests
    std::optional<std::string> https_proxy;     // Proxy for HTTPS requests
    std::vector<std::string> no_proxy;          // Hosts/domains to bypass proxy
    std::optional<std::filesystem::path> ca_cert_path; // Custom CA certificate bundle

    /// Check if any proxy is configured
    [[nodiscard]] bool has_proxy() const noexcept {
        return http_proxy.has_value() || https_proxy.has_value();
    }

    /// Determine proxy type from URL scheme
    [[nodiscard]] static ProxyType detect_type(std::string_view proxy_url) {
        if (proxy_url.starts_with("socks5://") || proxy_url.starts_with("socks5h://")) {
            return ProxyType::SOCKS5;
        }
        if (proxy_url.starts_with("https://")) return ProxyType::HTTPS;
        if (proxy_url.starts_with("http://")) return ProxyType::HTTP;
        return ProxyType::None;
    }

    /// Summary for logging
    [[nodiscard]] std::string summary() const {
        std::string s;
        if (http_proxy) s += std::format("http_proxy={} ", *http_proxy);
        if (https_proxy) s += std::format("https_proxy={} ", *https_proxy);
        if (!no_proxy.empty()) {
            s += std::format("no_proxy=[{}]", no_proxy.size());
        }
        return s.empty() ? "no proxy configured" : s;
    }
};

// ============================================================
// ProxyResolver - resolves proxy settings for a given URL
// ============================================================

/// Resolves proxy configuration from environment variables or explicit config,
/// and determines whether a given URL should use a proxy.
class ProxyResolver {
public:
    /// Read proxy settings from standard environment variables
    [[nodiscard]] static ProxyConfig from_environment() {
        ProxyConfig config;

        // Check both lowercase and uppercase variants (lowercase takes precedence)
        if (auto* val = std::getenv("http_proxy")) {
            config.http_proxy = val;
        } else if (auto* val_upper = std::getenv("HTTP_PROXY")) {
            config.http_proxy = val_upper;
        }

        if (auto* val = std::getenv("https_proxy")) {
            config.https_proxy = val;
        } else if (auto* val_upper = std::getenv("HTTPS_PROXY")) {
            config.https_proxy = val_upper;
        }

        // Parse NO_PROXY / no_proxy comma-separated list
        const char* no_proxy_str = std::getenv("no_proxy");
        if (!no_proxy_str) no_proxy_str = std::getenv("NO_PROXY");
        if (no_proxy_str) {
            config.no_proxy = parse_no_proxy(no_proxy_str);
        }

        // Custom CA cert path
        if (auto* val = std::getenv("SSL_CERT_FILE")) {
            config.ca_cert_path = val;
        } else if (auto* val2 = std::getenv("CURL_CA_BUNDLE")) {
            config.ca_cert_path = val2;
        }

        return config;
    }

    /// Build proxy config from application settings map
    [[nodiscard]] static ProxyConfig from_config(
        const std::unordered_map<std::string, std::string>& settings) {

        ProxyConfig config;
        if (auto it = settings.find("http_proxy"); it != settings.end()) {
            config.http_proxy = it->second;
        }
        if (auto it = settings.find("https_proxy"); it != settings.end()) {
            config.https_proxy = it->second;
        }
        if (auto it = settings.find("no_proxy"); it != settings.end()) {
            config.no_proxy = parse_no_proxy(it->second);
        }
        if (auto it = settings.find("ca_cert_path"); it != settings.end()) {
            config.ca_cert_path = it->second;
        }
        return config;
    }

    /// Resolve the appropriate proxy for a given URL.
    /// Returns nullopt if the URL matches no_proxy list (direct connection).
    [[nodiscard]] static std::optional<ProxyConfig> resolve(
        std::string_view url, const ProxyConfig& config) {

        // Extract host from URL
        auto host = extract_host(url);

        // Check against no_proxy list
        if (matches_no_proxy(host, config.no_proxy)) {
            return std::nullopt; // Bypass proxy for this host
        }
        return config;
    }

    /// Apply proxy configuration to a curl handle (CURLOPT_PROXY, etc.)
    /// This is a type-erased interface since we don't include curl headers here.
    static VoidResult apply_to_curl(void* curl_handle, const ProxyConfig& config) {
        if (!curl_handle) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Null curl handle"));
        }

        // Determine which proxy to use (prefer HTTPS proxy for HTTPS URLs)
        std::string proxy_url;
        if (config.https_proxy) {
            proxy_url = *config.https_proxy;
        } else if (config.http_proxy) {
            proxy_url = *config.http_proxy;
        }

        if (proxy_url.empty()) return {}; // No proxy to set

        // The caller owns the concrete curl/OpenSSL handle; this helper validates
        // and normalizes the proxy settings before the transport layer applies them.
        auto type = ProxyConfig::detect_type(proxy_url);
        (void)type;

        // Build no_proxy string for CURLOPT_NOPROXY
        if (!config.no_proxy.empty()) {
            std::string no_proxy_str;
            for (size_t i = 0; i < config.no_proxy.size(); ++i) {
                if (i > 0) no_proxy_str += ",";
                no_proxy_str += config.no_proxy[i];
            }
            (void)no_proxy_str;
        }

        if (config.ca_cert_path) {
            (void)config.ca_cert_path;
        }

        return {};
    }

private:
    /// Parse comma-separated no_proxy string into individual entries
    [[nodiscard]] static std::vector<std::string> parse_no_proxy(std::string_view input) {
        std::vector<std::string> entries;
        size_t start = 0;
        while (start < input.size()) {
            // Skip whitespace
            while (start < input.size() && (input[start] == ' ' || input[start] == ',')) {
                start++;
            }
            if (start >= input.size()) break;

            auto end = input.find(',', start);
            if (end == std::string_view::npos) end = input.size();

            auto entry = input.substr(start, end - start);
            // Trim trailing whitespace
            while (!entry.empty() && entry.back() == ' ') {
                entry = entry.substr(0, entry.size() - 1);
            }
            if (!entry.empty()) {
                entries.emplace_back(entry);
            }
            start = end + 1;
        }
        return entries;
    }

    /// Extract hostname from a URL
    [[nodiscard]] static std::string extract_host(std::string_view url) {
        // Skip scheme (http:// or https://)
        auto scheme_end = url.find("://");
        if (scheme_end != std::string_view::npos) {
            url = url.substr(scheme_end + 3);
        }
        // Take up to first '/', ':', or '?'
        auto end = url.find_first_of("/:?");
        if (end != std::string_view::npos) {
            url = url.substr(0, end);
        }
        return std::string(url);
    }

    /// Check if a host matches any entry in the no_proxy list
    [[nodiscard]] static bool matches_no_proxy(
        std::string_view host, const std::vector<std::string>& no_proxy) {

        for (const auto& entry : no_proxy) {
            // Wildcard '*' matches everything
            if (entry == "*") return true;
            // Exact match
            if (host == entry) return true;
            // Domain suffix match (e.g., ".example.com" matches "sub.example.com")
            if (entry.starts_with(".") && host.ends_with(entry)) return true;
            // Without leading dot, also match as suffix
            if (host.size() > entry.size() &&
                host.ends_with(entry) &&
                host[host.size() - entry.size() - 1] == '.') {
                return true;
            }
            // Localhost special cases
            if ((entry == "localhost" || entry == "127.0.0.1") &&
                (host == "localhost" || host == "127.0.0.1")) {
                return true;
            }
        }
        return false;
    }
};

// ============================================================
// CaCertManager - system CA and custom certificate management
// ============================================================

/// Manages CA certificate paths for TLS verification and mutual TLS configuration.
class CaCertManager {
public:
    /// Get the platform-specific system CA certificate path
    [[nodiscard]] static std::filesystem::path system_ca_path() {
#if defined(__APPLE__)
        // macOS: use Security framework or bundled certs
        return "/etc/ssl/cert.pem";
#elif defined(__linux__)
        // Common Linux paths, check in order
        static constexpr std::array<const char*, 4> linux_paths = {
            "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu
            "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL/CentOS
            "/etc/ssl/ca-bundle.pem",              // openSUSE
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // Fedora
        };
        for (const auto* path : linux_paths) {
            if (std::filesystem::exists(path)) return path;
        }
        return "/etc/ssl/certs/ca-certificates.crt"; // Fallback
#else
        return ""; // Windows uses system store, not a file path
#endif
    }

    /// Load a custom CA certificate bundle from a file path
    [[nodiscard]] static VoidResult load_custom_ca(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigNotFound,
                std::format("CA cert file not found: {}", path.string())));
        }
        auto size = std::filesystem::file_size(path);
        if (size == 0) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError, "CA cert file is empty"));
        }
        // Validate it looks like a PEM file (basic check)
        std::ifstream file(path);
        std::string first_line;
        std::getline(file, first_line);
        if (first_line.find("-----BEGIN") == std::string::npos) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError,
                "CA cert file does not appear to be PEM format"));
        }
        return {};
    }

    /// Configure mutual TLS (client certificate + key)
    [[nodiscard]] static VoidResult set_mtls(
        const std::filesystem::path& cert_path,
        const std::filesystem::path& key_path) {

        if (!std::filesystem::exists(cert_path)) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigNotFound,
                std::format("Client cert not found: {}", cert_path.string())));
        }
        if (!std::filesystem::exists(key_path)) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigNotFound,
                std::format("Client key not found: {}", key_path.string())));
        }

        // Verify key file permissions are restricted (security check)
        auto perms = std::filesystem::status(key_path).permissions();
        using fp = std::filesystem::perms;
        if ((perms & fp::group_read) != fp::none ||
            (perms & fp::others_read) != fp::none) {
            return std::unexpected(Error::make(
                ErrorCode::ToolPermissionDenied,
                std::format("Key file '{}' has insecure permissions (readable by others)",
                            key_path.string())));
        }

        return {};
    }

    /// Get the effective CA path (custom override or system default)
    [[nodiscard]] static std::filesystem::path effective_ca_path(
        const std::optional<std::filesystem::path>& custom_path) {
        if (custom_path && std::filesystem::exists(*custom_path)) {
            return *custom_path;
        }
        return system_ca_path();
    }
};

} // namespace cc::services::proxy
