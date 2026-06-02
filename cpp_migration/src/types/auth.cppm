/// @file auth.cppm
/// @brief Authentication context types from protobuf definition.
/// Migrated from: src/types/generated/events_mono/common/v1/auth.ts
module;

#include <cstdint>
#include <optional>
#include <string>

export module cc.types.auth;

export namespace cc::types::auth {

/// PublicApiAuth contains authentication context automatically injected by the API
struct PublicApiAuth {
    std::optional<int64_t> account_id;
    std::optional<std::string> organization_uuid;
    std::optional<std::string> account_uuid;
};

/// Create a default-initialized PublicApiAuth instance
[[nodiscard]] inline PublicApiAuth create_default_public_api_auth() {
    return PublicApiAuth{
        .account_id = 0,
        .organization_uuid = "",
        .account_uuid = "",
    };
}

} // namespace cc::types::auth
