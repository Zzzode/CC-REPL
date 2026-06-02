// C++23 Profiler Module
// Performance profiling infrastructure: startup timing, headless mode profiling,
// and query execution profiling
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.profiler;

export namespace cc::utils::profiler {

// ===========================================================================
// Profiler Base Infrastructure
// ===========================================================================

/// Format a millisecond value with 3 decimal places.
[[nodiscard]] std::string format_ms(double ms);

/// Memory usage snapshot
struct MemoryUsage {
    std::size_t rss = 0;          // Resident set size
    std::size_t heap_total = 0;   // Total heap allocated
    std::size_t heap_used = 0;    // Heap currently in use
    std::size_t external = 0;     // External memory (C++ objects bound to JS)
};

/// Render a single timeline line in the shared profiler report format:
///   [+  total.ms] (+  delta.ms) name [extra] [| RSS: .., Heap: ..]
[[nodiscard]] std::string format_timeline_line(
    double total_ms,
    double delta_ms,
    std::string_view name,
    const std::optional<MemoryUsage>& memory,
    std::size_t total_pad,
    std::size_t delta_pad,
    std::string_view extra = "");

/// A performance mark entry
struct PerformanceMark {
    std::string name;
    double start_time_ms = 0.0;  // Relative to process start
};

/// High-resolution clock for profiling
class PerformanceTimer {
public:
    PerformanceTimer();

    /// Record a named mark at the current time.
    void mark(std::string_view name);

    /// Clear all marks.
    void clear_marks();

    /// Clear a specific mark by name.
    void clear_marks(std::string_view name);

    /// Get all recorded marks in order.
    [[nodiscard]] const std::vector<PerformanceMark>& get_marks() const noexcept;

    /// Get current elapsed time since timer creation (ms).
    [[nodiscard]] double now() const noexcept;

private:
    std::chrono::steady_clock::time_point epoch_;
    std::vector<PerformanceMark> marks_;
};

/// Get or create the process-wide singleton performance timer.
[[nodiscard]] PerformanceTimer& get_performance();

// ===========================================================================
// Startup Profiler
// ===========================================================================

/// Configuration for the startup profiler
struct StartupProfilerConfig {
    bool detailed_profiling = false;    // CLAUDE_CODE_PROFILE_STARTUP=1
    bool statsig_logging_sampled = false;
    std::string_view user_type;         // "ant" or other
    double statsig_sample_rate = 0.005; // 0.5% external
};

/// Startup profiler that tracks initialization phases.
/// Two modes:
/// 1. Sampled logging: 100% ant, 0.5% external - logs phases to analytics
/// 2. Detailed profiling: CLAUDE_CODE_PROFILE_STARTUP=1 - full report with memory
class StartupProfiler {
public:
    explicit StartupProfiler(const StartupProfilerConfig& config);

    /// Record a checkpoint with the given name.
    void checkpoint(std::string_view name);

    /// Generate and output the profiling report. Only logs once.
    void report();

    /// Check if detailed profiling is enabled.
    [[nodiscard]] bool is_detailed_profiling_enabled() const noexcept;

    /// Get the path for the startup perf log file.
    [[nodiscard]] std::filesystem::path get_startup_perf_log_path(
        const std::filesystem::path& config_home_dir,
        std::string_view session_id) const;

    /// Get the formatted report string.
    [[nodiscard]] std::string get_report() const;

    /// Log startup performance phases to analytics.
    void log_startup_perf() const;

private:
    bool should_profile_ = false;
    bool detailed_profiling_ = false;
    bool statsig_logging_sampled_ = false;
    bool reported_ = false;
    std::vector<MemoryUsage> memory_snapshots_;

    /// Callback for emitting analytics events
    std::function<void(std::string_view event, const std::map<std::string, double>&)> log_event_;
};

// ===========================================================================
// Headless Profiler
// ===========================================================================

/// Configuration for headless mode profiling
struct HeadlessProfilerConfig {
    bool is_non_interactive = false;
    bool detailed_profiling = false;
    bool statsig_logging_sampled = false;
    std::string_view user_type;
    double statsig_sample_rate = 0.05; // 5% external
};

/// Headless mode profiling utility for measuring per-turn latency.
/// Tracks key timing phases per turn:
/// - Time to system message output (turn 0 only)
/// - Time to first query started
/// - Time to first API response (TTFT)
class HeadlessProfiler {
public:
    explicit HeadlessProfiler(const HeadlessProfilerConfig& config);

    /// Start a new turn for profiling. Clears previous marks, increments turn number.
    void start_turn();

    /// Record a checkpoint with the given name.
    void checkpoint(std::string_view name);

    /// Log headless latency metrics for the current turn to analytics.
    void log_turn();

    /// Get current turn number.
    [[nodiscard]] int current_turn_number() const noexcept { return turn_number_; }

private:
    bool should_profile_ = false;
    bool detailed_profiling_ = false;
    bool statsig_logging_sampled_ = false;
    bool is_non_interactive_ = false;
    int turn_number_ = -1;

    static constexpr std::string_view MARK_PREFIX = "headless_";

    void clear_headless_marks();
};

// ===========================================================================
// Query Profiler
// ===========================================================================

/// Slow operation warning severity levels
enum class SlowWarning : std::uint8_t {
    None,
    Slow,       // > 100ms
    VerySlow    // > 1000ms
};

/// Configuration for query profiling
struct QueryProfilerConfig {
    bool enabled = false;  // CLAUDE_CODE_PROFILE_QUERY=1
};

/// Query profiling utility for measuring time spent in the query pipeline
/// from user input to first token arrival.
///
/// Checkpoints tracked (in order):
/// - query_user_input_received: Start of profiling
/// - query_context_loading_start/end
/// - query_query_start: Entry to query call
/// - query_microcompact_start/end
/// - query_autocompact_start/end
/// - query_setup_start/end: StreamingToolExecutor and model setup
/// - query_api_loop_start
/// - query_api_streaming_start
/// - query_tool_schema_build_start/end
/// - query_message_normalization_start/end
/// - query_client_creation_start/end
/// - query_api_request_sent
/// - query_response_headers_received
/// - query_first_chunk_received (TTFT)
/// - query_api_streaming_end
/// - query_tool_execution_start/end
/// - query_end
class QueryProfiler {
public:
    explicit QueryProfiler(const QueryProfilerConfig& config);

    /// Start profiling a new query session.
    void start_query_profile();

    /// Record a checkpoint with the given name.
    void checkpoint(std::string_view name);

    /// End the current query profiling session.
    void end_query_profile();

    /// Get a formatted report of all checkpoints for the current/last query.
    [[nodiscard]] std::string get_query_profile_report() const;

    /// Log the query profile report to debug output.
    void log_query_profile_report() const;

    /// Get the current query count.
    [[nodiscard]] std::size_t query_count() const noexcept { return query_count_; }

    /// Check if profiling is enabled.
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

private:
    bool enabled_ = false;
    std::size_t query_count_ = 0;
    std::optional<double> first_token_time_;
    std::map<std::string, MemoryUsage> memory_snapshots_;

    /// Identify slow operations (> 100ms delta).
    [[nodiscard]] static SlowWarning get_slow_warning(double delta_ms, std::string_view name);

    /// Get phase-based summary showing time spent in each major phase.
    [[nodiscard]] std::string get_phase_summary(
        std::span<const PerformanceMark> marks,
        double baseline_time) const;
};

} // namespace cc::utils::profiler
