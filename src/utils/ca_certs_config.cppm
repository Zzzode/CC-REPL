module;

#include <string>
#include <optional>
#include <filesystem>
#include <cstdlib>
#include <mutex>

export module cc.utils.ca_certs_config;

export namespace cc::utils {

namespace fs = std::filesystem;

namespace detail {

// Custom CA bundle path set at runtime
inline std::optional<fs::path>& custom_ca_bundle() {
    static std::optional<fs::path> path;
    return path;
}

inline std::mutex& ca_mutex() {
    static std::mutex mtx;
    return mtx;
}

} // namespace detail

// Get the system CA certificate bundle path (platform-specific)
inline fs::path get_system_ca_path() {
#ifdef __APPLE__
    // macOS: use the system keychain or bundled certs
    // Common locations for CA bundles on macOS
    if (fs::exists("/etc/ssl/cert.pem")) return "/etc/ssl/cert.pem";
    if (fs::exists("/usr/local/etc/openssl/cert.pem")) return "/usr/local/etc/openssl/cert.pem";
    if (fs::exists("/opt/homebrew/etc/openssl/cert.pem")) return "/opt/homebrew/etc/openssl/cert.pem";
    return "/etc/ssl/cert.pem"; // Fallback
#else
    // Linux: common CA bundle locations
    if (fs::exists("/etc/ssl/certs/ca-certificates.crt")) return "/etc/ssl/certs/ca-certificates.crt";
    if (fs::exists("/etc/pki/tls/certs/ca-bundle.crt")) return "/etc/pki/tls/certs/ca-bundle.crt";
    if (fs::exists("/etc/ssl/ca-bundle.pem")) return "/etc/ssl/ca-bundle.pem";
    if (fs::exists("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"))
        return "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem";
    return "/etc/ssl/certs/ca-certificates.crt"; // Fallback
#endif
}

// Get the CA bundle path to use (checks env vars and custom overrides)
inline std::optional<fs::path> get_ca_bundle_path() {
    // 1. Check custom override
    {
        std::lock_guard lock(detail::ca_mutex());
        if (detail::custom_ca_bundle()) {
            if (fs::exists(*detail::custom_ca_bundle())) {
                return *detail::custom_ca_bundle();
            }
        }
    }

    // 2. Check NODE_EXTRA_CA_CERTS (commonly used in Node.js ecosystem)
    if (const char* extra = std::getenv("NODE_EXTRA_CA_CERTS")) {
        fs::path p(extra);
        if (fs::exists(p)) return p;
    }

    // 3. Check SSL_CERT_FILE
    if (const char* cert_file = std::getenv("SSL_CERT_FILE")) {
        fs::path p(cert_file);
        if (fs::exists(p)) return p;
    }

    // 4. Check SSL_CERT_DIR (return the directory itself)
    if (const char* cert_dir = std::getenv("SSL_CERT_DIR")) {
        fs::path p(cert_dir);
        if (fs::exists(p) && fs::is_directory(p)) return p;
    }

    // 5. Fall back to system CA path
    auto system_path = get_system_ca_path();
    if (fs::exists(system_path)) return system_path;

    return std::nullopt;
}

// Set a custom CA bundle path for the application
inline void set_custom_ca_bundle(const fs::path& path) {
    std::lock_guard lock(detail::ca_mutex());
    detail::custom_ca_bundle() = path;
}

} // namespace cc::utils
