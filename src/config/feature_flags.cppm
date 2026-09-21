/// @file feature_flags.cppm
/// @brief Feature flag system with compile-time and runtime control.
/// Provides both constexpr dead-code elimination and dynamic runtime toggling
/// from environment variables or JSON configuration.
module;

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <utility>
#include <optional>
#include <format>
#include <algorithm>
#include <ranges>

export module cc.config.feature_flags;

export namespace cc::core::flags {

// ============================================================
// Feature enumeration - all known feature flags
// ============================================================

enum class Feature : std::uint32_t {
    Proactive = 0,
    Kairos,
    BridgeMode,
    Daemon,
    VoiceMode,
    AgentTriggers,
    MonitorTool,
    Templates,
    BgSessions,
    ByocRunner,
    SelfHostedRunner,
    CoordinatorMode,
    WebBrowserTool,
    TerminalPanel,
    HistorySnip,
    Ultraplan,
    UltraReview,
    Torch,
    ForkSubagent,
    Buddy,
    // Sentinel for total count
    _Count,
};

inline constexpr std::size_t FEATURE_COUNT = static_cast<std::size_t>(Feature::_Count);

/// Feature metadata for display and configuration
struct FeatureInfo {
    Feature feature;
    std::string_view name;       // Machine-readable name (used in env vars)
    std::string_view display;    // Human-readable description
    bool default_enabled;        // Default state at startup
};

/// Static feature registry with compile-time metadata
inline constexpr std::array<FeatureInfo, FEATURE_COUNT> FEATURE_REGISTRY = {{
    {Feature::Proactive,       "PROACTIVE",          "Proactive suggestions",          false},
    {Feature::Kairos,          "KAIROS",             "Kairos timing system",           false},
    {Feature::BridgeMode,      "BRIDGE_MODE",        "IDE bridge integration",         false},
    {Feature::Daemon,          "DAEMON",             "Background daemon mode",         false},
    {Feature::VoiceMode,       "VOICE_MODE",         "Voice input support",            false},
    {Feature::AgentTriggers,   "AGENT_TRIGGERS",     "Automatic agent triggering",     false},
    {Feature::MonitorTool,     "MONITOR_TOOL",       "System monitoring tool",         false},
    {Feature::Templates,       "TEMPLATES",          "Template system",                false},
    {Feature::BgSessions,      "BG_SESSIONS",        "Background sessions",            false},
    {Feature::ByocRunner,      "BYOC_RUNNER",        "BYOC environment runner",        false},
    {Feature::SelfHostedRunner,"SELF_HOSTED_RUNNER",  "Self-hosted runner",             false},
    {Feature::CoordinatorMode, "COORDINATOR_MODE",   "Multi-agent coordinator",        false},
    {Feature::WebBrowserTool,  "WEB_BROWSER_TOOL",   "Web browser tool",               false},
    {Feature::TerminalPanel,   "TERMINAL_PANEL",     "Terminal panel UI",              false},
    {Feature::HistorySnip,     "HISTORY_SNIP",       "History snippet extraction",     false},
    {Feature::Ultraplan,       "ULTRAPLAN",          "Ultra planning mode",            false},
    {Feature::UltraReview,     "ULTRAREVIEW",        "Ultra deep code review",         false},
    {Feature::Torch,           "TORCH",              "Torch debugging mode",           false},
    {Feature::ForkSubagent,    "FORK_SUBAGENT",      "Fork sub-agent support",         false},
    {Feature::Buddy,           "BUDDY",              "Buddy pair programming mode",    false},
}};

// ============================================================
// Compile-time feature check (dead-code elimination)
// ============================================================

/// Compile-time feature enabled check. Override via build-time defines.
/// Default: all features disabled unless explicitly turned on.
consteval bool feature_enabled([[maybe_unused]] Feature f) noexcept {
    // In a real build system, these would be controlled by -D flags.
    // Here we provide a consteval interface that the build system can override.
    return false;
}

/// Compile-time conditional execution (replaces IF_FEATURE macro)
/// Usage:
///   if constexpr (is_feature_active<Feature::VoiceMode>) { ... }
template <Feature F>
inline constexpr bool is_feature_active = feature_enabled(F);

// ============================================================
// FeatureFlagManager - runtime feature flag control
// ============================================================

class FeatureFlagManager {
public:
    FeatureFlagManager() {
        // Initialize with defaults from the registry
        for (const auto& info : FEATURE_REGISTRY) {
            flags_[static_cast<std::size_t>(info.feature)] = info.default_enabled;
        }
    }

    /// Check if a feature is enabled at runtime
    [[nodiscard]] bool is_enabled(Feature feature) const noexcept {
        auto idx = static_cast<std::size_t>(feature);
        return idx < FEATURE_COUNT && flags_[idx];
    }

    /// Enable a feature at runtime
    void enable(Feature feature) noexcept {
        auto idx = static_cast<std::size_t>(feature);
        if (idx < FEATURE_COUNT) flags_[idx] = true;
    }

    /// Disable a feature at runtime
    void disable(Feature feature) noexcept {
        auto idx = static_cast<std::size_t>(feature);
        if (idx < FEATURE_COUNT) flags_[idx] = false;
    }

    /// Read feature flags from environment variables.
    /// Looks for CC_FEATURE_<NAME>=1|0|true|false
    void set_from_env() {
        for (const auto& info : FEATURE_REGISTRY) {
            auto env_name = std::format("CC_FEATURE_{}", info.name);
            if (const char* val = std::getenv(env_name.c_str())) {
                std::string_view sv(val);
                bool enabled = (sv == "1" || sv == "true" || sv == "yes");
                flags_[static_cast<std::size_t>(info.feature)] = enabled;
            }
        }
    }

    /// Set flags from a JSON-like key-value configuration.
    /// Accepts a vector of (feature_name, enabled) pairs.
    void set_from_config(const std::vector<std::pair<std::string_view, bool>>& config) {
        for (const auto& [name, enabled] : config) {
            if (auto feature = find_by_name(name); feature.has_value()) {
                flags_[static_cast<std::size_t>(*feature)] = enabled;
            }
        }
    }

    /// Get all flags with their current state
    [[nodiscard]] std::vector<std::pair<Feature, bool>> all_flags() const {
        std::vector<std::pair<Feature, bool>> result;
        result.reserve(FEATURE_COUNT);
        for (std::size_t i = 0; i < FEATURE_COUNT; ++i) {
            result.emplace_back(static_cast<Feature>(i), flags_[i]);
        }
        return result;
    }

    /// Get the info for a specific feature
    [[nodiscard]] static const FeatureInfo& info(Feature feature) {
        return FEATURE_REGISTRY[static_cast<std::size_t>(feature)];
    }

    /// Find a feature by its string name
    [[nodiscard]] static std::optional<Feature> find_by_name(std::string_view name) {
        for (const auto& info : FEATURE_REGISTRY) {
            if (info.name == name) return info.feature;
        }
        return std::nullopt;
    }

    /// Get a summary string of enabled features
    [[nodiscard]] std::string enabled_summary() const {
        std::string result;
        for (std::size_t i = 0; i < FEATURE_COUNT; ++i) {
            if (flags_[i]) {
                if (!result.empty()) result += ", ";
                result += FEATURE_REGISTRY[i].name;
            }
        }
        return result.empty() ? "(none)" : result;
    }

    /// Reset all flags to their default values
    void reset_to_defaults() {
        for (const auto& info : FEATURE_REGISTRY) {
            flags_[static_cast<std::size_t>(info.feature)] = info.default_enabled;
        }
    }

    /// Toggle a feature flag
    void toggle(Feature feature) noexcept {
        auto idx = static_cast<std::size_t>(feature);
        if (idx < FEATURE_COUNT) flags_[idx] = !flags_[idx];
    }

    /// Count of currently enabled features
    [[nodiscard]] std::size_t enabled_count() const noexcept {
        std::size_t count = 0;
        for (std::size_t i = 0; i < FEATURE_COUNT; ++i) {
            if (flags_[i]) ++count;
        }
        return count;
    }

    /// Sync feature flags from a remote source (e.g., GrowthBook).
    /// Accepts a callable that returns std::optional<bool> for each feature name.
    /// If the callable returns std::nullopt, the flag retains its current value.
    template <typename FlagResolver>
    void sync_from_remote(FlagResolver&& resolver) {
        for (const auto& info : FEATURE_REGISTRY) {
            auto result = resolver(info.name);
            if (result.has_value()) {
                flags_[static_cast<std::size_t>(info.feature)] = *result;
            }
        }
    }

    /// Set a flag by its string name. Returns true if the flag was found.
    bool set_by_name(std::string_view name, bool enabled) {
        if (auto feature = find_by_name(name); feature.has_value()) {
            flags_[static_cast<std::size_t>(*feature)] = enabled;
            return true;
        }
        return false;
    }

private:
    std::array<bool, FEATURE_COUNT> flags_{};
};

// ============================================================
// Global singleton accessor (lazy initialization)
// ============================================================

/// Get the global feature flag manager instance
[[nodiscard]] inline FeatureFlagManager& global_flags() {
    static FeatureFlagManager instance;
    return instance;
}

/// Convenience: check if a feature is enabled globally at runtime
[[nodiscard]] inline bool is_enabled(Feature feature) {
    return global_flags().is_enabled(feature);
}

// ============================================================
// IF_FEATURE constexpr helper (replacing macro pattern)
// ============================================================

/// Executes fn only if the feature is enabled at runtime.
/// For compile-time elimination, use `if constexpr (is_feature_active<F>)` instead.
template <Feature F, typename Fn>
inline void if_feature(Fn&& fn) {
    if (global_flags().is_enabled(F)) {
        std::forward<Fn>(fn)();
    }
}

/// Executes fn_enabled if feature is on, fn_disabled otherwise.
template <Feature F, typename FnEnabled, typename FnDisabled>
inline auto if_feature_else(FnEnabled&& fn_enabled, FnDisabled&& fn_disabled) {
    if (global_flags().is_enabled(F)) {
        return std::forward<FnEnabled>(fn_enabled)();
    } else {
        return std::forward<FnDisabled>(fn_disabled)();
    }
}

} // namespace cc::core::flags
