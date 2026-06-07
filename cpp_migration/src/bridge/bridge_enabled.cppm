/// @file bridge_enabled.cppm
/// @brief Bridge feature gating — environment/config-based checks for Remote Control.
///
/// In the TypeScript codebase these checks query GrowthBook feature flags at
/// runtime.  The C++ migration replaces that with a simple BridgeFeatureFlags
/// struct populated from environment variables (and optionally a config file),
/// avoiding any remote dependency.

module;

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <charconv>
#include <array>

export module cc.bridge.bridge_enabled;

export namespace cc::bridge {

// ---------------------------------------------------------------------------
// Feature-flag container
// ---------------------------------------------------------------------------

/// Holds all bridge-related feature flags.  Populated by
/// `load_bridge_feature_flags()` from environment variables (or a future
/// config-file reader).  No remote GrowthBook dependency — everything is
/// local.
struct BridgeFeatureFlags {
    bool bridge_mode{false};          ///< CC_BRIDGE_MODE  (build-time on/off)
    bool bridge_enabled{false};       ///< CC_BRIDGE_ENABLED ("tengu_ccr_bridge")
    bool bridge_v2{false};            ///< CC_BRIDGE_V2 ("tengu_bridge_repl_v2")
    bool cse_shim{true};             ///< CC_BRIDGE_CSE_SHIM ("tengu_bridge_repl_v2_cse_shim_enabled")
    bool auto_connect{false};         ///< CC_BRIDGE_AUTO_CONNECT ("tengu_cobalt_harbor")
    bool mirror{false};               ///< CC_BRIDGE_MIRROR ("tengu_ccr_mirror")
    std::string min_version{"0.0.0"}; ///< Minimum CLI version for bridge
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

/// Read a boolean environment variable.  Returns `default_value` when the
/// variable is unset.  Recognises "1"/"true"/"yes" (case-insensitive) as
/// true and everything else as false.
[[nodiscard]] bool env_bool(const char* name, bool default_value) {
    const char* raw = std::getenv(name);
    if (!raw) return default_value;
    std::string_view val{raw};
    // Trim leading/trailing whitespace
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
        val.remove_prefix(1);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
        val.remove_suffix(1);
    if (val == "1" || val == "true" || val == "yes" || val == "TRUE" || val == "YES")
        return true;
    if (val == "0" || val == "false" || val == "no" || val == "FALSE" || val == "NO")
        return false;
    return default_value;
}

/// Read a string environment variable.  Returns `default_value` when unset.
[[nodiscard]] std::string env_string(const char* name, std::string_view default_value) {
    const char* raw = std::getenv(name);
    if (!raw) return std::string{default_value};
    return std::string{raw};
}

/// Parse a semantic version string (major.minor.patch) into a 3-element
/// array.  Returns std::nullopt on parse failure.
[[nodiscard]] std::optional<std::array<int, 3>> parse_semver(std::string_view ver) {
    std::array<int, 3> parts{};
    size_t idx = 0;
    size_t start = 0;

    for (size_t i = 0; i <= ver.size() && idx < 3; ++i) {
        if (i == ver.size() || ver[i] == '.') {
            auto segment = ver.substr(start, i - start);
            auto [end, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), parts[idx]);
            if (ec != std::errc{} || end != segment.data() + segment.size())
                return std::nullopt;
            ++idx;
            start = i + 1;
        }
    }

    if (idx != 3) return std::nullopt;
    return parts;
}

/// Return true if `lhs < rhs` in semver terms.
[[nodiscard]] bool semver_less(std::string_view lhs, std::string_view rhs) {
    auto a = parse_semver(lhs);
    auto b = parse_semver(rhs);
    if (!a || !b) return false; // unparseable → don't claim it's less
    for (int i = 0; i < 3; ++i) {
        if ((*a)[i] < (*b)[i]) return true;
        if ((*a)[i] > (*b)[i]) return false;
    }
    return false; // equal
}

/// Check if the user has a claude.ai subscription equivalent by looking for
/// a non-empty OAuth token in the environment.
[[nodiscard]] bool is_claude_ai_subscriber() {
    const char* token = std::getenv("CC_OAUTH_TOKEN");
    return token != nullptr && token[0] != '\0';
}

} // namespace detail

// ---------------------------------------------------------------------------
// Flag loading
// ---------------------------------------------------------------------------

/// Populate a BridgeFeatureFlags struct from the current environment.
[[nodiscard]] BridgeFeatureFlags load_bridge_feature_flags() {
    BridgeFeatureFlags flags;
    flags.bridge_mode   = detail::env_bool("CC_BRIDGE_MODE", false);
    flags.bridge_enabled = detail::env_bool("CC_BRIDGE_ENABLED", false);
    flags.bridge_v2     = detail::env_bool("CC_BRIDGE_V2", false);
    flags.cse_shim      = detail::env_bool("CC_BRIDGE_CSE_SHIM", true);
    flags.auto_connect  = detail::env_bool("CC_BRIDGE_AUTO_CONNECT", false);
    flags.mirror        = detail::env_bool("CC_BRIDGE_MIRROR", false);
    flags.min_version   = detail::env_string("CC_BRIDGE_MIN_VERSION", "0.0.0");
    return flags;
}

// ---------------------------------------------------------------------------
// Public query API
// ---------------------------------------------------------------------------

/// Check if bridge mode is available.  Requires the build-level
/// BRIDGE_MODE flag AND a claude.ai subscription (CC_OAUTH_TOKEN) AND the
/// tengu_ccr_bridge feature flag (CC_BRIDGE_ENABLED).
[[nodiscard]] bool is_bridge_enabled() {
    auto flags = load_bridge_feature_flags();
    if (!flags.bridge_mode) return false;
    if (!detail::is_claude_ai_subscriber()) return false;
    return flags.bridge_enabled;
}

/// Blocking check — in C++ there is no async GrowthBook call, so this is
/// identical to `is_bridge_enabled()`.
[[nodiscard]] bool is_bridge_enabled_blocking() {
    return is_bridge_enabled();
}

/// Returns a human-readable reason string if Remote Control is disabled,
/// or std::nullopt if it is enabled.  Call this at user-facing error
/// gates to give actionable diagnostics.
[[nodiscard]] std::optional<std::string> get_bridge_disabled_reason() {
    auto flags = load_bridge_feature_flags();

    if (!flags.bridge_mode) {
        return "Remote Control is not available in this build.";
    }
    if (!detail::is_claude_ai_subscriber()) {
        return "Remote Control requires a claude.ai subscription. "
               "Set CC_OAUTH_TOKEN or run `claude auth login` to sign in "
               "with your claude.ai account.";
    }
    if (!flags.bridge_enabled) {
        return "Remote Control is not yet enabled for your account.";
    }
    return std::nullopt;
}

/// Check if the env-less (v2) bridge REPL path is available.
/// This gates which bridge implementation is used — NOT whether bridge is
/// available at all (see `is_bridge_enabled()`).
[[nodiscard]] bool is_envless_bridge_enabled() {
    auto flags = load_bridge_feature_flags();
    if (!flags.bridge_mode) return false;
    return flags.bridge_v2;
}

/// Kill-switch for the cse_* -> session_* client-side retag shim.
/// Defaults to true — the shim stays active until explicitly disabled.
[[nodiscard]] bool is_cse_shim_enabled() {
    auto flags = load_bridge_feature_flags();
    if (!flags.bridge_mode) return true; // default-on outside bridge mode
    return flags.cse_shim;
}

/// Returns an error message if `current_version` is below the configured
/// minimum, or std::nullopt if the version is acceptable.
[[nodiscard]] std::optional<std::string> check_bridge_min_version(std::string_view current_version) {
    auto flags = load_bridge_feature_flags();
    if (!flags.bridge_mode) return std::nullopt;
    if (!flags.min_version.empty() && detail::semver_less(current_version, flags.min_version)) {
        return "Your version of Claude Code (" + std::string{current_version}
             + ") is too old for Remote Control.\nVersion " + flags.min_version
             + " or higher is required. Run `claude update` to update.";
    }
    return std::nullopt;
}

/// Default for the auto-connect-at-startup setting.  Requires both the
/// build-level CCR_AUTO_CONNECT flag (CC_BRIDGE_MODE) and the GrowthBook
/// gate (CC_BRIDGE_AUTO_CONNECT).
[[nodiscard]] bool get_ccr_auto_connect_default() {
    auto flags = load_bridge_feature_flags();
    if (!flags.bridge_mode) return false;
    return flags.auto_connect;
}

/// Opt-in CCR mirror mode — every local session spawns an outbound-only
/// Remote Control session that receives forwarded events.
[[nodiscard]] bool is_ccr_mirror_enabled() {
    auto flags = load_bridge_feature_flags();
    if (!flags.bridge_mode) return false;
    return flags.mirror;
}

} // namespace cc::bridge
