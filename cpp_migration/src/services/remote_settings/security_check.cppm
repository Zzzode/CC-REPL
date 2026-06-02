module;
#include <map>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.remote_settings.security_check;

export namespace cc::services::remote_settings {

namespace detail {
    // Settings that cannot be overridden remotely
    inline const std::vector<std::string> locked_setting_keys = {
        "auth.token",
        "auth.api_key",
        "security.allowed_commands",
        "security.sandbox_mode",
    };
} // namespace detail

// Check if a setting key can be overridden by remote configuration
auto is_setting_overrideable(std::string_view key) -> bool {
    for (const auto& locked : detail::locked_setting_keys) {
        if (locked == key) return false;
    }
    return true;
}

// Validate remote settings for security issues
auto validate_remote_settings(const std::map<std::string, std::string>& settings)
    -> std::vector<std::string> {
    std::vector<std::string> warnings;

    for (const auto& [key, value] : settings) {
        // Warn if trying to override locked settings
        if (!is_setting_overrideable(key)) {
            warnings.push_back("Setting '" + key + "' is locked and cannot be overridden remotely");
        }
        // Warn about suspicious values
        if (value.find("eval(") != std::string::npos ||
            value.find("exec(") != std::string::npos) {
            warnings.push_back("Setting '" + key + "' contains potentially unsafe value");
        }
    }

    return warnings;
}

// Get list of settings that are locked from remote override
auto get_locked_settings() -> std::vector<std::string> {
    return detail::locked_setting_keys;
}

} // namespace cc::services::remote_settings
