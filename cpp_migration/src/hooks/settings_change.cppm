module;
#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <vector>
#include <chrono>
#include <mutex>

export module cc.hooks.settings_change;

export namespace cc::hooks {

// Source of the settings change (which config layer/file triggered it)
enum class SettingChangeSource {
    global_config,   // ~/.config/cc/settings.json changed
    project_config,  // .cc/settings.json changed
    env_variable,    // Environment variable changed
    runtime_set,     // Programmatic set() call
    file_watcher     // File system watcher detected change
};

// Convert source to display string
[[nodiscard]] constexpr auto setting_source_to_string(SettingChangeSource source) noexcept
    -> std::string_view {
    switch (source) {
        case SettingChangeSource::global_config:  return "global_config";
        case SettingChangeSource::project_config: return "project_config";
        case SettingChangeSource::env_variable:   return "env_variable";
        case SettingChangeSource::runtime_set:    return "runtime_set";
        case SettingChangeSource::file_watcher:   return "file_watcher";
    }
    return "unknown";
}

// Subscriber callback: receives the source that triggered the change
using SettingsChangeCallback = std::function<void(SettingChangeSource source)>;

// Unsubscribe handle
using UnsubscribeHandle = std::function<void()>;

/// Settings change detector and notification fan-out.
///
/// Watches for settings file modifications and notifies all subscribers
/// when a change is detected. Subscribers receive the source of the change
/// and can re-read the current settings state.
///
/// This is the C++ equivalent of useSettingsChange which subscribes to
/// settingsChangeDetector and re-reads settings on each notification.
class SettingsChangeDetector {
public:
    SettingsChangeDetector() = default;

    // Subscribe to settings change events.
    // Returns an unsubscribe handle.
    [[nodiscard]] auto subscribe(SettingsChangeCallback callback) -> UnsubscribeHandle {
        std::lock_guard lock(mu_);
        auto id = next_id_++;
        subscribers_.push_back({id, std::move(callback)});
        return [this, id]() { unsubscribe(id); };
    }

    // Notify all subscribers that settings have changed.
    // Called by the file watcher or runtime setter.
    auto notify(SettingChangeSource source) -> void {
        std::lock_guard lock(mu_);
        last_change_source_ = source;
        last_change_time_ = std::chrono::steady_clock::now();
        ++change_count_;

        for (const auto& [id, cb] : subscribers_) {
            if (cb) cb(source);
        }
    }

    // Get the number of active subscribers
    [[nodiscard]] auto subscriber_count() const -> std::size_t {
        std::lock_guard lock(mu_);
        return subscribers_.size();
    }

    // Get the last change source
    [[nodiscard]] auto last_source() const -> std::optional<SettingChangeSource> {
        std::lock_guard lock(mu_);
        return last_change_source_;
    }

    // Get total number of change notifications fired
    [[nodiscard]] auto change_count() const -> std::size_t {
        std::lock_guard lock(mu_);
        return change_count_;
    }

    // Check if any change happened within the given duration
    [[nodiscard]] auto changed_recently(std::chrono::milliseconds window) const -> bool {
        std::lock_guard lock(mu_);
        if (!last_change_time_) return false;
        auto elapsed = std::chrono::steady_clock::now() - *last_change_time_;
        return elapsed <= window;
    }

private:
    struct Subscriber {
        std::size_t id;
        SettingsChangeCallback callback;
    };

    mutable std::mutex mu_;
    std::vector<Subscriber> subscribers_;
    std::size_t next_id_{0};
    std::optional<SettingChangeSource> last_change_source_;
    std::optional<std::chrono::steady_clock::time_point> last_change_time_;
    std::size_t change_count_{0};

    auto unsubscribe(std::size_t id) -> void {
        std::lock_guard lock(mu_);
        std::erase_if(subscribers_, [id](const Subscriber& s) { return s.id == id; });
    }
};

// Global singleton accessor (mirrors the TS settingsChangeDetector export)
[[nodiscard]] inline auto get_settings_change_detector() -> SettingsChangeDetector& {
    static SettingsChangeDetector instance;
    return instance;
}

} // namespace cc::hooks
