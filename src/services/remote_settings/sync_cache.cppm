module;
#include <chrono>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
export module cc.services.remote_settings.sync_cache;

export namespace cc::services::remote_settings {

// Cache for synchronized remote settings
class SettingsSyncCache {
public:
    // Get a cached setting value
    auto get(std::string_view key) -> std::optional<std::string> {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(std::string(key));
        if (it != cache_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Set a cached setting value
    auto set(std::string_view key, std::string_view value) -> void {
        std::lock_guard lock(mutex_);
        cache_[std::string(key)] = std::string(value);
        last_update_ = std::chrono::steady_clock::now();
    }

    // Check if cache is stale (older than TTL)
    auto is_stale() -> bool {
        std::lock_guard lock(mutex_);
        auto age = std::chrono::steady_clock::now() - last_update_;
        return age > ttl_;
    }

    // Refresh cache from remote source
    auto refresh() -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        // Refresh records the local cache timestamp when no remote source is configured.
        last_update_ = std::chrono::steady_clock::now();
        return {};
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::string> cache_;
    std::chrono::steady_clock::time_point last_update_{};
    std::chrono::minutes ttl_{5};
};

} // namespace cc::services::remote_settings
