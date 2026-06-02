/// @file first_party_event_logger.cppm
/// @brief First-party event logging system for internal analytics
module;

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include <format>
#include <mutex>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <functional>

export module cc.services.analytics.first_party;

export import cc.services.analytics;
export import cc.services.analytics.growthbook;
export import cc.utils.json;
export import cc.utils.http;
export import cc.utils.crypto;

export namespace cc::services::analytics::first_party {

using Clock = std::chrono::system_clock;
using Duration = std::chrono::milliseconds;

// ============================================================
// Event sampling configuration
// ============================================================

struct EventSamplingConfig {
    std::unordered_map<std::string, double> sample_rates; // event_name -> sample_rate
    
    [[nodiscard]] double get_sample_rate(std::string_view event_name) const {
        auto it = sample_rates.find(std::string(event_name));
        return it != sample_rates.end() ? it->second : 1.0;
    }
};

// ============================================================
// Batch configuration
// ============================================================

struct BatchConfig {
    Duration scheduled_delay = Duration{10000}; // 10 seconds
    std::size_t max_export_batch_size = 200;
    std::size_t max_queue_size = 8192;
    bool skip_auth = false;
    std::size_t max_attempts = 3;
    std::string path = "/api/event_logging/batch";
    std::string base_url = "https://api.anthropic.com";
};

// ============================================================
// Log record (for OpenTelemetry-style logging)
// ============================================================

struct LogRecord {
    std::string event_name;
    std::string event_id;
    std::unordered_map<std::string, std::string> attributes;
    TimePoint timestamp = Clock::now();
    
    [[nodiscard]] std::string to_json() const {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        
        root.add("event_name", doc.string(event_name));
        root.add("event_id", doc.string(event_id));
        root.add("timestamp", doc.number(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()).count())));
        
        auto attrs_obj = doc.object();
        for (const auto& [k, v] : attributes) {
            attrs_obj.add(k, doc.string(v));
        }
        root.add("attributes", attrs_obj);
        
        doc.set_root(root);
        return doc.to_string();
    }
};

// ============================================================
// Generate a random event ID (UUID-like)
// ============================================================

[[nodiscard]] std::string generate_event_id() {
    // Simple random ID generator for local event correlation.
    auto now = Clock::now().time_since_epoch().count();
    return std::format("evt_{}_{}", now, std::rand());
}

// ============================================================
// First-party event logger
// ============================================================

class FirstPartyEventLogger {
public:
    explicit FirstPartyEventLogger(BatchConfig config = BatchConfig{})
        : config_(std::move(config)),
          enabled_(false),
          initialized_(false) {}

    ~FirstPartyEventLogger() = default;

    /// Initialize the logger
    void initialize() {
        if (is_analytics_disabled()) {
            return;
        }
        
        std::lock_guard lock(mutex_);
        enabled_ = true;
        initialized_ = true;
    }

    /// Check if enabled
    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled_ && !is_analytics_disabled();
    }

    /// Log a generic event
    void log_event(std::string_view event_name, const EventMetadata& metadata) {
        if (!is_enabled()) return;
        
        // Check sampling
        if (!should_sample(event_name)) return;
        
        LogRecord record;
        record.event_name = std::string(event_name);
        record.event_id = generate_event_id();
        
        // Convert metadata to attributes
        for (const auto& [k, v] : metadata.bool_values) {
            record.attributes[k] = v ? "true" : "false";
        }
        for (const auto& [k, v] : metadata.int_values) {
            record.attributes[k] = std::format("{}", v);
        }
        for (const auto& [k, v] : metadata.double_values) {
            record.attributes[k] = std::format("{}", v);
        }
        
        queue_record(std::move(record));
    }

    /// Log a GrowthBook experiment exposure
    void log_growthbook_experiment(const growthbook::ExperimentData& exp_data,
                                   const growthbook::UserAttributes& user_attrs) {
        if (!is_enabled()) return;
        
        LogRecord record;
        record.event_name = "growthbook_experiment";
        record.event_id = generate_event_id();
        
        record.attributes["experiment_id"] = exp_data.experiment_id;
        record.attributes["variation_id"] = std::format("{}", exp_data.variation_id);
        record.attributes["in_experiment"] = exp_data.in_experiment ? "true" : "false";
        record.attributes["environment"] = "production";
        
        if (!user_attrs.id.empty()) {
            record.attributes["device_id"] = user_attrs.id;
        }
        if (user_attrs.account_uuid) {
            record.attributes["account_uuid"] = *user_attrs.account_uuid;
        }
        if (user_attrs.organization_uuid) {
            record.attributes["organization_uuid"] = *user_attrs.organization_uuid;
        }
        if (!user_attrs.session_id.empty()) {
            record.attributes["session_id"] = user_attrs.session_id;
        }
        
        queue_record(std::move(record));
    }

    /// Force flush pending records
    void flush() {
        std::lock_guard lock(mutex_);
        flush_locked();
    }

    /// Update configuration from GrowthBook
    void update_config(const BatchConfig& config) {
        std::lock_guard lock(mutex_);
        config_ = config;
    }

    /// Update sampling configuration
    void update_sampling_config(const EventSamplingConfig& config) {
        std::lock_guard lock(mutex_);
        sampling_config_ = config;
    }

private:
    BatchConfig config_;
    EventSamplingConfig sampling_config_;
    std::vector<LogRecord> pending_records_;
    std::mutex mutex_;
    bool enabled_;
    bool initialized_;

    [[nodiscard]] bool should_sample(std::string_view event_name) const {
        double rate = sampling_config_.get_sample_rate(event_name);
        if (rate <= 0.0) return false;
        if (rate >= 1.0) return true;
        
        // Simple random sampling
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<> dist(0.0, 1.0);
        return dist(rng) < rate;
    }

    void queue_record(LogRecord record) {
        std::lock_guard lock(mutex_);
        pending_records_.push_back(std::move(record));
        
        if (pending_records_.size() >= config_.max_export_batch_size) {
            flush_locked();
        }
    }

    void flush_locked() {
        if (pending_records_.empty()) return;
        
        std::vector<LogRecord> batch;
        batch.swap(pending_records_);
        
        // Unlock while sending
        mutex_.unlock();
        send_batch(batch);
        mutex_.lock();
    }

    void send_batch(const std::vector<LogRecord>& batch) {
        if (batch.empty()) return;
        
        // Build JSON array by concatenating individual record JSONs
        std::string payload = "[";
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (i > 0) payload += ",";
            payload += batch[i].to_json();
        }
        payload += "]";
        
        std::string url = std::format("{}{}", config_.base_url, config_.path);
        // Persist the prepared payload in memory; network export can reuse this exact body.
        last_export_url_ = std::move(url);
        last_export_payload_ = std::move(payload);
    }

    std::string last_export_url_;
    std::string last_export_payload_;
};

// ============================================================
// First-party event sink (implements IAnalyticsSink)
// ============================================================

class FirstPartyEventSink : public IAnalyticsSink {
public:
    explicit FirstPartyEventSink(BatchConfig config = BatchConfig{})
        : logger_(std::move(config)) {}

    [[nodiscard]] bool initialize() {
        logger_.initialize();
        return logger_.is_enabled();
    }

    void write(const AnalyticsEvent& event) override {
        logger_.log_event(event.event_name, event.metadata);
    }

    void flush() override {
        logger_.flush();
    }

    [[nodiscard]] bool is_enabled() const override {
        return logger_.is_enabled();
    }

    [[nodiscard]] FirstPartyEventLogger& logger() noexcept { return logger_; }
    [[nodiscard]] const FirstPartyEventLogger& logger() const noexcept { return logger_; }

private:
    FirstPartyEventLogger logger_;
};

} // namespace cc::services::analytics::first_party
