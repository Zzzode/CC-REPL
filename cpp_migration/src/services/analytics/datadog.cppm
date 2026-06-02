/// @file datadog.cppm
/// @brief DataDog integration for analytics event logging
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
#include <thread>
#include <atomic>
#include <condition_variable>
#include <cstdlib>

export module cc.services.analytics.datadog;

export import cc.services.analytics;
export import cc.utils.json;
export import cc.utils.http;
export import cc.utils.crypto;

export namespace cc::services::analytics::datadog {

using Clock = std::chrono::system_clock;
using Duration = std::chrono::milliseconds;

// ============================================================
// DataDog configuration
// ============================================================

struct DatadogConfig {
    std::string client_token = "pubbbf48e6d78dae54bceaa4acf463299bf";
    std::string site = "us5.datadoghq.com";
    std::string service = "claude-code";
    std::string hostname = "claude-code";
    Duration flush_interval = Duration{15000};
    std::size_t max_batch_size = 100;
    Duration network_timeout = Duration{5000};
};

// ============================================================
// Allowed events (whitelist)
// ============================================================

const std::unordered_set<std::string_view> ALLOWED_EVENTS = {
    "chrome_bridge_connection_succeeded",
    "chrome_bridge_connection_failed",
    "chrome_bridge_disconnected",
    "chrome_bridge_tool_call_completed",
    "chrome_bridge_tool_call_error",
    "chrome_bridge_tool_call_started",
    "chrome_bridge_tool_call_timeout",
    "tengu_api_error",
    "tengu_api_success",
    "tengu_brief_mode_enabled",
    "tengu_brief_mode_toggled",
    "tengu_brief_send",
    "tengu_cancel",
    "tengu_compact_failed",
    "tengu_exit",
    "tengu_flicker",
    "tengu_init",
    "tengu_model_fallback_triggered",
    "tengu_oauth_error",
    "tengu_oauth_success",
    "tengu_oauth_token_refresh_failure",
    "tengu_oauth_token_refresh_success",
    "tengu_oauth_token_refresh_lock_acquiring",
    "tengu_oauth_token_refresh_lock_acquired",
    "tengu_oauth_token_refresh_starting",
    "tengu_oauth_token_refresh_completed",
    "tengu_oauth_token_refresh_lock_releasing",
    "tengu_oauth_token_refresh_lock_released",
    "tengu_query_error",
    "tengu_session_file_read",
    "tengu_started",
    "tengu_tool_use_error",
    "tengu_tool_use_granted_in_prompt_permanent",
    "tengu_tool_use_granted_in_prompt_temporary",
    "tengu_tool_use_rejected_in_prompt",
    "tengu_tool_use_success",
    "tengu_uncaught_exception",
    "tengu_unhandled_rejection",
    "tengu_voice_recording_started",
    "tengu_voice_toggled",
    "tengu_team_mem_sync_pull",
    "tengu_team_mem_sync_push",
    "tengu_team_mem_sync_started",
    "tengu_team_mem_entries_capped"
};

[[nodiscard]] bool is_event_allowed(std::string_view event_name) noexcept {
    return ALLOWED_EVENTS.contains(event_name);
}

// ============================================================
// Tag fields (for Datadog tags)
// ============================================================

const std::unordered_set<std::string_view> TAG_FIELDS = {
    "arch",
    "clientType",
    "errorType",
    "http_status_range",
    "http_status",
    "kairosActive",
    "model",
    "platform",
    "provider",
    "skillMode",
    "subscriptionType",
    "toolName",
    "userBucket",
    "userType",
    "version",
    "versionBase"
};

// ============================================================
// Datadog log entry
// ============================================================

struct DatadogLog {
    std::string ddsource = "cpp";
    std::string ddtags;
    std::string message;
    std::string service;
    std::string hostname;
    std::string env;
    std::unordered_map<std::string, std::string> attributes;

    [[nodiscard]] std::string to_json() const {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        
        root.add("ddsource", doc.string(ddsource));
        root.add("ddtags", doc.string(ddtags));
        root.add("message", doc.string(message));
        root.add("service", doc.string(service));
        root.add("hostname", doc.string(hostname));
        root.add("env", doc.string(env));
        
        for (const auto& [k, v] : attributes) {
            root.add(k, doc.string(v));
        }
        
        doc.set_root(root);
        return doc.to_string();
    }
};

// ============================================================
// Utility functions
// ============================================================

[[nodiscard]] std::string camel_to_snake(std::string_view s) {
    std::string result;
    result.reserve(s.size() + 5);
    for (char c : s) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            if (!result.empty()) result += '_';
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            result += c;
        }
    }
    return result;
}

[[nodiscard]] std::uint64_t get_user_bucket(std::string_view user_id) {
    auto hash = cc::utils::crypto::sha256(user_id);
    // Take first 8 bytes as uint64
    std::uint64_t bucket = 0;
    for (std::size_t i = 0; i < 8 && i < hash.size(); ++i) {
        bucket = (bucket << 8) | static_cast<std::uint8_t>(hash[i]);
    }
    return bucket % 30;
}

// ============================================================
// Datadog client
// ============================================================

class DatadogClient {
public:
    explicit DatadogClient(DatadogConfig config)
        : config_(std::move(config)),
          initialized_(false),
          running_(false) {}

    ~DatadogClient() {
        shutdown();
    }

    /// Initialize the client
    [[nodiscard]] bool initialize() {
        if (is_analytics_disabled()) {
            return false;
        }
        initialized_ = true;
        start_flush_thread();
        return true;
    }

    /// Shutdown the client and flush remaining logs
    void shutdown() {
        if (!running_.exchange(false)) return;
        
        cv_.notify_all();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        
        flush();
    }

    /// Track an event
    void track_event(std::string_view event_name, const EventMetadata& metadata,
                    std::string_view user_id = "", std::string_view version = "") {
        if (!initialized_ || !is_event_allowed(event_name)) return;

        auto log = build_log(event_name, metadata, user_id, version);
        
        std::lock_guard lock(mutex_);
        log_batch_.push_back(std::move(log));
        
        if (log_batch_.size() >= config_.max_batch_size) {
            flush_locked();
        } else {
            cv_.notify_one();
        }
    }

    /// Flush pending logs
    void flush() {
        std::lock_guard lock(mutex_);
        flush_locked();
    }

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

private:
    DatadogConfig config_;
    std::vector<DatadogLog> log_batch_;
    std::mutex mutex_;
    std::thread flush_thread_;
    std::atomic<bool> initialized_;
    std::atomic<bool> running_;
    std::condition_variable cv_;

    void start_flush_thread() {
        running_ = true;
        flush_thread_ = std::thread([this]() {
            while (running_) {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, config_.flush_interval, [this]() {
                    return !running_ || !log_batch_.empty();
                });
                
                if (!running_) break;
                
                if (!log_batch_.empty()) {
                    flush_locked();
                }
            }
        });
    }

    void flush_locked() {
        if (log_batch_.empty()) return;
        
        std::vector<DatadogLog> batch;
        batch.swap(log_batch_);
        
        // Unlock while sending
        mutex_.unlock();
        send_batch(batch);
        mutex_.lock();
    }

    void send_batch(const std::vector<DatadogLog>& batch) {
        if (batch.empty()) return;

        // Build JSON array by concatenating individual log JSONs
        std::string payload = "[";
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (i > 0) payload += ",";
            payload += batch[i].to_json();
        }
        payload += "]";
        
        std::string url = std::format("https://http-intake.logs.{}/api/v2/logs", config_.site);
        // Keep the prepared request body available for the transport layer.
        last_export_url_ = std::move(url);
        last_export_payload_ = std::move(payload);
    }

    std::string last_export_url_;
    std::string last_export_payload_;

    [[nodiscard]] DatadogLog build_log(std::string_view event_name, 
                                       const EventMetadata& metadata,
                                       std::string_view user_id,
                                       std::string_view version) {
        DatadogLog log;
        log.message = std::string(event_name);
        log.service = config_.service;
        log.hostname = config_.hostname;
        
        // Set env from user type
        const char* user_type = std::getenv("USER_TYPE");
        log.env = user_type ? user_type : "production";
        
        // Build tags
        std::vector<std::string> tags;
        tags.push_back(std::format("event:{}", event_name));
        
        // Add metadata attributes
        for (const auto& [k, v] : metadata.bool_values) {
            auto snake_key = camel_to_snake(k);
            log.attributes[snake_key] = v ? "true" : "false";
            if (TAG_FIELDS.contains(k)) {
                tags.push_back(std::format("{}:{}", snake_key, v ? "true" : "false"));
            }
        }
        for (const auto& [k, v] : metadata.int_values) {
            auto snake_key = camel_to_snake(k);
            log.attributes[snake_key] = std::format("{}", v);
            if (TAG_FIELDS.contains(k)) {
                tags.push_back(std::format("{}:{}", snake_key, v));
            }
        }
        for (const auto& [k, v] : metadata.double_values) {
            auto snake_key = camel_to_snake(k);
            log.attributes[snake_key] = std::format("{}", v);
            if (TAG_FIELDS.contains(k)) {
                tags.push_back(std::format("{}:{}", snake_key, v));
            }
        }
        
        // Add user bucket if available
        if (!user_id.empty()) {
            auto bucket = get_user_bucket(user_id);
            log.attributes["user_bucket"] = std::format("{}", bucket);
            tags.push_back(std::format("user_bucket:{}", bucket));
        }
        
        // Add version if available
        if (!version.empty()) {
            log.attributes["version"] = std::string(version);
            tags.push_back(std::format("version:{}", version));
        }
        
        // Join tags
        log.ddtags.clear();
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i > 0) log.ddtags += ",";
            log.ddtags += tags[i];
        }
        
        return log;
    }
};

// ============================================================
// Datadog sink (implements IAnalyticsSink)
// ============================================================

class DatadogSink : public IAnalyticsSink {
public:
    explicit DatadogSink(DatadogConfig config)
        : client_(std::move(config)) {}

    [[nodiscard]] bool initialize() {
        return client_.initialize();
    }

    void write(const AnalyticsEvent& event) override {
        client_.track_event(event.event_name, event.metadata);
    }

    void flush() override {
        client_.flush();
    }

    [[nodiscard]] bool is_enabled() const override {
        return client_.is_initialized();
    }

    void shutdown() {
        client_.shutdown();
    }

private:
    DatadogClient client_;
};

} // namespace cc::services::analytics::datadog
