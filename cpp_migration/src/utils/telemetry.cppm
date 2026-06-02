// Telemetry Module
// Merges: events, logger, instrumentation, skillLoadedEvent
// Provides telemetry logging, event emission, and OpenTelemetry instrumentation
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.utils.telemetry;

import cc.utils.json;
import cc.utils.async;

export namespace cc::utils::telemetry {

using cc::utils::async::Task;
using cc::utils::json::JsonVal;

// =========================================================================
// Log Levels (from logger.ts - ClaudeCodeDiagLogger)
// =========================================================================

/// Diagnostic log levels matching OpenTelemetry DiagLogger
enum class DiagLogLevel {
    Error,
    Warn,
    Info,
    Debug,
    Verbose,
};

// =========================================================================
// TelemetryLogger (from logger.ts)
// Diagnostic logger for OpenTelemetry integration
// =========================================================================

/// Diagnostic logger that routes to internal debug/error logging
class TelemetryLogger {
public:
    /// Log an error message
    void error(std::string_view message) const;

    /// Log a warning message
    void warn(std::string_view message) const;

    /// Log an info message (no-op by default)
    void info([[maybe_unused]] std::string_view message) const {}

    /// Log a debug message (no-op by default)
    void debug([[maybe_unused]] std::string_view message) const {}

    /// Log a verbose message (no-op by default)
    void verbose([[maybe_unused]] std::string_view message) const {}

    /// Get the minimum log level
    [[nodiscard]] DiagLogLevel min_level() const noexcept { return min_level_; }

    /// Set the minimum log level
    void set_min_level(DiagLogLevel level) noexcept { min_level_ = level; }

private:
    DiagLogLevel min_level_ = DiagLogLevel::Error;
};

// =========================================================================
// EventEmitter (from events.ts)
// OpenTelemetry event logging with attributes
// =========================================================================

/// Attributes map for telemetry events
using TelemetryAttributes = std::unordered_map<std::string, std::string>;

/// Configuration for user prompt logging (privacy control)
struct PromptLoggingConfig {
    bool enabled = false; // OTEL_LOG_USER_PROMPTS
};

/// Redact content if prompt logging is disabled
[[nodiscard]] std::string redact_if_disabled(
    std::string_view content,
    const PromptLoggingConfig& config);

/// Core telemetry event emitter
class EventEmitter {
public:
    /// Get the singleton instance
    static EventEmitter& instance() {
        static EventEmitter emitter;
        return emitter;
    }

    /// Log an OpenTelemetry event with metadata
    /// Event name will be prefixed with "claude_code."
    Task<void> log_event(
        std::string_view event_name,
        TelemetryAttributes metadata = {});

    /// Check if the event logger is initialized
    [[nodiscard]] bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    /// Set the event logger as initialized (called during telemetry init)
    void set_initialized(bool value = true) noexcept {
        initialized_.store(value, std::memory_order_release);
    }

    /// Set prompt ID for the current interaction
    void set_prompt_id(std::string prompt_id);

    /// Get the current prompt ID
    [[nodiscard]] std::optional<std::string_view> get_prompt_id() const;

    /// Get common telemetry attributes (service name, version, etc.)
    [[nodiscard]] TelemetryAttributes get_base_attributes() const;

    /// Get the monotonically increasing event sequence number
    [[nodiscard]] uint64_t next_sequence() noexcept {
        return event_sequence_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Reset state (for testing)
    void reset();

private:
    EventEmitter() = default;
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> event_sequence_{0};
    mutable std::mutex mutex_;
    std::optional<std::string> prompt_id_;
    bool has_warned_no_logger_ = false;
};

// =========================================================================
// InstrumentationContext (from instrumentation.ts)
// OpenTelemetry SDK initialization and management
// =========================================================================

/// Telemetry exporter protocol types
enum class OtlpProtocol {
    Grpc,
    HttpJson,
    HttpProtobuf,
};

/// Exporter types (from OTEL_*_EXPORTER env vars)
enum class ExporterType {
    None,
    Console,
    Otlp,
    Prometheus,
    BigQuery,
};

/// Parse exporter types from a comma-separated string (e.g., "otlp,console")
[[nodiscard]] std::vector<ExporterType> parse_exporter_types(std::string_view value);

/// Check if telemetry is enabled (CLAUDE_CODE_ENABLE_TELEMETRY)
[[nodiscard]] bool is_telemetry_enabled();

/// Telemetry resource attributes for service identification
struct ResourceAttributes {
    std::string service_name = "claude-code";
    std::string service_version;
    std::optional<std::string> host_arch;
    std::optional<std::string> os_type;
    std::optional<std::string> wsl_version;
};

/// Configuration for the telemetry instrumentation system
struct InstrumentationConfig {
    ResourceAttributes resource;
    std::vector<ExporterType> metrics_exporters;
    std::vector<ExporterType> logs_exporters;
    std::vector<ExporterType> traces_exporters;
    std::chrono::milliseconds metrics_export_interval{60000};
    std::chrono::milliseconds logs_export_interval{5000};
    std::chrono::milliseconds traces_export_interval{5000};
    std::chrono::milliseconds shutdown_timeout{2000};
    std::optional<OtlpProtocol> otlp_protocol;
    std::optional<std::string> otlp_endpoint;
};

/// Core instrumentation context managing OTEL providers
class InstrumentationContext {
public:
    /// Get the singleton instance
    static InstrumentationContext& instance() {
        static InstrumentationContext ctx;
        return ctx;
    }

    /// Bootstrap telemetry environment variables (ANT_OTEL_* -> OTEL_*)
    void bootstrap();

    /// Initialize the full telemetry stack (meters, loggers, tracers)
    Task<void> initialize();

    /// Initialize with explicit configuration
    Task<void> initialize(InstrumentationConfig config);

    /// Flush all pending telemetry data immediately
    /// Should be called before logout or org switching
    Task<void> flush();

    /// Shutdown all providers and exporters
    Task<void> shutdown();

    /// Check if instrumentation is initialized
    [[nodiscard]] bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    /// Check if BigQuery metrics are enabled for this user
    [[nodiscard]] bool is_bigquery_metrics_enabled() const;

    /// Get the current instrumentation configuration
    [[nodiscard]] const InstrumentationConfig& config() const noexcept { return config_; }

private:
    InstrumentationContext() = default;
    std::atomic<bool> initialized_{false};
    InstrumentationConfig config_;
    mutable std::mutex mutex_;
};

// =========================================================================
// Skill Loaded Event (from skillLoadedEvent.ts)
// Logs which skills are available at session startup
// =========================================================================

/// Information about a loaded skill for telemetry
struct LoadedSkillInfo {
    std::string name;
    std::string source;        // "user", "project", "plugin", etc.
    std::string loaded_from;   // File path or origin
    std::optional<std::string> kind; // Skill kind/type
};

/// Log all loaded skills for analytics
/// Called at session startup to record which skills are available
Task<void> log_skills_loaded(
    const std::vector<LoadedSkillInfo>& skills,
    std::size_t context_window_tokens);

// =========================================================================
// Convenience functions for common telemetry operations
// =========================================================================

/// Log a hook-related analytics event
inline Task<void> log_hook_event(
    std::string_view event_suffix,
    TelemetryAttributes metadata = {}) {
    return EventEmitter::instance().log_event(
        std::format("tengu_{}", event_suffix), std::move(metadata));
}

/// Log a session-level event
inline Task<void> log_session_event(
    std::string_view event_name,
    TelemetryAttributes metadata = {}) {
    return EventEmitter::instance().log_event(event_name, std::move(metadata));
}

} // namespace cc::utils::telemetry
