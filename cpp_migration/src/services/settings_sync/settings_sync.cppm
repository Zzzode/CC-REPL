/// @file settings_sync.cppm
/// @brief Settings synchronization service.
/// Syncs user settings across machines with conflict detection,
/// merge strategies, and change history tracking.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <mutex>

export module cc.services.settings_sync;

import cc.types.types;

export namespace cc::services::settings_sync {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================

// ============================================================


struct SettingValue {
    std::string key;
    std::string value_json;
    std::uint64_t version{0};
    std::string machine_id;
    TimePoint modified_at;
};


struct SettingsConflict {
    std::string key;
    SettingValue local;
    SettingValue remote;
};


enum class SettingsMergeStrategy : std::uint8_t {
    LastWriteWins,
    LocalWins,
    RemoteWins,
    Manual,
};


struct ChangeRecord {
    std::string key;
    std::string old_value_json;
    std::string new_value_json;
    std::string machine_id;
    TimePoint changed_at;
};


struct SyncSettingsResult {
    std::size_t pushed{0};
    std::size_t pulled{0};
    std::size_t conflicts{0};
    std::vector<SettingsConflict> unresolved;
    TimePoint synced_at;
};


struct SettingsSyncConfig {
    std::string machine_id;
    SettingsMergeStrategy merge_strategy{SettingsMergeStrategy::LastWriteWins};
    std::size_t max_history{100};
    bool auto_sync{false};
    std::chrono::seconds sync_interval{60};
};

// ============================================================

// ============================================================

class SettingsSyncService {
public:
    explicit SettingsSyncService(SettingsSyncConfig config = {})
        : config_(std::move(config)) {}


    VoidResult set(std::string key, std::string value_json) {
        std::lock_guard lock(mutex_);
        auto now = Clock::now();
        auto it = settings_.find(key);
        std::string old_value;
        std::uint64_t new_ver = 1;
        if (it != settings_.end()) {
            old_value = it->second.value_json;
            new_ver = it->second.version + 1;
        }
        settings_[key] = SettingValue{
            .key = key, .value_json = value_json,
            .version = new_ver, .machine_id = config_.machine_id,
            .modified_at = now,
        };

        history_.push_back({key, std::move(old_value), value_json, config_.machine_id, now});
        if (history_.size() > config_.max_history) {
            history_.erase(history_.begin());
        }
        return {};
    }


    [[nodiscard]] std::optional<std::string> get(const std::string& key) const {
        std::lock_guard lock(mutex_);
        auto it = settings_.find(key);
        if (it == settings_.end()) return std::nullopt;
        return it->second.value_json;
    }


    [[nodiscard]] std::expected<SyncSettingsResult, Error> sync(
        std::vector<SettingValue> remote_settings)
    {
        std::lock_guard lock(mutex_);
        SyncSettingsResult result;
        result.synced_at = Clock::now();

        for (auto& remote : remote_settings) {
            auto it = settings_.find(remote.key);
            if (it == settings_.end()) {
                settings_[remote.key] = std::move(remote);
                ++result.pulled;
            } else if (it->second.version != remote.version) {

                auto resolved = resolve_conflict(it->second, remote);
                if (resolved) {
                    it->second = std::move(*resolved);
                    ++result.conflicts;
                } else {
                    result.unresolved.push_back({remote.key, it->second, remote});
                }
            }
        }
        return result;
    }


    [[nodiscard]] std::vector<ChangeRecord> change_history() const {
        std::lock_guard lock(mutex_);
        return history_;
    }


    [[nodiscard]] std::vector<SettingsConflict> detect_conflicts(
        const std::vector<SettingValue>& remote) const
    {
        std::lock_guard lock(mutex_);
        std::vector<SettingsConflict> conflicts;
        for (const auto& r : remote) {
            auto it = settings_.find(r.key);
            if (it != settings_.end() && it->second.version != r.version &&
                it->second.value_json != r.value_json) {
                conflicts.push_back({r.key, it->second, r});
            }
        }
        return conflicts;
    }

    void set_config(SettingsSyncConfig config) noexcept { config_ = std::move(config); }
    [[nodiscard]] const SettingsSyncConfig& config() const noexcept { return config_; }

private:
    SettingsSyncConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SettingValue> settings_;
    std::vector<ChangeRecord> history_;


    std::optional<SettingValue> resolve_conflict(
        const SettingValue& local, const SettingValue& remote) const
    {
        switch (config_.merge_strategy) {
            case SettingsMergeStrategy::LastWriteWins:
                return (remote.modified_at > local.modified_at) ? remote : local;
            case SettingsMergeStrategy::LocalWins:
                return local;
            case SettingsMergeStrategy::RemoteWins:
                return remote;
            case SettingsMergeStrategy::Manual:
                return std::nullopt;
        }
        return std::nullopt;
    }
};

} // namespace cc::services::settings_sync
