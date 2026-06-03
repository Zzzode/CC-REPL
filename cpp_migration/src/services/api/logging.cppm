// API Logging - Request/response logging and usage tracking
module;
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.services.api.logging;

import cc.services.api.models;
import cc.utils.json;

export namespace cc::services::api {

using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonVal;

// =========================================================================
// Token Usage (local definition for logging independence)
// =========================================================================

struct TokenUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_creation_tokens = 0;
    int cache_read_tokens = 0;

    [[nodiscard]] int total() const { return input_tokens + output_tokens; }
};

// =========================================================================
// Log Level
// =========================================================================

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

// =========================================================================
// API Log Entry
// =========================================================================

struct ApiLogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string request_id;
    std::string method;
    std::string url;
    int status_code = 0;
    std::chrono::milliseconds latency{0};
    std::optional<std::string> error_message;
    std::optional<TokenUsage> usage;
};

// =========================================================================
// Usage Tracker
// =========================================================================

struct UsageStats {
    int total_requests = 0;
    int total_input_tokens = 0;
    int total_output_tokens = 0;
    int total_cache_creation_tokens = 0;
    int total_cache_read_tokens = 0;
    std::chrono::system_clock::time_point session_start;
    std::unordered_map<std::string, int> model_usage; // model -> count
};

class UsageTracker {
public:
    UsageTracker() {
        stats_.session_start = std::chrono::system_clock::now();
    }

    // Record a request completion with usage
    void record_request(const std::string& model, const TokenUsage& usage) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        stats_.total_input_tokens += usage.input_tokens;
        stats_.total_output_tokens += usage.output_tokens;
        stats_.total_cache_creation_tokens += usage.cache_creation_tokens;
        stats_.total_cache_read_tokens += usage.cache_read_tokens;
        stats_.model_usage[model]++;
    }

    // Get current stats
    [[nodiscard]] UsageStats get_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    // Reset stats
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = UsageStats{};
        stats_.session_start = std::chrono::system_clock::now();
    }

    // Calculate estimated cost (simplified)
    [[nodiscard]] double estimate_cost() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Aggregate estimate for the default Sonnet pricing tier.
        const double input_price_per_million = 3.0;
        const double output_price_per_million = 15.0;
        return (static_cast<double>(stats_.total_input_tokens) * input_price_per_million / 1e6) +
               (static_cast<double>(stats_.total_output_tokens) * output_price_per_million / 1e6);
    }

private:
    mutable std::mutex mutex_;
    UsageStats stats_;
};

// =========================================================================
// API Logger
// =========================================================================

class ApiLogger {
public:
    struct Config {
        LogLevel min_level = LogLevel::Info;
        bool log_requests = true;
        bool log_responses = true;
        bool log_usage = true;
        std::optional<std::string> log_file;
        size_t max_entries = 1000;
    };

    ApiLogger() : config_(), usage_tracker_() {}
    explicit ApiLogger(Config config)
        : config_(std::move(config))
        , usage_tracker_() {}

    // Log a request start
    void log_request_start(
        const std::string& request_id,
        const std::string& method,
        const std::string& url) {
        
        if (!config_.log_requests) return;

        ApiLogEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.level = LogLevel::Debug;
        entry.request_id = request_id;
        entry.method = method;
        entry.url = url;
        
        add_entry(std::move(entry));
    }

    // Log a request completion
    void log_request_complete(
        const std::string& request_id,
        int status_code,
        std::chrono::milliseconds latency,
        const std::optional<TokenUsage>& usage = std::nullopt,
        const std::optional<std::string>& error_message = std::nullopt) {
        
        if (!config_.log_responses) return;

        ApiLogEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.level = (status_code >= 400) ? LogLevel::Error : LogLevel::Info;
        entry.request_id = request_id;
        entry.status_code = status_code;
        entry.latency = latency;
        entry.error_message = error_message;
        entry.usage = usage;

        add_entry(std::move(entry));

        // Track usage
        if (usage) {
            usage_tracker_.record_request("unknown", *usage);
        }
    }

    // Log an error
    void log_error(
        const std::string& request_id,
        const std::string& error_message,
        int status_code = 0) {
        
        ApiLogEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.level = LogLevel::Error;
        entry.request_id = request_id;
        entry.status_code = status_code;
        entry.error_message = error_message;

        add_entry(std::move(entry));
    }

    // Get recent entries
    [[nodiscard]] std::vector<ApiLogEntry> get_recent_entries(size_t count = 100) const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t start = (entries_.size() > count) ? (entries_.size() - count) : 0;
        return std::vector<ApiLogEntry>(entries_.begin() + start, entries_.end());
    }

    // Get usage tracker
    [[nodiscard]] UsageTracker& usage_tracker() { return usage_tracker_; }
    [[nodiscard]] const UsageTracker& usage_tracker() const { return usage_tracker_; }

    // Clear logs
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    // Reconfigure the logger in-place
    void reconfigure(Config new_config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(new_config);
        entries_.clear();
    }

    // Export logs to JSON
    [[nodiscard]] std::string export_to_json() const {
        std::lock_guard<std::mutex> lock(mutex_);
        JsonMutDoc doc;
        auto arr = doc.array();
        
        for (const auto& entry : entries_) {
            auto obj = doc.object();
            obj.add("timestamp", doc.string(format_timestamp(entry.timestamp)));
            obj.add("level", doc.string(log_level_to_string(entry.level)));
            obj.add("request_id", doc.string(entry.request_id));
            if (!entry.method.empty()) {
                obj.add("method", doc.string(entry.method));
            }
            if (!entry.url.empty()) {
                obj.add("url", doc.string(entry.url));
            }
            if (entry.status_code > 0) {
                obj.add("status_code", doc.number(static_cast<int64_t>(entry.status_code)));
            }
            obj.add("latency_ms", doc.number(static_cast<int64_t>(entry.latency.count())));
            if (entry.error_message) {
                obj.add("error", doc.string(*entry.error_message));
            }
            if (entry.usage) {
                auto usage_obj = doc.object();
                usage_obj.add("input", doc.number(static_cast<int64_t>(entry.usage->input_tokens)));
                usage_obj.add("output", doc.number(static_cast<int64_t>(entry.usage->output_tokens)));
                usage_obj.add("cache_creation", doc.number(static_cast<int64_t>(entry.usage->cache_creation_tokens)));
                usage_obj.add("cache_read", doc.number(static_cast<int64_t>(entry.usage->cache_read_tokens)));
                obj.add("usage", usage_obj);
            }
            arr.append(obj);
        }
        
        doc.set_root(arr);
        return doc.to_string();
    }

private:
    void add_entry(ApiLogEntry entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(entry));
        
        // Trim if too many
        while (entries_.size() > config_.max_entries) {
            entries_.erase(entries_.begin());
        }

        if (config_.log_file && !entries_.empty()) {
            auto path = std::filesystem::path(*config_.log_file);
            if (!path.parent_path().empty()) {
                std::error_code ec;
                std::filesystem::create_directories(path.parent_path(), ec);
            }
            std::ofstream file(*config_.log_file, std::ios::app);
            if (file.is_open()) {
                file << serialize_entry(entries_.back()) << '\n';
            }
        }
    }

    [[nodiscard]] static std::string serialize_entry(const ApiLogEntry& entry) {
        JsonMutDoc doc;
        auto obj = doc.object();
        obj.add("timestamp", doc.string(format_timestamp(entry.timestamp)));
        obj.add("level", doc.string(log_level_to_string(entry.level)));
        obj.add("request_id", doc.string(entry.request_id));
        if (!entry.method.empty()) obj.add("method", doc.string(entry.method));
        if (!entry.url.empty()) obj.add("url", doc.string(entry.url));
        if (entry.status_code > 0) obj.add("status_code", doc.number(static_cast<int64_t>(entry.status_code)));
        obj.add("latency_ms", doc.number(static_cast<int64_t>(entry.latency.count())));
        if (entry.error_message) obj.add("error", doc.string(*entry.error_message));
        if (entry.usage) {
            auto usage_obj = doc.object();
            usage_obj.add("input", doc.number(static_cast<int64_t>(entry.usage->input_tokens)));
            usage_obj.add("output", doc.number(static_cast<int64_t>(entry.usage->output_tokens)));
            usage_obj.add("cache_creation", doc.number(static_cast<int64_t>(entry.usage->cache_creation_tokens)));
            usage_obj.add("cache_read", doc.number(static_cast<int64_t>(entry.usage->cache_read_tokens)));
            obj.add("usage", usage_obj);
        }
        doc.set_root(obj);
        return doc.to_string();
    }

    [[nodiscard]] static std::string log_level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::Debug: return "debug";
            case LogLevel::Info: return "info";
            case LogLevel::Warning: return "warning";
            case LogLevel::Error: return "error";
        }
        return "unknown";
    }

    [[nodiscard]] static std::string format_timestamp(
        const std::chrono::system_clock::time_point& tp) {
        auto t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
        localtime_r(&t, &tm);
        return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    Config config_;
    mutable std::mutex mutex_;
    std::vector<ApiLogEntry> entries_;
    UsageTracker usage_tracker_;
};

// =========================================================================
// Global Logger Instance
// =========================================================================

class GlobalApiLogger {
public:
    static ApiLogger& instance() {
        static ApiLogger logger;
        return logger;
    }

    static void configure(ApiLogger::Config config) {
        instance().reconfigure(std::move(config));
    }
};

// =========================================================================
// Helper Functions
// =========================================================================

// Generate a unique request ID
[[nodiscard]] inline std::string generate_request_id() {
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return std::format("req_{}_{}", now, ++counter);
}

// RAII timer for request latency
class RequestTimer {
public:
    explicit RequestTimer(ApiLogger& logger, std::string request_id)
        : logger_(logger)
        , request_id_(std::move(request_id))
        , start_(std::chrono::steady_clock::now()) {}

    ~RequestTimer() = default;

    [[nodiscard]] std::chrono::milliseconds elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_);
    }

    void complete(int status_code,
                  const std::optional<TokenUsage>& usage = std::nullopt,
                  const std::optional<std::string>& error = std::nullopt) {
        logger_.log_request_complete(request_id_, status_code, elapsed(), usage, error);
    }

private:
    ApiLogger& logger_;
    std::string request_id_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace cc::services::api
