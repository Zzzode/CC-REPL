// C++23 module: GrowthBook client keys for feature flag evaluation.
// Returns appropriate SDK key based on user type and dev mode settings.
module;
#include <string>
#include <string_view>

export module cc.constants.keys;


export namespace cc::constants::keys {

// SDK keys for different environments
inline constexpr std::string_view growthbook_key_ant_dev = "sdk-yZQvlplybuXjYh6L";
inline constexpr std::string_view growthbook_key_ant_prod = "sdk-xRVcrliHIlrg4og4";
inline constexpr std::string_view growthbook_key_external = "sdk-zAZezfDKGoZuXXKe";

enum class UserType {
    ant,
    external
};

// Returns the appropriate GrowthBook client key based on user type and dev mode
inline constexpr std::string_view get_growthbook_client_key(
    UserType user_type,
    bool enable_dev = false
) {
    if (user_type == UserType::ant) {
        return enable_dev ? growthbook_key_ant_dev : growthbook_key_ant_prod;
    }
    return growthbook_key_external;
}

} // namespace cc::constants::keys
