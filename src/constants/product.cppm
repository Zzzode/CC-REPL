/// @file product.cppm
/// @brief Product URLs and remote session environment detection.
/// Migrated from src/constants/product.ts
module;
#include <cstdlib>

#include <string>
#include <string_view>
#include <optional>

export module cc.constants.product;

export namespace cc::constants::product {

/// Public documentation site, if the user has one. Empty by default: this
/// project ships no site, and the upstream vendor's URLs must not be
/// re-pointed at a host that does not resolve. Set LOOM_DOCS_BASE to your own
/// docs root and every help/about/doc link in the UI becomes live.
[[nodiscard]] inline std::string_view docs_base() {
    if (const char* base = std::getenv("LOOM_DOCS_BASE"); base && *base) {
        return base;
    }
    return "";
}

/// Build a documentation URL, or "" when no docs site is configured. Callers
/// should omit the link entirely rather than print an unfollowable path.
[[nodiscard]] inline std::string doc_url(std::string_view path) {
    const auto base = docs_base();
    if (base.empty()) return {};
    return std::string(base) + std::string(path);
}

inline constexpr std::string_view LOOM_VERSION = "1.0.0-cpp";
inline constexpr std::string_view BUILD_DATE = "unknown";
inline constexpr std::string_view BUILD_TIME = "unknown";
/// Hosted-product base URLs.
///
/// Empty by default: these were the upstream vendor's hosted endpoints, and
/// this build has no hosted service behind it. Set the matching environment
/// variable to point them at your own deployment; code that needs one should
/// check for empty and report "not configured" rather than dial a dead host.
[[nodiscard]] inline std::string_view product_url() {
    if (const char* v = std::getenv("LOOM_PRODUCT_URL"); v && *v) return v;
    return "";
}

[[nodiscard]] inline std::string_view remote_base_url() {
    if (const char* v = std::getenv("LOOM_REMOTE_API_BASE_URL"); v && *v) return v;
    return "";
}

/// Check if a session is in staging environment
[[nodiscard]] inline bool is_remote_session_staging(
    std::optional<std::string_view> session_id = std::nullopt,
    std::optional<std::string_view> ingress_url = std::nullopt
) {
    if (session_id && session_id->find("_staging_") != std::string_view::npos) return true;
    if (ingress_url && ingress_url->find("staging") != std::string_view::npos) return true;
    return false;
}

/// Check if a session is in local-dev environment
[[nodiscard]] inline bool is_remote_session_local(
    std::optional<std::string_view> session_id = std::nullopt,
    std::optional<std::string_view> ingress_url = std::nullopt
) {
    if (session_id && session_id->find("_local_") != std::string_view::npos) return true;
    if (ingress_url && ingress_url->find("localhost") != std::string_view::npos) return true;
    return false;
}

/// Get the base URL for Loom AI based on environment
[[nodiscard]] inline std::string get_remote_base_url(
    std::optional<std::string_view> session_id = std::nullopt,
    std::optional<std::string_view> ingress_url = std::nullopt
) {
    (void)session_id;
    (void)ingress_url;
    // One user-configurable base; no vendor hosts are baked in. Empty means
    // remote sessions are not configured, which callers surface as such.
    return std::string(remote_base_url());
}

/// Get the full session URL for a remote session
[[nodiscard]] inline std::string get_remote_session_url(
    std::string_view session_id,
    std::optional<std::string_view> ingress_url = std::nullopt
) {
    auto base = get_remote_base_url(session_id, ingress_url);
    return base + "/code/" + std::string(session_id);
}

} // namespace cc::constants::product
