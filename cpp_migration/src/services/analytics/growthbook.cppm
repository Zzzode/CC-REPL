/// @file growthbook.cppm
/// @brief GrowthBook experiment framework integration
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include <format>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <variant>
#include <cstdlib>

export module cc.services.analytics.growthbook;

export import cc.services.analytics;
export import cc.utils.json;
export import cc.utils.http;
export import cc.utils.crypto;

export namespace cc::services::analytics::growthbook {

using Clock = std::chrono::system_clock;
using Duration = std::chrono::milliseconds;

// ============================================================
// GrowthBook configuration
// ============================================================

struct GrowthBookConfig {
    std::string client_key;
    std::string api_host = "https://api.anthropic.com";
    bool enabled = true;
    Duration refresh_interval = Duration{6 * 60 * 60 * 1000}; // 6 hours
    Duration request_timeout = Duration{5000};
};

// ============================================================
// User attributes for targeting
// ============================================================

struct UserAttributes {
    std::string id;
    std::string session_id;
    std::string device_id;
    std::string platform;
    std::optional<std::string> api_base_url_host;
    std::optional<std::string> organization_uuid;
    std::optional<std::string> account_uuid;
    std::optional<std::string> user_type;
    std::optional<std::string> subscription_type;
    std::optional<std::string> rate_limit_tier;
    std::optional<std::int64_t> first_token_time;
    std::optional<std::string> email;
    std::optional<std::string> app_version;
};

// ============================================================
// Experiment data
// ============================================================

struct ExperimentData {
    std::string experiment_id;
    std::int64_t variation_id;
    bool in_experiment = true;
    std::optional<std::string> hash_attribute;
    std::optional<std::string> hash_value;
};

// ============================================================
// Feature value types
// ============================================================

using FeatureValue = std::variant<
    bool,
    std::int64_t,
    double,
    std::string,
    std::nullptr_t
>;

// ============================================================
// Refresh listener callback
// ============================================================

using RefreshCallback = std::function<void()>;

// ============================================================
// GrowthBook client
// ============================================================

class GrowthBookClient {
public:
    explicit GrowthBookClient(GrowthBookConfig config)
        : config_(std::move(config)),
          initialized_(false),
          refreshing_(false) {}

    ~GrowthBookClient() = default;

    /// Initialize with user attributes
    [[nodiscard]] bool initialize(const UserAttributes& attributes) {
        if (!config_.enabled || is_analytics_disabled()) {
            return false;
        }
        
        std::lock_guard lock(mutex_);
        attributes_ = attributes;
        initialized_ = true;
        
        // Load cached features
        load_cached_features();
        
        return true;
    }

    /// Set user attributes and re-initialize if needed
    void set_attributes(const UserAttributes& attributes) {
        std::lock_guard lock(mutex_);
        attributes_ = attributes;
    }

    /// Get a feature value (cached, may be stale)
    template<typename T>
    [[nodiscard]] T get_feature_value(std::string_view feature_key, T default_value) const {
        std::lock_guard lock(mutex_);
        
        // Check environment override first (for ants)
        if (auto* override = get_env_override(feature_key)) {
            if constexpr (std::is_same_v<T, bool>) {
                if (auto* b = std::get_if<bool>(override)) return *b;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                if (auto* i = std::get_if<std::int64_t>(override)) return *i;
            } else if constexpr (std::is_same_v<T, double>) {
                if (auto* d = std::get_if<double>(override)) return *d;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (auto* s = std::get_if<std::string>(override)) return *s;
            }
        }
        
        // Check config override
        if (auto* config_override = get_config_override(feature_key)) {
            if constexpr (std::is_same_v<T, bool>) {
                if (auto* b = std::get_if<bool>(config_override)) return *b;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                if (auto* i = std::get_if<std::int64_t>(config_override)) return *i;
            } else if constexpr (std::is_same_v<T, double>) {
                if (auto* d = std::get_if<double>(config_override)) return *d;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (auto* s = std::get_if<std::string>(config_override)) return *s;
            }
        }
        
        // Check in-memory features
        auto it = feature_values_.find(std::string(feature_key));
        if (it != feature_values_.end()) {
            if constexpr (std::is_same_v<T, bool>) {
                if (auto* b = std::get_if<bool>(&it->second)) return *b;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                if (auto* i = std::get_if<std::int64_t>(&it->second)) return *i;
            } else if constexpr (std::is_same_v<T, double>) {
                if (auto* d = std::get_if<double>(&it->second)) return *d;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (auto* s = std::get_if<std::string>(&it->second)) return *s;
            }
        }
        
        // Check cached features
        auto cache_it = cached_features_.find(std::string(feature_key));
        if (cache_it != cached_features_.end()) {
            if constexpr (std::is_same_v<T, bool>) {
                if (auto* b = std::get_if<bool>(&cache_it->second)) return *b;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                if (auto* i = std::get_if<std::int64_t>(&cache_it->second)) return *i;
            } else if constexpr (std::is_same_v<T, double>) {
                if (auto* d = std::get_if<double>(&cache_it->second)) return *d;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (auto* s = std::get_if<std::string>(&cache_it->second)) return *s;
            }
        }
        
        return default_value;
    }

    /// Check a boolean feature gate
    [[nodiscard]] bool is_feature_enabled(std::string_view feature_key, bool default_value = false) const {
        return get_feature_value<bool>(feature_key, default_value);
    }

    /// Get all features (for debugging)
    [[nodiscard]] std::unordered_map<std::string, FeatureValue> get_all_features() const {
        std::lock_guard lock(mutex_);
        if (!feature_values_.empty()) {
            return feature_values_;
        }
        return cached_features_;
    }

    /// Refresh features from server
    void refresh_features() {
        if (!initialized_ || refreshing_.exchange(true)) return;
        
        load_cached_features();
        notify_refresh_callbacks();
        refreshing_ = false;
    }

    /// Register a refresh callback
    void on_refresh(RefreshCallback callback) {
        std::lock_guard lock(mutex_);
        refresh_callbacks_.push_back(std::move(callback));
    }

    /// Set a config override (ant-only)
    void set_config_override(std::string_view feature_key, FeatureValue value) {
        const char* user_type = std::getenv("USER_TYPE");
        if (!user_type || std::string_view(user_type) != "ant") return;
        
        std::lock_guard lock(mutex_);
        config_overrides_[std::string(feature_key)] = std::move(value);
        notify_refresh_callbacks();
    }

    /// Clear config overrides
    void clear_config_overrides() {
        const char* user_type = std::getenv("USER_TYPE");
        if (!user_type || std::string_view(user_type) != "ant") return;
        
        std::lock_guard lock(mutex_);
        config_overrides_.clear();
        notify_refresh_callbacks();
    }

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

private:
    GrowthBookConfig config_;
    UserAttributes attributes_;
    std::unordered_map<std::string, FeatureValue> feature_values_;
    std::unordered_map<std::string, FeatureValue> cached_features_;
    mutable std::unordered_map<std::string, FeatureValue> env_overrides_;
    std::unordered_map<std::string, FeatureValue> config_overrides_;
    std::unordered_map<std::string, ExperimentData> experiment_data_;
    std::unordered_set<std::string> logged_exposures_;
    std::vector<RefreshCallback> refresh_callbacks_;
    mutable std::mutex mutex_;
    bool initialized_;
    std::atomic<bool> refreshing_;

    [[nodiscard]] const FeatureValue* get_env_override(std::string_view feature_key) const {
        const char* user_type = std::getenv("USER_TYPE");
        if (!user_type || std::string_view(user_type) != "ant") return nullptr;
        
        const char* overrides = std::getenv("CLAUDE_INTERNAL_FC_OVERRIDES");
        if (!overrides) return nullptr;
        
        // Parse and cache on first access
        if (env_overrides_.empty()) {
            parse_env_overrides(overrides);
        }
        
        auto it = env_overrides_.find(std::string(feature_key));
        return it != env_overrides_.end() ? &it->second : nullptr;
    }

    void parse_env_overrides(std::string_view json_str) const {
        // Parse using yyjson wrapper
        auto doc_result = cc::utils::json::parse(json_str);
        if (!doc_result) return;
        
        auto root = doc_result->root();
        if (!root.is_obj()) return;
        
        root.iter_obj([this](cc::utils::json::JsonVal key, cc::utils::json::JsonVal val) {
            std::string k(key.as_str());
            FeatureValue value;
            
            if (val.is_bool()) {
                value = val.as_bool();
            } else if (val.is_num()) {
                // Heuristic: check if it has a fractional part
                double d = val.as_double();
                int64_t i = val.as_int();
                if (static_cast<double>(i) == d) {
                    value = i;
                } else {
                    value = d;
                }
            } else if (val.is_str()) {
                value = std::string(val.as_str());
            } else {
                return;
            }
            
            env_overrides_[std::move(k)] = std::move(value);
        });
    }

    [[nodiscard]] const FeatureValue* get_config_override(std::string_view feature_key) const {
        auto it = config_overrides_.find(std::string(feature_key));
        return it != config_overrides_.end() ? &it->second : nullptr;
    }

    void load_cached_features() {
        if (const char* raw = std::getenv("CLAUDE_GROWTHBOOK_FEATURES")) {
            parse_env_overrides(raw);
            cached_features_ = env_overrides_;
        }
    }

    void notify_refresh_callbacks() {
        for (const auto& callback : refresh_callbacks_) {
            try {
                callback();
            } catch (...) {
                // Ignore callback exceptions
            }
        }
    }
};

// ============================================================
// GrowthBook sink (for integration with analytics system)
// ============================================================

class GrowthBookSink : public IAnalyticsSink {
public:
    explicit GrowthBookSink(GrowthBookConfig config)
        : client_(std::move(config)) {}

    [[nodiscard]] bool initialize(const UserAttributes& attributes) {
        return client_.initialize(attributes);
    }

    void write(const AnalyticsEvent&) override {
        // Not used for GrowthBook - it's for feature flags, not event logging
    }

    void flush() override {
        // No-op for GrowthBook
    }

    [[nodiscard]] bool is_enabled() const override {
        return client_.is_initialized();
    }

    [[nodiscard]] GrowthBookClient& client() noexcept { return client_; }
    [[nodiscard]] const GrowthBookClient& client() const noexcept { return client_; }

private:
    GrowthBookClient client_;
};

} // namespace cc::services::analytics::growthbook
