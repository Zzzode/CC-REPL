/// @file product.cppm
/// @brief Product URLs and remote session environment detection.
/// Migrated from src/constants/product.ts
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.constants.product;

export namespace cc::constants::product {

inline constexpr std::string_view CC_REPL_VERSION = "1.0.0-cpp";
inline constexpr std::string_view BUILD_DATE = "unknown";
inline constexpr std::string_view BUILD_TIME = "unknown";
inline constexpr std::string_view PRODUCT_URL = "https://claude.com/claude-code";
inline constexpr std::string_view CLAUDE_AI_BASE_URL = "https://claude.ai";
inline constexpr std::string_view CLAUDE_AI_STAGING_BASE_URL = "https://claude-ai.staging.ant.dev";
inline constexpr std::string_view CLAUDE_AI_LOCAL_BASE_URL = "http://localhost:4000";

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

/// Get the base URL for Claude AI based on environment
[[nodiscard]] inline std::string get_claude_ai_base_url(
    std::optional<std::string_view> session_id = std::nullopt,
    std::optional<std::string_view> ingress_url = std::nullopt
) {
    if (is_remote_session_local(session_id, ingress_url)) {
        return std::string(CLAUDE_AI_LOCAL_BASE_URL);
    }
    if (is_remote_session_staging(session_id, ingress_url)) {
        return std::string(CLAUDE_AI_STAGING_BASE_URL);
    }
    return std::string(CLAUDE_AI_BASE_URL);
}

/// Get the full session URL for a remote session
[[nodiscard]] inline std::string get_remote_session_url(
    std::string_view session_id,
    std::optional<std::string_view> ingress_url = std::nullopt
) {
    auto base = get_claude_ai_base_url(session_id, ingress_url);
    return base + "/code/" + std::string(session_id);
}

} // namespace cc::constants::product
