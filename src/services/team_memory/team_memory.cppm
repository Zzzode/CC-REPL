/// @file team_memory.cppm
/// @brief Team memory synchronization service.
/// Syncs memories across team members with secret scanning, conflict resolution
/// (merge/overwrite), and version tracking capabilities.
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
#include <regex>
#include <functional>

export module cc.services.team_memory;

import cc.types.types;

export namespace cc::services::team_memory {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================

// ============================================================


struct VersionInfo {
    std::uint64_t version{0};
    std::string author;
    TimePoint modified_at;
    std::string change_summary;
};


struct TeamMemoryEntry {
    std::string id;
    std::string content;
    std::string owner;
    std::vector<std::string> shared_with;
    std::vector<VersionInfo> history;
    TimePoint created_at;
    bool is_deleted{false};
};


enum class ConflictType : std::uint8_t {
    ContentDiverged,
    DeleteVsModify,
    ConcurrentEdit,
};


struct SyncConflict {
    std::string entry_id;
    ConflictType type;
    TeamMemoryEntry local_version;
    TeamMemoryEntry remote_version;
};


enum class MergeStrategy : std::uint8_t {
    LocalWins,
    RemoteWins,
    Merge,
    Manual,
};


struct SyncResult {
    std::size_t entries_pushed{0};
    std::size_t entries_pulled{0};
    std::size_t conflicts_resolved{0};
    std::vector<SyncConflict> unresolved_conflicts;
    TimePoint synced_at;
};


struct SecretScanResult {
    bool has_secrets{false};
    std::vector<std::string> detected_patterns;
    std::vector<std::size_t> line_numbers;
};


struct TeamMemoryConfig {
    MergeStrategy default_strategy{MergeStrategy::RemoteWins};
    bool enable_secret_scanning{true};
    bool auto_sync{false};
    std::chrono::seconds sync_interval{300};
    std::size_t max_history_versions{50};
};

// ============================================================

// ============================================================

class TeamMemorySync {
public:
    explicit TeamMemorySync(TeamMemoryConfig config = {})
        : config_(config) {}


    [[nodiscard]] std::expected<std::string, Error> add_entry(TeamMemoryEntry entry) {

        if (config_.enable_secret_scanning) {
            auto scan = scan_for_secrets(entry.content);
            if (scan.has_secrets) {
                return std::unexpected(Error::make(
                    ErrorCode::PermissionDenied,
                    std::format("Secret detected: {}", scan.detected_patterns.front())));
            }
        }
        std::lock_guard lock(mutex_);
        auto id = entry.id.empty() ? generate_id() : entry.id;
        entry.id = id;
        entry.created_at = Clock::now();
        entry.history.push_back({
            .version = 1, .author = entry.owner,
            .modified_at = Clock::now(), .change_summary = "created",
        });
        entries_[id] = std::move(entry);
        return id;
    }


    VoidResult update_entry(const std::string& id, std::string new_content, std::string author) {
        if (config_.enable_secret_scanning) {
            auto scan = scan_for_secrets(new_content);
            if (scan.has_secrets) {
                return std::unexpected(Error::make(ErrorCode::PermissionDenied, "secret detected"));
            }
        }
        std::lock_guard lock(mutex_);
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            return std::unexpected(Error::make(ErrorCode::NotFound, "entry not found"));
        }
        auto& entry = it->second;
        entry.content = std::move(new_content);
        auto ver = entry.history.empty() ? 1 : entry.history.back().version + 1;
        entry.history.push_back({
            .version = ver, .author = std::move(author),
            .modified_at = Clock::now(), .change_summary = "updated",
        });

        if (entry.history.size() > config_.max_history_versions) {
            entry.history.erase(entry.history.begin());
        }
        return {};
    }


    [[nodiscard]] std::expected<SyncResult, Error> sync(
        std::vector<TeamMemoryEntry> remote_entries)
    {
        std::lock_guard lock(mutex_);
        SyncResult result;
        result.synced_at = Clock::now();

        for (auto& remote : remote_entries) {
            auto it = entries_.find(remote.id);
            if (it == entries_.end()) {

                entries_[remote.id] = std::move(remote);
                ++result.entries_pulled;
            } else {

                auto& local = it->second;
                if (has_conflict(local, remote)) {
                    auto conflict = SyncConflict{
                        .entry_id = remote.id,
                        .type = ConflictType::ContentDiverged,
                        .local_version = local,
                        .remote_version = remote,
                    };
                    if (config_.default_strategy == MergeStrategy::RemoteWins) {
                        local = std::move(remote);
                        ++result.conflicts_resolved;
                    } else if (config_.default_strategy == MergeStrategy::LocalWins) {
                        ++result.conflicts_resolved;
                    } else {
                        result.unresolved_conflicts.push_back(std::move(conflict));
                    }
                }
            }
        }
        return result;
    }


    [[nodiscard]] SecretScanResult scan_for_secrets(std::string_view content) const {
        SecretScanResult result;

        static const std::vector<std::pair<std::string, std::string>> patterns = {
            {"AWS Key", "AKIA[0-9A-Z]{16}"},
            {"Private Key", "-----BEGIN (RSA |EC )?PRIVATE KEY-----"},
            {"GitHub Token", "gh[ps]_[A-Za-z0-9_]{36,}"},
            {"Generic Secret", "(?i)(password|secret|token|api_key)\\s*[=:]\\s*['\"][^'\"]{8,}"},
        };
        for (const auto& [name, pattern] : patterns) {
            std::regex re(pattern);
            if (std::regex_search(content.begin(), content.end(), re)) {
                result.has_secrets = true;
                result.detected_patterns.push_back(name);
            }
        }
        return result;
    }


    [[nodiscard]] std::optional<TeamMemoryEntry> get_entry(const std::string& id) const {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return std::nullopt;
        return it->second;
    }


    [[nodiscard]] std::vector<TeamMemoryEntry> all_entries() const {
        std::lock_guard lock(mutex_);
        std::vector<TeamMemoryEntry> result;
        result.reserve(entries_.size());
        for (const auto& [_, entry] : entries_) {
            if (!entry.is_deleted) result.push_back(entry);
        }
        return result;
    }

    void set_config(TeamMemoryConfig config) noexcept { config_ = config; }
    [[nodiscard]] const TeamMemoryConfig& config() const noexcept { return config_; }

private:
    TeamMemoryConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TeamMemoryEntry> entries_;
    std::uint64_t id_counter_{0};

    std::string generate_id() { return std::format("tm_{}", ++id_counter_); }

    bool has_conflict(const TeamMemoryEntry& local, const TeamMemoryEntry& remote) const {
        if (local.history.empty() || remote.history.empty()) return false;
        return local.history.back().version != remote.history.back().version &&
               local.content != remote.content;
    }
};

} // namespace cc::services::team_memory
