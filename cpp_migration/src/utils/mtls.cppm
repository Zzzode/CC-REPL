module;

#include <string>
#include <optional>
#include <expected>
#include <filesystem>
#include <cstdlib>

export module cc.utils.mtls;

export namespace cc::utils {

namespace fs = std::filesystem;

// Mutual TLS configuration
struct MtlsConfig {
    fs::path cert_file;
    fs::path key_file;
    std::optional<fs::path> ca_file;
};

// Get mTLS configuration from environment variables
inline std::optional<MtlsConfig> get_mtls_config() {
    const char* cert = std::getenv("CLAUDE_MTLS_CERT");
    const char* key = std::getenv("CLAUDE_MTLS_KEY");

    // Both cert and key are required
    if (!cert || !key) return std::nullopt;

    MtlsConfig config;
    config.cert_file = fs::path(cert);
    config.key_file = fs::path(key);

    // CA file is optional
    if (const char* ca = std::getenv("CLAUDE_MTLS_CA")) {
        config.ca_file = fs::path(ca);
    }

    return config;
}

// Validate that all mTLS config files exist and are readable
inline std::expected<void, std::string> validate_mtls_config(const MtlsConfig& config) {
    // Verify certificate file exists
    if (!fs::exists(config.cert_file)) {
        return std::unexpected("mTLS cert file not found: " + config.cert_file.string());
    }
    if (!fs::is_regular_file(config.cert_file)) {
        return std::unexpected("mTLS cert path is not a file: " + config.cert_file.string());
    }

    // Verify key file exists
    if (!fs::exists(config.key_file)) {
        return std::unexpected("mTLS key file not found: " + config.key_file.string());
    }
    if (!fs::is_regular_file(config.key_file)) {
        return std::unexpected("mTLS key path is not a file: " + config.key_file.string());
    }

    // Check key file permissions (should be restrictive)
    auto key_perms = fs::status(config.key_file).permissions();
    if ((key_perms & fs::perms::others_read) != fs::perms::none ||
        (key_perms & fs::perms::group_read) != fs::perms::none) {
        return std::unexpected("mTLS key file has overly permissive permissions: " +
                             config.key_file.string() + " (should be 0600)");
    }

    // Verify CA file if specified
    if (config.ca_file) {
        if (!fs::exists(*config.ca_file)) {
            return std::unexpected("mTLS CA file not found: " + config.ca_file->string());
        }
        if (!fs::is_regular_file(*config.ca_file)) {
            return std::unexpected("mTLS CA path is not a file: " + config.ca_file->string());
        }
    }

    return {};
}

} // namespace cc::utils
