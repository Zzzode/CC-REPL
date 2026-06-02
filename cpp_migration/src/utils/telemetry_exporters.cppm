// Telemetry Exporters Module
// Merges: bigqueryExporter, perfettoTracing, sessionTracing, betaSessionTracing, pluginTelemetry
// Provides metric/trace exporters and session-level tracing infrastructure
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.utils.telemetry_exporters;

import cc.utils.json;
import cc.utils.async;
import cc.utils.telemetry;

export namespace cc::utils::telemetry_exporters {

using cc::utils::async::Task;
using cc::utils::json::JsonVal;
using cc::utils::telemetry::TelemetryAttributes;
namespace fs = std::filesystem;

// =========================================================================
// BigQueryExporter (from bigqueryExporter.ts)
// Exports metrics to BigQuery via the Anthropic API
// =========================================================================

/// A single metric data point for BigQuery export
struct MetricDataPoint {
    std::unordered_map<std::string, std::string> attributes;
    double value = 0.0;
    std::string timestamp; // ISO 8601
};

/// A metric with its data points
struct MetricRecord {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> unit;
    std::vector<MetricDataPoint> data_points;
};

/// Payload sent to the BigQuery metrics endpoint
struct MetricsPayload {
    std::unordered_map<std::string, std::string> resource_attributes;
    std::vector<MetricRecord> metrics;
};

/// Export result status
enum class ExportResultCode {
    Success,
    Failed,
};

/// Result of an export operation
struct ExportResult {
    ExportResultCode code;
    std::optional<std::string> error;
};

/// BigQuery metrics exporter implementing push-based metric export
class BigQueryExporter {
public:
    struct Options {
        std::chrono::milliseconds timeout{5000};
        std::optional<std::string> endpoint_override;
    };

    BigQueryExporter() : BigQueryExporter(Options{}) {}
    explicit BigQueryExporter(Options options);

    /// Export a batch of metrics to BigQuery
    Task<ExportResult> do_export(const MetricsPayload& payload);

    /// Force flush any pending exports
    Task<void> force_flush();

    /// Shutdown the exporter (no more exports accepted)
    Task<void> shutdown();

    /// Check if the exporter has been shut down
    [[nodiscard]] bool is_shutdown() const noexcept {
        return is_shutdown_.load(std::memory_order_acquire);
    }

    /// Get the configured endpoint URL
    [[nodiscard]] std::string_view endpoint() const noexcept { return endpoint_; }

private:
    std::string endpoint_;
    std::chrono::milliseconds timeout_;
    std::atomic<bool> is_shutdown_{false};
    mutable std::mutex mutex_;
    std::vector<Task<void>> pending_exports_;
};

// =========================================================================
// PerfettoTracer (from perfettoTracing.ts)
// Chrome Trace Event format tracing for visualization in ui.perfetto.dev
// =========================================================================

/// Chrome Trace Event phase markers
enum class TraceEventPhase : char {
    Begin = 'B',       // Begin duration event
    End = 'E',         // End duration event
    Complete = 'X',    // Complete event (with duration)
    Instant = 'i',     // Instant event
    Counter = 'C',     // Counter event
    AsyncBegin = 'b',  // Async begin
    AsyncEnd = 'e',    // Async end
    Metadata = 'M',    // Metadata event
};

/// A single Chrome Trace Event
struct TraceEvent {
    std::string name;
    TraceEventPhase phase;
    std::chrono::microseconds timestamp;
    std::optional<std::chrono::microseconds> duration; // For Complete events
    uint64_t pid = 1;
    uint64_t tid = 1;
    std::optional<std::string> category;
    std::optional<std::string> id;  // For async events
    std::unordered_map<std::string, std::string> args;
};

/// Configuration for Perfetto tracing
struct PerfettoConfig {
    fs::path output_path;                              // Trace file path
    std::optional<std::chrono::seconds> write_interval; // Periodic write interval
    bool enabled = false;
};

/// Perfetto tracer for generating Chrome Trace Event format files
class PerfettoTracer {
public:
    /// Get the singleton instance
    static PerfettoTracer& instance() {
        static PerfettoTracer tracer;
        return tracer;
    }

    /// Initialize Perfetto tracing (reads CLAUDE_CODE_PERFETTO_TRACE env var)
    void initialize();

    /// Initialize with explicit configuration
    void initialize(PerfettoConfig config);

    /// Check if Perfetto tracing is enabled
    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    /// Start an interaction span (top-level user interaction)
    /// Returns a span ID for later ending
    [[nodiscard]] std::string start_interaction_span(
        std::string_view name,
        std::unordered_map<std::string, std::string> attributes = {});

    /// End an interaction span
    void end_interaction_span(std::string_view span_id);

    /// Start an LLM request span
    [[nodiscard]] std::string start_llm_request_span(
        std::string_view model,
        std::unordered_map<std::string, std::string> attributes = {});

    /// End an LLM request span with response attributes
    void end_llm_request_span(
        std::string_view span_id,
        std::unordered_map<std::string, std::string> response_attributes = {});

    /// Start a tool execution span
    [[nodiscard]] std::string start_tool_span(
        std::string_view tool_name,
        std::unordered_map<std::string, std::string> attributes = {});

    /// End a tool execution span
    void end_tool_span(
        std::string_view span_id,
        std::unordered_map<std::string, std::string> result_attributes = {});

    /// Start a user input waiting span
    [[nodiscard]] std::string start_user_input_span();

    /// End user input waiting span
    void end_user_input_span(std::string_view span_id);

    /// Write the trace file to disk
    void flush_to_disk();

    /// Dispose and write final trace
    void dispose();

private:
    PerfettoTracer() = default;

    /// Add a trace event to the buffer
    void add_event(TraceEvent event);

    /// Generate a unique span ID
    [[nodiscard]] std::string generate_span_id();

    std::atomic<bool> enabled_{false};
    PerfettoConfig config_;
    mutable std::mutex mutex_;
    std::vector<TraceEvent> events_;
    std::atomic<uint64_t> span_counter_{0};
};

// =========================================================================
// SessionTracer (from sessionTracing.ts, betaSessionTracing.ts)
// OpenTelemetry-based session tracing with span hierarchy
// =========================================================================

/// Types of spans in the session trace
enum class SpanType {
    Interaction,       // Top-level user interaction
    LlmRequest,        // API request to model
    Tool,              // Tool execution (full lifecycle)
    ToolBlockedOnUser, // Tool waiting for user permission
    ToolExecution,     // Tool actual execution
    Hook,              // Hook execution
};

/// Context for an active span
struct SpanContext {
    std::string span_id;
    SpanType type;
    std::chrono::steady_clock::time_point start_time;
    std::unordered_map<std::string, std::string> attributes;
    bool ended = false;
    std::optional<std::string> perfetto_span_id;
};

/// LLM request attributes for beta tracing
struct LlmRequestAttributes {
    std::string model;
    std::optional<std::size_t> prompt_tokens;
    std::optional<std::size_t> completion_tokens;
    std::optional<std::chrono::milliseconds> ttft; // Time to first token
    std::optional<std::chrono::milliseconds> ttlt; // Time to last token
    std::optional<std::string> message_id;
    bool speculative = false;
    std::optional<double> cache_read_ratio;
};

/// Configuration for session tracing
struct SessionTracingConfig {
    bool enhanced_telemetry_enabled = false; // Feature flag
    bool beta_tracing_enabled = false;       // ENABLE_BETA_TRACING_DETAILED
    std::optional<std::string> beta_tracing_endpoint; // BETA_TRACING_ENDPOINT
};

/// Maximum time before a span is considered stale and cleaned up
inline constexpr auto kSpanTtl = std::chrono::minutes{30};

/// Session-level tracer managing span hierarchy
class SessionTracer {
public:
    /// Get the singleton instance
    static SessionTracer& instance() {
        static SessionTracer tracer;
        return tracer;
    }

    /// Check if enhanced telemetry (session tracing) is enabled
    [[nodiscard]] bool is_enhanced_telemetry_enabled() const noexcept;

    /// Check if beta tracing is enabled
    [[nodiscard]] bool is_beta_tracing_enabled() const noexcept;

    /// Start an interaction span (user prompt → response complete)
    [[nodiscard]] std::string start_interaction_span(
        TelemetryAttributes attributes = {});

    /// End the current interaction span
    void end_interaction_span();

    /// Start an LLM request span within the current interaction
    [[nodiscard]] std::string start_llm_request_span(
        std::string_view model,
        TelemetryAttributes attributes = {});

    /// End an LLM request span with response metadata
    void end_llm_request_span(
        std::string_view span_id,
        const LlmRequestAttributes& response_attrs = {});

    /// Start a tool span
    [[nodiscard]] std::string start_tool_span(
        std::string_view tool_name,
        TelemetryAttributes attributes = {});

    /// End a tool span
    void end_tool_span(
        std::string_view span_id,
        TelemetryAttributes result_attributes = {});

    /// Start a blocked-on-user span (permission prompt)
    [[nodiscard]] std::string start_blocked_on_user_span(
        std::string_view tool_name);

    /// End a blocked-on-user span
    void end_blocked_on_user_span(std::string_view span_id);

    /// Start a tool execution span (actual work)
    [[nodiscard]] std::string start_tool_execution_span(
        std::string_view tool_name);

    /// End a tool execution span
    void end_tool_execution_span(std::string_view span_id);

    /// Start a hook span
    [[nodiscard]] std::string start_hook_span(
        std::string_view hook_name,
        std::string_view hook_event);

    /// End a hook span
    void end_hook_span(std::string_view span_id);

    /// Start a user input span
    [[nodiscard]] std::string start_user_input_span();

    /// End user input span
    void end_user_input_span(std::string_view span_id);

    /// Add beta-specific interaction attributes
    void add_beta_interaction_attributes(TelemetryAttributes attributes);

    /// Add beta-specific LLM request attributes
    void add_beta_llm_request_attributes(
        std::string_view span_id,
        const LlmRequestAttributes& attrs);

    /// Truncate content to fit within tracing size limits
    [[nodiscard]] static std::string truncate_content(
        std::string_view content,
        std::size_t max_length = 4096);

    /// Clean up stale spans
    void cleanup_stale_spans();

    /// Reset all state (for testing)
    void reset();

private:
    SessionTracer() = default;

    /// Generate a unique span ID
    [[nodiscard]] std::string generate_span_id();

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SpanContext>> active_spans_;
    std::unordered_map<std::string, std::shared_ptr<SpanContext>> strong_spans_;
    std::optional<std::string> current_interaction_id_;
    std::optional<std::string> current_tool_id_;
    std::atomic<uint64_t> interaction_sequence_{0};
    std::atomic<uint64_t> span_counter_{0};
    bool cleanup_interval_started_ = false;
};

// =========================================================================
// Plugin Telemetry (from pluginTelemetry.ts)
// Telemetry for plugin lifecycle events
// =========================================================================

/// Plugin telemetry event types
enum class PluginTelemetryEvent {
    PluginLoaded,
    PluginUnloaded,
    PluginError,
    PluginHookExecuted,
    PluginToolUsed,
};

/// Plugin telemetry data
struct PluginTelemetryData {
    std::string plugin_id;
    std::string plugin_name;
    PluginTelemetryEvent event;
    std::optional<std::string> error_message;
    std::optional<std::chrono::milliseconds> duration;
    TelemetryAttributes extra_attributes;
};

/// Log a plugin telemetry event
Task<void> log_plugin_telemetry(const PluginTelemetryData& data);

/// Log plugin hook execution metrics
Task<void> log_plugin_hook_execution(
    std::string_view plugin_id,
    std::string_view hook_event,
    std::chrono::milliseconds duration,
    bool success);

// =========================================================================
// Convenience initialization functions
// =========================================================================

/// Initialize all tracing subsystems (Perfetto + Session + Beta)
/// Called from InstrumentationContext::initialize()
void initialize_all_tracers(const SessionTracingConfig& config);

/// Check if any tracing subsystem is active
[[nodiscard]] bool is_any_tracing_enabled();

/// Shutdown all tracing subsystems
Task<void> shutdown_all_tracers();

} // namespace cc::utils::telemetry_exporters
