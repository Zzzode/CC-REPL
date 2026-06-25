/// @file analytics.cppm
/// @brief Analytics and telemetry service for tracking usage, performance,
/// and errors. Uses a sink-based architecture with concept constraints,
/// supporting both file-based (NDJSON) and null (privacy-mode) backends.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include <format>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <memory>
#include <concepts>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <cstdlib>

export module cc.services.analytics;

export import cc.utils.json;
export import cc.utils.http;
export import cc.utils.crypto;

export namespace cc::services::analytics {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

// ============================================================
// Configuration
// ============================================================

struct AnalyticsConfig {
    bool enabled = true;
    bool datadog_enabled = false;
    bool first_party_enabled = false;
    bool growthbook_enabled = false;
    std::string datadog_client_token;
    std::string datadog_site = "us5.datadoghq.com";
    std::string growthbook_client_key;
    std::string growthbook_api_host = "https://api.anthropic.com";
    std::filesystem::path log_dir;
    std::size_t flush_threshold = 50;
    Duration flush_interval = Duration{15000};
};

[[nodiscard]] bool is_analytics_disabled() noexcept {
    // Check environment variables
    if (const char* test_env = std::getenv("NODE_ENV")) {
        if (std::string_view(test_env) == "test") return true;
    }
    if (std::getenv("CLAUDE_CODE_USE_BEDROCK")) return true;
    if (std::getenv("CLAUDE_CODE_USE_VERTEX")) return true;
    if (std::getenv("CLAUDE_CODE_USE_FOUNDRY")) return true;
    return false;
}

// ============================================================
// Event type classification
// ============================================================

enum class EventType : std::uint8_t {
    ToolUse,
    CommandExec,
    ApiCall,
    Error,
    SessionStart,
    SessionEnd,
    UserInput,
    ModelResponse,
    PermissionCheck,
    FileOperation,
    Custom,
};

[[nodiscard]] constexpr std::string_view event_type_name(EventType type) noexcept {
    switch (type) {
        case EventType::ToolUse:         return "tool_use";
        case EventType::CommandExec:     return "command_exec";
        case EventType::ApiCall:         return "api_call";
        case EventType::Error:           return "error";
        case EventType::SessionStart:    return "session_start";
        case EventType::SessionEnd:      return "session_end";
        case EventType::UserInput:       return "user_input";
        case EventType::ModelResponse:   return "model_response";
        case EventType::PermissionCheck: return "permission_check";
        case EventType::FileOperation:   return "file_operation";
        case EventType::Custom:          return "custom";
    }
    return "unknown";
}

// ============================================================
// Event metadata (string-free to avoid sensitive data)
// ============================================================

struct EventMetadata {
    std::unordered_map<std::string, bool> bool_values;
    std::unordered_map<std::string, std::int64_t> int_values;
    std::unordered_map<std::string, double> double_values;

    [[nodiscard]] std::string to_json() const {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        
        for (const auto& [k, v] : bool_values) {
            root.add(k, doc.boolean(v));
        }
        for (const auto& [k, v] : int_values) {
            root.add(k, doc.number(v));
        }
        for (const auto& [k, v] : double_values) {
            root.add(k, doc.number(v));
        }
        
        doc.set_root(root);
        return doc.to_string();
    }
};

// ============================================================
// Analytics event structure
// ============================================================

struct AnalyticsEvent {
    EventType type;
    TimePoint timestamp = Clock::now();
    std::string event_name;
    EventMetadata metadata;
    std::string session_id;

    /// Serialize event to NDJSON line
    [[nodiscard]] std::string to_ndjson() const {
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()).count();
        return std::format(R"({{"type":"{}","name":"{}","ts":{},"session":"{}","props":{}}})",
            event_type_name(type), event_name, ts_ms, session_id,
            metadata.to_json());
    }
};

// ============================================================
// AnalyticsSink concept - defines the sink interface
// ============================================================

template <typename T>
concept AnalyticsSink = requires(T sink, const AnalyticsEvent& event) {
    { sink.write(event) } -> std::same_as<void>;
    { sink.flush() } -> std::same_as<void>;
    { sink.is_enabled() } -> std::same_as<bool>;
};

// ============================================================
// FileSink - writes events to local NDJSON file
// ============================================================

class FileSink {
public:
    explicit FileSink(std::filesystem::path output_path)
        : path_(std::move(output_path)) {
        // Ensure parent directory exists
        std::filesystem::create_directories(path_.parent_path());
        stream_.open(path_, std::ios::app);
    }

    void write(const AnalyticsEvent& event) {
        std::lock_guard lock(mutex_);
        if (stream_.is_open()) {
            stream_ << event.to_ndjson() << '\n';
            ++pending_count_;
        }
    }

    void flush() {
        std::lock_guard lock(mutex_);
        if (stream_.is_open()) {
            stream_.flush();
            pending_count_ = 0;
        }
    }

    [[nodiscard]] bool is_enabled() const { return stream_.is_open(); }

    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_count_; }

private:
    std::filesystem::path path_;
    std::ofstream stream_;
    std::mutex mutex_;
    std::size_t pending_count_ = 0;
};

// ============================================================
// NullSink - no-op implementation for privacy mode
// ============================================================

class NullSink {
public:
    void write([[maybe_unused]] const AnalyticsEvent& event) { /* intentionally empty */ }
    void flush() { /* intentionally empty */ }
    [[nodiscard]] bool is_enabled() const { return false; }
};

// Verify concept satisfaction
static_assert(AnalyticsSink<FileSink>);
static_assert(AnalyticsSink<NullSink>);

// ============================================================
// Performance tracking
// ============================================================

using SpanId = std::uint64_t;

struct SpanRecord {
    std::string name;
    TimePoint start_time;
    std::optional<TimePoint> end_time;
    Duration duration() const {
        auto end = end_time.value_or(Clock::now());
        return std::chrono::duration_cast<Duration>(end - start_time);
    }
};

struct SessionMetrics {
    Duration total_duration{0};
    std::size_t api_calls = 0;
    std::size_t tool_uses = 0;
    std::size_t errors = 0;
    std::size_t total_tokens_in = 0;
    std::size_t total_tokens_out = 0;
    Duration avg_api_latency{0};
};

class PerformanceTracker {
public:
    /// Start a named performance span
    [[nodiscard]] SpanId start_span(std::string name) {
        auto id = next_span_id_++;
        spans_.emplace(id, SpanRecord{
            .name = std::move(name),
            .start_time = Clock::now(),
            .end_time = std::nullopt,
        });
        return id;
    }

    /// End a previously started span
    void end_span(SpanId id) {
        if (auto it = spans_.find(id); it != spans_.end()) {
            it->second.end_time = Clock::now();
        }
    }

    /// Record a named metric value
    void record_metric(std::string_view name, double value) {
        metrics_[std::string(name)].push_back(value);
    }

    /// Get aggregated session metrics
    [[nodiscard]] SessionMetrics get_session_metrics() const {
        return session_metrics_;
    }

    /// Update session metrics directly
    void update_session_metrics(const SessionMetrics& m) { session_metrics_ = m; }

    /// Get a specific span by ID
    [[nodiscard]] std::optional<SpanRecord> get_span(SpanId id) const {
        if (auto it = spans_.find(id); it != spans_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

private:
    SpanId next_span_id_ = 1;
    std::unordered_map<SpanId, SpanRecord> spans_;
    std::unordered_map<std::string, std::vector<double>> metrics_;
    SessionMetrics session_metrics_;
};

// ============================================================
// Queued event for pre-sink attachment
// ============================================================

struct QueuedEvent {
    std::string event_name;
    EventMetadata metadata;
    bool async;
};

// ============================================================
// AnalyticsSink interface (type-erased)
// ============================================================

class IAnalyticsSink {
public:
    virtual ~IAnalyticsSink() = default;
    virtual void write(const AnalyticsEvent& event) = 0;
    virtual void flush() = 0;
    [[nodiscard]] virtual bool is_enabled() const = 0;
};

// Type-erased sink wrapper
template <AnalyticsSink T>
class SinkWrapper : public IAnalyticsSink {
public:
    explicit SinkWrapper(T sink) : sink_(std::move(sink)) {}
    void write(const AnalyticsEvent& event) override { sink_.write(event); }
    void flush() override { sink_.flush(); }
    [[nodiscard]] bool is_enabled() const override { return sink_.is_enabled(); }
private:
    T sink_;
};

// ============================================================
// Analytics - main analytics service
// ============================================================

class Analytics {
public:
    explicit Analytics(AnalyticsConfig config)
        : config_(std::move(config)),
          file_sink_(config_.log_dir / "analytics.ndjson") {}

    /// Attach a sink to receive events
    void attach_sink(std::unique_ptr<IAnalyticsSink> sink) {
        std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
        // Drain queued events
        drain_queue();
    }

    /// Log a synchronous event
    void log_event(std::string_view event_name, EventMetadata metadata) {
        if (!config_.enabled) return;
        
        AnalyticsEvent event{
            .type = EventType::Custom,
            .event_name = std::string(event_name),
            .metadata = std::move(metadata),
            .session_id = session_id_
        };
        
        std::lock_guard lock(mutex_);
        if (sink_) {
            sink_->write(event);
        } else {
            queue_.push_back(QueuedEvent{
                .event_name = std::string(event_name),
                .metadata = event.metadata,
                .async = false
            });
        }
    }

    /// Log an asynchronous event
    void log_event_async(std::string_view event_name, EventMetadata metadata) {
        if (!config_.enabled) return;
        
        AnalyticsEvent event{
            .type = EventType::Custom,
            .event_name = std::string(event_name),
            .metadata = std::move(metadata),
            .session_id = session_id_
        };
        
        std::lock_guard lock(mutex_);
        if (sink_) {
            sink_->write(event);
        } else {
            queue_.push_back(QueuedEvent{
                .event_name = std::string(event_name),
                .metadata = event.metadata,
                .async = true
            });
        }
    }

    /// Track a generic event (queued until flush)
    void track(AnalyticsEvent event) {
        if (!config_.enabled) return;
        event.session_id = session_id_;
        std::lock_guard lock(mutex_);
        queue_.push_back(QueuedEvent{
            .event_name = event.event_name,
            .metadata = event.metadata,
            .async = false
        });
        // Auto-flush at threshold
        if (queue_.size() >= config_.flush_threshold) flush();
    }

    /// Flush all queued events to the sink
    void flush() {
        std::lock_guard lock(mutex_);
        if (sink_) {
            for (const auto& queued : queue_) {
                AnalyticsEvent event{
                    .type = EventType::Custom,
                    .event_name = queued.event_name,
                    .metadata = queued.metadata,
                    .session_id = session_id_
                };
                sink_->write(event);
            }
            sink_->flush();
        }
        queue_.clear();
    }

    /// Enable or disable analytics collection
    void set_enabled(bool enabled) noexcept { config_.enabled = enabled; }

    /// Set user identifier for session correlation
    void set_user_id(std::string_view id) { user_id_ = std::string(id); }

    /// Mark session start
    void session_start() {
        session_id_ = generate_session_id();
        EventMetadata meta;
        log_event("session_start", std::move(meta));
        perf_.update_session_metrics(SessionMetrics{});
    }

    /// Mark session end
    void session_end() {
        EventMetadata meta;
        log_event("session_end", std::move(meta));
        flush();
    }

    /// Track tool usage event
    void tool_used(std::string_view, Duration duration, bool success) {
        EventMetadata meta;
        meta.int_values["duration_ms"] = duration.count();
        meta.bool_values["success"] = success;
        log_event("tool_use", std::move(meta));
    }

    /// Track API call metrics
    void api_call(std::string_view, std::size_t tokens_in,
                  std::size_t tokens_out, Duration latency) {
        EventMetadata meta;
        meta.int_values["tokens_in"] = static_cast<std::int64_t>(tokens_in);
        meta.int_values["tokens_out"] = static_cast<std::int64_t>(tokens_out);
        meta.int_values["latency_ms"] = latency.count();
        log_event("api_call", std::move(meta));
    }

    /// Track an error occurrence
    void error_occurred(std::string_view, std::string_view) {
        EventMetadata meta;
        log_event("error", std::move(meta));
    }

    /// Access the performance tracker
    [[nodiscard]] PerformanceTracker& perf() noexcept { return perf_; }
    [[nodiscard]] const PerformanceTracker& perf() const noexcept { return perf_; }

    /// Check if analytics is enabled
    [[nodiscard]] bool is_enabled() const noexcept { return config_.enabled; }

    /// Get configuration
    [[nodiscard]] const AnalyticsConfig& config() const noexcept { return config_; }

private:
    AnalyticsConfig config_;
    FileSink file_sink_;
    std::unique_ptr<IAnalyticsSink> sink_;
    PerformanceTracker perf_;
    std::vector<QueuedEvent> queue_;
    std::string session_id_;
    std::string user_id_;
    std::mutex mutex_;

    void drain_queue() {
        // Called with lock held
        if (!sink_) return;
        for (const auto& queued : queue_) {
            AnalyticsEvent event{
                .type = EventType::Custom,
                .event_name = queued.event_name,
                .metadata = queued.metadata,
                .session_id = session_id_
            };
            sink_->write(event);
        }
        queue_.clear();
    }

    [[nodiscard]] static std::string generate_session_id() {
        auto now = Clock::now().time_since_epoch().count();
        return std::format("sess_{}", now);
    }
};

} // namespace cc::services::analytics
