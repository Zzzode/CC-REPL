module;
#include <optional>
#include <string>
#include <string_view>

export module cc.hooks.notifs.auto_mode_unavailable;

export namespace cc::hooks::notifs {

// TS-parity stub mirroring src/hooks/notifs/useAutoModeUnavailableNotification.ts.
//
// That TS module is a React hook gated on feature('TRANSCRIPT_CLASSIFIER'). When
// the feature flag is OFF the hook early-returns and renders nothing. The
// subsystems the hook needs to do real work are NOT migrated to C++:
//   - React / Ink + useNotifications / useAppState (no UI runtime here)
//   - getIsRemoteMode / hasAutoModeOptIn
//   - getAutoModeUnavailableReason, which needs GrowthBook tengu_auto_mode_config
//     and modelSupportsAutoMode
// With the feature flag off, TS shows no notification — which is exactly what
// this stub achieves: check_auto_mode_available() returns true, so
// get_unavailable_reason() returns nullopt and show_...() is a no-op.
inline bool check_auto_mode_available() {
    return true;
}


inline std::optional<std::string> get_unavailable_reason() {
    if (check_auto_mode_available()) {
        return std::nullopt;
    }
    return "Auto mode requires explicit user consent and valid API key";
}


inline void show_auto_mode_unavailable_notification() {
    auto reason = get_unavailable_reason();
    if (reason.has_value()) {
        // No notification surface is wired in the C++ migration; the body is
        // intentionally empty (parity with the flag-off TS branch).
    }
}

} // namespace cc::hooks::notifs
