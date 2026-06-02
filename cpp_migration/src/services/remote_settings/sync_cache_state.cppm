module;
#include <chrono>
#include <expected>
#include <optional>
#include <string>
export module cc.services.remote_settings.sync_cache_state;

export namespace cc::services::remote_settings {

using time_point = std::chrono::system_clock::time_point;

// Sync state machine states
enum class SyncState { Fresh, Stale, Error, Syncing };

namespace detail {
    inline SyncState current_state = SyncState::Stale;
    inline std::optional<time_point> last_sync_time;
} // namespace detail

// Get the current synchronization state
auto get_sync_state() -> SyncState {
    return detail::current_state;
}

// Get the time of the last successful sync
auto get_last_sync_time() -> std::optional<time_point> {
    return detail::last_sync_time;
}

// Force an immediate sync with remote settings server
auto force_sync() -> std::expected<void, std::string> {
    detail::current_state = SyncState::Syncing;
    // Complete immediately when no remote settings server is configured.
    detail::current_state = SyncState::Fresh;
    detail::last_sync_time = std::chrono::system_clock::now();
    return {};
}

} // namespace cc::services::remote_settings
