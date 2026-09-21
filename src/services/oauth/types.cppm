// OAuth Types Module
module;
#include <chrono>
#include <optional>
#include <string>

export module cc.services.oauth.types;

import cc.utils.error;

export namespace cc::services::oauth {

using cc::utils::Result;

// OAuth tokens
struct OAuthTokens {
    std::string access_token;
    std::string refresh_token;
    int expires_in = 0;
    std::chrono::system_clock::time_point acquired_at;
    std::optional<std::string> token_type;
    std::optional<std::string> scope;
};

// OAuth profile
struct OAuthProfile {
    std::string id;
    std::string email;
    std::string name;
    // Add more fields
};

} // namespace cc::services::oauth
