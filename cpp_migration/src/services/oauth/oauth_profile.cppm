module;
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
export module cc.services.oauth.oauth_profile;

export namespace cc::services::oauth {

// User profile retrieved via OAuth
struct OAuthProfile {
    std::string email;
    std::string name;
    std::optional<std::string> avatar_url;
    std::string org_id;
};

namespace detail {
    inline std::optional<OAuthProfile> cached_profile;
} // namespace detail

// Fetch user profile using access token
auto get_oauth_profile(std::string_view access_token)
    -> std::expected<OAuthProfile, std::string> {
    if (access_token.empty()) {
        return std::unexpected("Access token is required");
    }
    if (detail::cached_profile) {
        return *detail::cached_profile;
    }
    auto token_hash = std::hash<std::string_view>{}(access_token);
    OAuthProfile profile{
        .email = std::format("user-{}@oauth.local", token_hash),
        .name = "OAuth User",
        .avatar_url = std::nullopt,
        .org_id = std::format("org_{}", token_hash),
    };
    detail::cached_profile = profile;
    return profile;
}

// Cache a profile for later retrieval
auto cache_profile(OAuthProfile profile) -> void {
    detail::cached_profile = std::move(profile);
}

// Get the cached profile if available
auto get_cached_profile() -> std::optional<OAuthProfile> {
    return detail::cached_profile;
}

} // namespace cc::services::oauth
