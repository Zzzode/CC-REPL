module;

#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <chrono>

export module cc.utils.aws_auth;

export namespace cc::utils::aws_auth {

enum class AuthStatus { Authenticated, Expired, NotConfigured, Error };

struct AWSCredentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::optional<std::string> session_token;
    std::optional<std::chrono::system_clock::time_point> expiry;
};

inline AuthStatus get_auth_status() {
    return AuthStatus::NotConfigured;
}

inline std::expected<AWSCredentials, std::string> get_credentials() {
    return std::unexpected("not configured");
}

inline std::expected<void, std::string> refresh_credentials() {
    return {};
}

inline bool is_aws_environment() {
    return false;
}

} // namespace cc::utils::aws_auth
