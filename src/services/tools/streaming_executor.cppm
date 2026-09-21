/// @file streaming_executor.cppm



module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <coroutine>
#include <format>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>

export module cc.services.streaming_executor;

import cc.types.types;
import cc.tools.tool;
import cc.utils.async;
import cc.utils.error;

export namespace cc::services::streaming_executor {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::async::Task;
using cc::core::ToolInput;
using cc::core::ToolResult;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ============================================================

// ============================================================


struct StreamChunk {
    std::string data;
    bool is_stderr;
    TimePoint timestamp;
};


struct StreamConfig {
    std::size_t buffer_size = 4096;
    std::uint32_t flush_interval_ms = 100;
    std::size_t max_output_bytes = 1024 * 1024;
    std::uint32_t timeout_ms = 30000;
};


enum class ExecutionState : std::uint8_t {
    Pending,
    Running,
    Streaming,
    Completed,
    Cancelled,
    TimedOut,
};


[[nodiscard]] constexpr std::string_view state_to_string(ExecutionState state) noexcept {
    switch (state) {
        case ExecutionState::Pending:    return "pending";
        case ExecutionState::Running:    return "running";
        case ExecutionState::Streaming:  return "streaming";
        case ExecutionState::Completed:  return "completed";
        case ExecutionState::Cancelled:  return "cancelled";
        case ExecutionState::TimedOut:   return "timed_out";
    }
    return "unknown";
}


using OnChunkCallback = std::function<void(const StreamChunk&)>;

/// Tool dispatch function type (dependency injection)
using ToolDispatchFn = std::function<Task<ToolResult>(std::string_view, const ToolInput&)>;

// ============================================================

// ============================================================


struct ExecutionContext {
    std::string execution_id;
    std::atomic<ExecutionState> state{ExecutionState::Pending};
    std::string accumulated_output;
    std::size_t total_bytes{0};
    TimePoint start_time;
    std::uint32_t timeout_ms{30000};
    mutable std::mutex output_mutex;

    ExecutionContext() = default;
    ExecutionContext(ExecutionContext&& other) noexcept
        : execution_id(std::move(other.execution_id))
        , state(other.state.load())
        , accumulated_output(std::move(other.accumulated_output))
        , total_bytes(other.total_bytes)
        , start_time(other.start_time)
        , timeout_ms(other.timeout_ms) {}
    ExecutionContext& operator=(ExecutionContext&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(output_mutex, other.output_mutex);
            execution_id = std::move(other.execution_id);
            state.store(other.state.load());
            accumulated_output = std::move(other.accumulated_output);
            total_bytes = other.total_bytes;
            start_time = other.start_time;
            timeout_ms = other.timeout_ms;
        }
        return *this;
    }
    ExecutionContext(const ExecutionContext&) = delete;
    ExecutionContext& operator=(const ExecutionContext&) = delete;


    void append_output(std::string_view data) {
        std::lock_guard lock(output_mutex);
        accumulated_output.append(data);
        total_bytes += data.size();
    }


    [[nodiscard]] std::string get_output() const {
        std::lock_guard lock(output_mutex);
        return accumulated_output;
    }
};

// ============================================================

// ============================================================


class StreamingToolExecutor {
public:
    explicit StreamingToolExecutor(ToolDispatchFn dispatch_fn, StreamConfig default_config = {})
        : dispatch_fn_(std::move(dispatch_fn))
        , default_config_(std::move(default_config))
        , next_id_(1) {}

    ~StreamingToolExecutor() = default;


    StreamingToolExecutor(const StreamingToolExecutor&) = delete;
    StreamingToolExecutor& operator=(const StreamingToolExecutor&) = delete;
    StreamingToolExecutor(StreamingToolExecutor&&) noexcept = delete;
    StreamingToolExecutor& operator=(StreamingToolExecutor&&) noexcept = delete;





    Task<ToolResult> execute(
        std::string_view tool_name,
        const ToolInput& input,
        std::optional<StreamConfig> config = std::nullopt) {

        auto exec_config = config.value_or(default_config_);
        auto exec_id = allocate_execution_id();
        auto& ctx = create_context(exec_id, exec_config.timeout_ms);

        ctx.state.store(ExecutionState::Running);
        ctx.start_time = Clock::now();

        auto result = co_await execute_with_timeout(exec_id, tool_name, input, exec_config);

        ctx.state.store(ExecutionState::Completed);
        co_return result;
    }





    Task<ToolResult> execute_streaming(
        std::string_view tool_name,
        const ToolInput& input,
        OnChunkCallback on_chunk) {

        auto exec_id = allocate_execution_id();
        auto& ctx = create_context(exec_id, default_config_.timeout_ms);

        ctx.state.store(ExecutionState::Streaming);
        ctx.start_time = Clock::now();

        auto result = co_await stream_tool_output(exec_id, tool_name, input, on_chunk);

        if (ctx.state.load() == ExecutionState::Streaming) {
            ctx.state.store(ExecutionState::Completed);
        }
        co_return result;
    }


    void cancel(std::string_view execution_id) {
        std::lock_guard lock(contexts_mutex_);
        auto it = contexts_.find(std::string(execution_id));
        if (it != contexts_.end()) {
            auto expected_state = ExecutionState::Running;
            it->second.state.compare_exchange_strong(expected_state, ExecutionState::Cancelled);
            expected_state = ExecutionState::Streaming;
            it->second.state.compare_exchange_strong(expected_state, ExecutionState::Cancelled);
        }
    }


    [[nodiscard]] ExecutionState get_state(std::string_view execution_id) const {
        std::lock_guard lock(contexts_mutex_);
        auto it = contexts_.find(std::string(execution_id));
        if (it != contexts_.end()) {
            return it->second.state.load();
        }
        return ExecutionState::Pending;
    }


    void set_timeout(std::string_view execution_id, std::uint32_t ms) {
        std::lock_guard lock(contexts_mutex_);
        auto it = contexts_.find(std::string(execution_id));
        if (it != contexts_.end()) {
            it->second.timeout_ms = ms;
        }
    }


    [[nodiscard]] std::string get_output_so_far(std::string_view execution_id) const {
        std::lock_guard lock(contexts_mutex_);
        auto it = contexts_.find(std::string(execution_id));
        if (it != contexts_.end()) {
            return it->second.get_output();
        }
        return "";
    }


    void cleanup_completed() {
        std::lock_guard lock(contexts_mutex_);
        std::erase_if(contexts_, [](const auto& pair) {
            auto state = pair.second.state.load();
            return state == ExecutionState::Completed ||
                   state == ExecutionState::Cancelled ||
                   state == ExecutionState::TimedOut;
        });
    }

private:

    [[nodiscard]] std::string allocate_execution_id() {
        return std::format("exec_{}", next_id_++);
    }


    ExecutionContext& create_context(const std::string& exec_id, std::uint32_t timeout_ms) {
        std::lock_guard lock(contexts_mutex_);
        ExecutionContext context;
        context.execution_id = exec_id;
        context.state.store(ExecutionState::Pending);
        context.start_time = Clock::now();
        context.timeout_ms = timeout_ms;
        auto [it, _] = contexts_.emplace(exec_id, std::move(context));
        return it->second;
    }


    Task<ToolResult> execute_with_timeout(
        const std::string& exec_id,
        std::string_view tool_name,
        const ToolInput& input,
        const StreamConfig& config) {

        auto* ctx = find_context(exec_id);
        if (!ctx) {
            co_return ToolResult::error("execution context not found");
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - ctx->start_time);
        if (elapsed.count() > config.timeout_ms) {
            ctx->state.store(ExecutionState::TimedOut);
            co_return ToolResult::error(std::format("tool '{}' timed out after {}ms", tool_name, config.timeout_ms));
        }

        auto result = co_await dispatch_fn_(tool_name, input);

        // Track output
        if (!result.is_error && !result.content.empty()) {
            std::string output;
            for (const auto& c : result.content) {
                output += c.text;
            }
            if (output.size() > config.max_output_bytes) {
                output.resize(config.max_output_bytes);
            }
            ctx->append_output(output);
        }
        co_return result;
    }


    Task<ToolResult> stream_tool_output(
        const std::string& exec_id,
        std::string_view tool_name,
        const ToolInput& input,
        const OnChunkCallback& on_chunk) {

        auto* ctx = find_context(exec_id);
        if (!ctx) {
            co_return ToolResult::error("execution context not found");
        }

        // Execute the tool
        auto result = co_await dispatch_fn_(tool_name, input);

        // Stream the result content in chunks
        std::string content;
        for (const auto& c : result.content) {
            content += c.text;
        }

        std::size_t offset = 0;
        while (offset < content.size()) {
            if (ctx->state.load() == ExecutionState::Cancelled) {
                co_return ToolResult::error("execution cancelled");
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - ctx->start_time);
            if (elapsed.count() > ctx->timeout_ms) {
                ctx->state.store(ExecutionState::TimedOut);
                co_return ToolResult::error(std::format("tool '{}' timed out", tool_name));
            }

            auto chunk_size = std::min(default_config_.buffer_size, content.size() - offset);
            auto chunk_text = content.substr(offset, chunk_size);
            ctx->append_output(chunk_text);
            if (on_chunk) {
                on_chunk(StreamChunk{.data = chunk_text, .is_stderr = false, .timestamp = Clock::now()});
            }
            offset += chunk_size;
        }
        co_return result;
    }

    ExecutionContext* find_context(const std::string& exec_id) {
        std::lock_guard lock(contexts_mutex_);
        auto it = contexts_.find(exec_id);
        return it == contexts_.end() ? nullptr : &it->second;
    }

    ToolDispatchFn dispatch_fn_;
    StreamConfig default_config_;
    std::atomic<std::uint64_t> next_id_;
    mutable std::mutex contexts_mutex_;
    std::unordered_map<std::string, ExecutionContext> contexts_;
};

} // namespace cc::services::streaming_executor
