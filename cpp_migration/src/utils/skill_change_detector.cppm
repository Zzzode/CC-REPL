// Watch skill definition files for changes, notify on skill updates.
// Migrated from src/utils/skills/skillChangeDetector.ts
module;

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.skill_change_detector;

export namespace cc::utils::skill_change_detector {

using namespace std::chrono;
using UnsubscribeFn = std::function<void()>;
using SkillChangeCallback = std::function<void()>;

// --- Timing Constants ---

/// Time to wait for file writes to stabilize before processing.
inline constexpr auto kFileStabilityThreshold = milliseconds{1000};

/// Polling interval for checking file stability.
inline constexpr auto kFileStabilityPollInterval = milliseconds{500};

/// Debounce rapid skill change events into a single reload.
/// Prevents cascading reloads when many skill files change at once
/// (e.g. during auto-update or git operations).
inline constexpr auto kReloadDebounce = milliseconds{300};

/// Polling interval when usePolling is enabled (rare changes → less stat()).
inline constexpr auto kPollingInterval = milliseconds{2000};

// --- Test Override Configuration ---

struct TimingOverrides {
    std::optional<milliseconds> stability_threshold;
    std::optional<milliseconds> poll_interval;
    std::optional<milliseconds> reload_debounce;
    std::optional<milliseconds> chokidar_interval;
};

// --- Skill Change Detector Interface ---

/// Opaque handle to the skill change detector instance.
class SkillChangeDetector {
public:
    virtual ~SkillChangeDetector() = default;

    /// Initialize file watching for skill directories.
    /// Idempotent: subsequent calls are no-ops.
    virtual std::expected<void, std::string> initialize() = 0;

    /// Clean up file watcher resources.
    virtual std::expected<void, std::string> dispose() = 0;

    /// Subscribe to skill change notifications.
    /// Returns an unsubscribe function.
    [[nodiscard]] virtual UnsubscribeFn subscribe(SkillChangeCallback cb) = 0;

    /// Reset internal state for testing purposes.
    virtual std::expected<void, std::string> reset_for_testing() = 0;
    virtual std::expected<void, std::string> reset_for_testing(
        const TimingOverrides& overrides) = 0;
};

// --- Factory ---

/// Create the global skill change detector.
[[nodiscard]] std::unique_ptr<SkillChangeDetector> create_skill_change_detector();

// --- Module-level convenience (mirrors TS export shape) ---

/// Initialize file watching for skill directories.
std::expected<void, std::string> initialize();

/// Clean up file watcher.
std::expected<void, std::string> dispose();

/// Subscribe to skill changes. Returns unsubscribe function.
[[nodiscard]] UnsubscribeFn subscribe(SkillChangeCallback cb);

/// Reset internal state for testing purposes.
std::expected<void, std::string> reset_for_testing();
std::expected<void, std::string> reset_for_testing(
    const TimingOverrides& overrides);

} // namespace cc::utils::skill_change_detector
