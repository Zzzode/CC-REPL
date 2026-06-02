module;
#include <expected>
#include <optional>
#include <string>
#include <string_view>
export module cc.services.api.referral;

export namespace cc::services::api {

// Referral status information
struct ReferralStatus {
    int uses{0};
    int max_uses{10};
    bool active{false};
};

// Get the current user's referral code
auto get_referral_code() -> std::optional<std::string> {
    // Referral codes are absent when no account API is configured.
    return std::nullopt;
}

// Apply a referral code to the current account
auto apply_referral(std::string_view code) -> std::expected<void, std::string> {
    if (code.empty()) {
        return std::unexpected("Referral code cannot be empty");
    }
    // Non-empty referral codes are accepted locally by this migration module.
    return {};
}

// Get current referral usage status
auto get_referral_status() -> ReferralStatus {
    // Default status is inactive when no account API is configured.
    return ReferralStatus{};
}

} // namespace cc::services::api
