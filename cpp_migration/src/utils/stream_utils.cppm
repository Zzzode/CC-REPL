module;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.stream_utils;

export namespace cc::utils {

// ─── Async Stream (single-consumer async iterator) ───────────────────────────

/// A single-use async stream that allows one producer to enqueue values
/// and one consumer to await them. Modeled after TS `Stream<T>`.
template<typename T>
class AsyncStream {
public:
    explicit AsyncStream(std::function<void()> on_return = nullptr)
        : on_return_(std::move(on_return)) {}

    ~AsyncStream() { done(); }

    // Non-copyable, movable
    AsyncStream(const AsyncStream&) = delete;
    AsyncStream& operator=(const AsyncStream&) = delete;
    AsyncStream(AsyncStream&&) noexcept = default;
    AsyncStream& operator=(AsyncStream&&) noexcept = default;

    /// Producer: push a value into the stream
    void enqueue(T value) {
        std::lock_guard lock(mutex_);
        if (is_done_) return;
        queue_.push_back(std::move(value));
        cv_.notify_one();
    }

    /// Producer: signal that no more values will arrive
    void done() {
        std::lock_guard lock(mutex_);
        is_done_ = true;
        cv_.notify_all();
    }

    /// Producer: signal an error condition
    void error(std::string err_msg) {
        std::lock_guard lock(mutex_);
        error_ = std::move(err_msg);
        cv_.notify_all();
    }

    /// Consumer: blocking wait for next value.
    /// Returns std::nullopt when stream is done.
    /// Returns unexpected on error.
    std::expected<std::optional<T>, std::string> next() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] {
            return !queue_.empty() || is_done_ || error_.has_value();
        });

        if (error_.has_value()) {
            return std::unexpected(error_.value());
        }
        if (!queue_.empty()) {
            T val = std::move(queue_.front());
            queue_.pop_front();
            return std::optional<T>(std::move(val));
        }
        // Done
        return std::optional<T>(std::nullopt);
    }

    /// Consumer: signal early termination (calls on_return callback)
    void return_stream() {
        done();
        if (on_return_) {
            on_return_();
        }
    }

    /// Check if stream has been started (first next() called)
    bool started() const {
        std::lock_guard lock(mutex_);
        return started_;
    }

    /// Mark the stream as started (called by iteration facilities)
    void mark_started() {
        std::lock_guard lock(mutex_);
        started_ = true;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool is_done_ = false;
    bool started_ = false;
    std::optional<std::string> error_;
    std::function<void()> on_return_;
};

// ─── JSON Stdout Guard ───────────────────────────────────────────────────────

/// Sentinel marker written to stderr for diverted non-JSON lines
inline constexpr std::string_view STDOUT_GUARD_MARKER = "[stdout-guard]";

/// Determines whether a line is valid JSON (or empty, which is valid in NDJSON)
bool is_json_line(std::string_view line);

/// Write function type for stdout/stderr output
using WriteFn = std::function<bool(std::string_view)>;

/// Configuration for the JSON stdout guard
struct JsonStdoutGuardConfig {
    WriteFn stdout_write;
    WriteFn stderr_write;
};

/// Installs a runtime guard that intercepts stdout writes for stream-json mode.
/// Non-JSON lines are diverted to stderr tagged with STDOUT_GUARD_MARKER.
/// Returns a cleanup function that restores original behavior.
class JsonStdoutGuard {
public:
    explicit JsonStdoutGuard(JsonStdoutGuardConfig config);
    ~JsonStdoutGuard();

    // Non-copyable, non-movable (singleton semantics)
    JsonStdoutGuard(const JsonStdoutGuard&) = delete;
    JsonStdoutGuard& operator=(const JsonStdoutGuard&) = delete;

    /// Process a write through the guard. JSON lines go to stdout, others to stderr.
    bool write(std::string_view chunk);

    /// Flush any buffered partial line
    void flush();

    /// Check if the guard is currently installed
    static bool is_installed();

private:
    void process_line(std::string_view line);

    JsonStdoutGuardConfig config_;
    std::string buffer_;
    static std::atomic<bool> installed_;
};

// ─── Streamlined Transform ───────────────────────────────────────────────────

/// Tool categories for summarization in streamlined mode
enum class ToolCategory {
    searches,
    reads,
    writes,
    commands,
    other
};

/// Accumulated tool use counts
struct ToolCounts {
    int searches = 0;
    int reads = 0;
    int writes = 0;
    int commands = 0;
    int other = 0;
};

/// Result of a streamlined transform
enum class StreamlinedMessageType {
    text,
    tool_use_summary,
    result_passthrough,
    filtered_out
};

struct StreamlinedMessage {
    StreamlinedMessageType type;
    std::string text;           // populated for text and tool_use_summary
    std::string session_id;
    std::string uuid;
};

/// Categorize a tool name into a ToolCategory
ToolCategory categorize_tool_name(std::string_view tool_name);

/// Generate a human-readable summary text from tool counts
std::optional<std::string> get_tool_summary_text(const ToolCounts& counts);

/// A stateful transformer that accumulates tool counts between text messages.
/// Tool counts reset when a message with text content is encountered.
class StreamlinedTransformer {
public:
    StreamlinedTransformer() = default;

    /// Transform a message, returning the streamlined result or nullopt if filtered
    std::optional<StreamlinedMessage> transform(
        std::string_view message_type,
        std::string_view text_content,
        std::string_view tool_name,
        std::string_view session_id,
        std::string_view uuid);

    /// Reset accumulated counts
    void reset();

private:
    ToolCounts cumulative_counts_;
};

// ─── Buffered Writer ─────────────────────────────────────────────────────────

/// Configuration for creating a buffered writer
struct BufferedWriterConfig {
    WriteFn write_fn;
    std::chrono::milliseconds flush_interval{1000};
    size_t max_buffer_size = 100;
    size_t max_buffer_bytes = SIZE_MAX;
    bool immediate_mode = false;
};

/// A buffered writer that batches write operations for efficiency.
/// Flushes on timer, buffer count, or byte limit.
class BufferedWriter {
public:
    explicit BufferedWriter(BufferedWriterConfig config);
    ~BufferedWriter();

    // Non-copyable, non-movable
    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;

    /// Write content (buffered unless immediate_mode)
    void write(std::string_view content);

    /// Force flush all buffered content
    void flush();

    /// Flush and release resources (also called by destructor)
    void dispose();

private:
    void schedule_flush();
    void flush_deferred();
    void clear_timer();

    BufferedWriterConfig config_;
    std::vector<std::string> buffer_;
    size_t buffer_bytes_ = 0;
    bool disposed_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> timer_active_{false};
};

} // namespace cc::utils
