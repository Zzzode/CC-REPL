/// @file lifecycle_hooks.cppm
/// @brief Lifecycle hook system for pre/post tool execution and session events.
/// Provides an event bus where consumers can register callbacks for tool and session lifecycle.
module;

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.lifecycle_hooks;

export namespace cc::hooks {

// ============================================================
// Hook Event Types
// ============================================================

/// Context passed to pre-tool-use hooks
struct PreToolUseEvent {
    std::string tool_name;
    std::string tool_input_json;
    std::string tool_use_id;
    std::chrono::system_clock::time_point timestamp;
};

/// Context passed to post-tool-use hooks
struct PostToolUseEvent {
    std::string tool_name;
    std::string tool_use_id;
    bool is_error;
    std::string output_preview;      // First 500 chars of output
    std::chrono::milliseconds duration;
    std::chrono::system_clock::time_point timestamp;
};

/// Session lifecycle event
enum class SessionEventType {
    Started,
    Resumed,
    Compacted,
    Ended,
};

struct SessionEvent {
    SessionEventType type;
    std::string session_id;
    std::chrono::system_clock::time_point timestamp;
};

/// Query lifecycle event
struct QueryStartEvent {
    std::string query_text;
    std::string model;
    std::chrono::system_clock::time_point timestamp;
};

struct QueryEndEvent {
    bool success;
    std::uint32_t rounds;          // Number of tool-loop rounds
    std::uint32_t tools_executed;
    std::chrono::milliseconds duration;
    std::chrono::system_clock::time_point timestamp;
};

// ============================================================
// Hook Callback Types
// ============================================================

using PreToolUseHook = std::function<void(const PreToolUseEvent&)>;
using PostToolUseHook = std::function<void(const PostToolUseEvent&)>;
using SessionHook = std::function<void(const SessionEvent&)>;
using QueryStartHook = std::function<void(const QueryStartEvent&)>;
using QueryEndHook = std::function<void(const QueryEndEvent&)>;

// ============================================================
// Hook Checker Types (hooks that can block execution)
// ============================================================

/// Pre-tool-use checker: returns nullopt if allowed, or a denial reason string if blocked.
using PreToolUseChecker = std::function<std::optional<std::string>(const PreToolUseEvent&)>;

/// Stop hook checker: returns nullopt if allowed to continue, or a stop reason if should stop.
using StopHookChecker = std::function<std::optional<std::string>()>;

// ============================================================
// Lifecycle Hook Registry
// ============================================================

/// Central registry for lifecycle hooks.
/// Thread-safe: all registration and emission is mutex-protected.
/// Supports both notification hooks (fire-and-forget) and checker hooks (can block execution).
class LifecycleHookRegistry {
public:
    LifecycleHookRegistry() = default;

    // ========================================================
    // Notification hook registration
    // ========================================================

    void on_pre_tool_use(PreToolUseHook hook) {
        std::lock_guard lk(mu_);
        pre_tool_hooks_.push_back(std::move(hook));
    }

    void on_post_tool_use(PostToolUseHook hook) {
        std::lock_guard lk(mu_);
        post_tool_hooks_.push_back(std::move(hook));
    }

    void on_session_event(SessionHook hook) {
        std::lock_guard lk(mu_);
        session_hooks_.push_back(std::move(hook));
    }

    void on_query_start(QueryStartHook hook) {
        std::lock_guard lk(mu_);
        query_start_hooks_.push_back(std::move(hook));
    }

    void on_query_end(QueryEndHook hook) {
        std::lock_guard lk(mu_);
        query_end_hooks_.push_back(std::move(hook));
    }

    // ========================================================
    // Checker hook registration (can block execution)
    // ========================================================

    /// Register a pre-tool-use checker that can block tool execution.
    /// If any checker returns a non-nullopt string, the tool is denied with that reason.
    void add_pre_tool_use_checker(PreToolUseChecker checker) {
        std::lock_guard lk(mu_);
        pre_tool_checkers_.push_back(std::move(checker));
    }

    /// Register a stop hook checker that can stop the query loop.
    /// If any checker returns a non-nullopt string, the loop stops with that reason.
    void add_stop_hook(StopHookChecker checker) {
        std::lock_guard lk(mu_);
        stop_hooks_.push_back(std::move(checker));
    }

    // ========================================================
    // Emission (called by the query engine at appropriate points)
    // ========================================================

    void emit_pre_tool_use(const PreToolUseEvent& event) {
        std::lock_guard lk(mu_);
        for (const auto& hook : pre_tool_hooks_) {
            hook(event);
        }
    }

    /// Check pre-tool-use permissions. Returns nullopt if allowed, or denial reason if blocked.
    /// Also emits the notification event to all pre-tool-use hooks.
    [[nodiscard]] std::optional<std::string> check_and_emit_pre_tool_use(const PreToolUseEvent& event) {
        std::vector<PreToolUseChecker> checkers;
        {
            std::lock_guard lk(mu_);
            checkers = pre_tool_checkers_;
            // Emit notification hooks while holding lock (they should be fast)
            for (const auto& hook : pre_tool_hooks_) {
                hook(event);
            }
        }
        // Run checkers outside the lock to avoid deadlocks
        for (const auto& checker : checkers) {
            if (auto reason = checker(event)) {
                return reason;
            }
        }
        return std::nullopt;
    }

    void emit_post_tool_use(const PostToolUseEvent& event) {
        std::lock_guard lk(mu_);
        for (const auto& hook : post_tool_hooks_) {
            hook(event);
        }
    }

    void emit_session_event(const SessionEvent& event) {
        std::lock_guard lk(mu_);
        for (const auto& hook : session_hooks_) {
            hook(event);
        }
    }

    void emit_query_start(const QueryStartEvent& event) {
        std::lock_guard lk(mu_);
        for (const auto& hook : query_start_hooks_) {
            hook(event);
        }
    }

    void emit_query_end(const QueryEndEvent& event) {
        std::lock_guard lk(mu_);
        for (const auto& hook : query_end_hooks_) {
            hook(event);
        }
    }

    /// Check all stop hooks. Returns nullopt if ok to continue, or stop reason if should stop.
    [[nodiscard]] std::optional<std::string> check_stop_hooks() const {
        std::vector<StopHookChecker> hooks;
        {
            std::lock_guard lk(mu_);
            hooks = stop_hooks_;
        }
        for (const auto& hook : hooks) {
            if (auto reason = hook()) {
                return reason;
            }
        }
        return std::nullopt;
    }

    // ========================================================
    // Stats
    // ========================================================

    [[nodiscard]] std::size_t total_hooks() const {
        std::lock_guard lk(mu_);
        return pre_tool_hooks_.size() + post_tool_hooks_.size() +
               session_hooks_.size() + query_start_hooks_.size() +
               query_end_hooks_.size() + pre_tool_checkers_.size() +
               stop_hooks_.size();
    }

    [[nodiscard]] std::size_t checker_count() const {
        std::lock_guard lk(mu_);
        return pre_tool_checkers_.size() + stop_hooks_.size();
    }

    /// Clear all hooks (for testing)
    void clear() {
        std::lock_guard lk(mu_);
        pre_tool_hooks_.clear();
        post_tool_hooks_.clear();
        session_hooks_.clear();
        query_start_hooks_.clear();
        query_end_hooks_.clear();
        pre_tool_checkers_.clear();
        stop_hooks_.clear();
    }

private:
    mutable std::mutex mu_;
    std::vector<PreToolUseHook> pre_tool_hooks_;
    std::vector<PostToolUseHook> post_tool_hooks_;
    std::vector<SessionHook> session_hooks_;
    std::vector<QueryStartHook> query_start_hooks_;
    std::vector<QueryEndHook> query_end_hooks_;
    std::vector<PreToolUseChecker> pre_tool_checkers_;
    std::vector<StopHookChecker> stop_hooks_;
};

} // namespace cc::hooks
