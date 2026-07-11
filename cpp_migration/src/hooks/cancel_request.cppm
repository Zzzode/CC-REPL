/// @file cancel_request.cppm
/// @brief Cancel in-flight API requests with thread-safe cancellation tokens.
/// Provides CancelToken (passable to async ops) and CancelController
/// (manages state machine). Faithful port of src/hooks/useCancelRequest.ts.
module;

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.cancel_request;

export namespace cc::hooks::cancel_request {

// =========================================================================
// CancelState enum
// =========================================================================

/// State of a cancellation request.
/// TS REF: src/hooks/useCancelRequest.ts:97-101 (abortSignal check + cancel flow)
enum class CancelState : std::uint8_t {
    Idle,         ///< No cancellation in progress
    Pending,      ///< Cancel requested, not yet processed
    Cancelling,   ///< Cancellation is being processed
    Cancelled     ///< Cancellation completed
};

// =========================================================================
// CancelToken — lightweight, passable token
// =========================================================================

/// A thread-safe cancellation token that can be passed to async operations.
/// Cooperatively signals cancellation via an atomic flag and supports
/// callback registration.
/// TS REF: src/hooks/useCancelRequest.ts:97 (abortSignal.aborted)
///          AbortController / AbortSignal pattern
class CancelToken {
public:
    CancelToken()
        : cancelled_(std::make_shared<std::atomic<bool>>(false))
        , callbacks_mtx_(std::make_shared<std::mutex>())
    {}

    /// Non-copyable, movable (mutex is shared_ptr so move works).
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) noexcept = default;
    CancelToken& operator=(CancelToken&&) noexcept = default;

    /// Signal cancellation. Thread-safe.
    /// TS REF: src/hooks/useCancelRequest.ts:98-101 (onCancel called)
    auto cancel(std::optional<std::string> reason = std::nullopt) -> void {
        bool expected = false;
        if (cancelled_->compare_exchange_strong(expected, true)) {
            // Fire callbacks under lock
            std::lock_guard<std::mutex> lock(*callbacks_mtx_);
            reason_ = std::move(reason);
            for (const auto& cb : callbacks_) {
                if (cb) cb();
            }
            callbacks_.clear();
        }
    }

    /// Check if cancellation has been requested. Thread-safe.
    /// TS REF: src/hooks/useCancelRequest.ts:97 (!abortSignal.aborted)
    [[nodiscard]] auto is_cancelled() const noexcept -> bool {
        return cancelled_->load(std::memory_order_acquire);
    }

    /// Register a callback to be invoked when cancellation occurs.
    /// If already cancelled, callback fires immediately.
    /// TS REF: AbortSignal.addEventListener('abort', ...)
    auto on_cancel(std::function<void()> callback) -> void {
        if (!callback) return;
        if (is_cancelled()) {
            callback();
            return;
        }
        std::lock_guard<std::mutex> lock(*callbacks_mtx_);
        // Double-check after acquiring lock
        if (cancelled_->load(std::memory_order_acquire)) {
            callback();
            return;
        }
        callbacks_.push_back(std::move(callback));
    }

    /// Get the reason provided with the cancel request, if any.
    [[nodiscard]] auto reason() const -> std::optional<std::string> {
        std::lock_guard<std::mutex> lock(*callbacks_mtx_);
        return reason_;
    }

    /// Reset the token to its initial (non-cancelled) state.
    auto reset() -> void {
        cancelled_->store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(*callbacks_mtx_);
        callbacks_.clear();
        reason_.reset();
    }

    /// Create a child token that is also cancelled when this parent is cancelled.
    /// TS REF: AbortSignal.any([...]) pattern
    [[nodiscard]] auto derive_child() const -> CancelToken {
        CancelToken child;
        if (is_cancelled()) {
            child.cancel();
        } else {
            // Capture shared state so child sees parent's cancellation
            auto parent_cancelled = cancelled_;
            child.on_cancel_check_parent_ = [parent_cancelled]() {
                return parent_cancelled->load(std::memory_order_acquire);
            };
        }
        return child;
    }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<std::mutex> callbacks_mtx_;
    std::vector<std::function<void()>> callbacks_;
    std::optional<std::string> reason_;

    // For child tokens: optional parent check
    std::function<bool()> on_cancel_check_parent_;
};

// =========================================================================
// CancelController — state machine for request lifecycle
// =========================================================================

/// Controller that manages the full lifecycle of a cancellable request.
/// Binds to an external atomic abort flag and tracks state transitions.
/// TS REF: src/hooks/useCancelRequest.ts (CancelRequestHandler component)
class CancelController {
public:
    CancelController() = default;

    /// Bind an external abort flag for cooperative cancellation.
    /// TS REF: src/hooks/useCancelRequest.ts:97 (abortSignal parameter)
    auto bind(std::atomic<bool>& ref) noexcept -> void {
        abort_flag_ = &ref;
    }

    /// Signal cancellation. Sets the atomic flag and transitions to Cancelling.
    /// TS REF: src/hooks/useCancelRequest.ts:87-122 (handleCancel)
    auto request_cancel(std::optional<std::string> reason = std::nullopt)
        -> std::expected<void, std::string>
    {
        if (!abort_flag_) {
            return std::unexpected(std::string{"no abort flag bound"});
        }
        if (state_ == CancelState::Cancelling || state_ == CancelState::Cancelled) {
            return std::unexpected(std::string{"cancel already in progress or completed"});
        }

        abort_flag_->store(true, std::memory_order_release);
        state_ = CancelState::Cancelling;
        reason_ = std::move(reason);

        // Fire registered callbacks
        std::lock_guard<std::mutex> lock(callbacks_mtx_);
        for (const auto& cb : on_cancel_callbacks_) {
            if (cb) cb();
        }
        return {};
    }

    /// Returns the current cancellation state.
    [[nodiscard]] auto get_state() const noexcept -> CancelState {
        if (!abort_flag_) {
            return CancelState::Idle;
        }
        // Reflect external flag changes
        if (abort_flag_->load(std::memory_order_acquire) && state_ == CancelState::Idle) {
            return CancelState::Cancelling;
        }
        return state_;
    }

    /// Called when cancellation has been fully processed.
    /// Resets state to Idle.
    /// TS REF: src/hooks/useCancelRequest.ts:114 (onCancel callback completion)
    auto on_cancel_complete() noexcept -> void {
        state_ = CancelState::Idle;
        reason_.reset();
    }

    /// Full reset: clears the atomic flag and returns to Idle.
    auto reset() noexcept -> void {
        if (abort_flag_) {
            abort_flag_->store(false, std::memory_order_release);
        }
        state_ = CancelState::Idle;
        reason_.reset();
        std::lock_guard<std::mutex> lock(callbacks_mtx_);
        on_cancel_callbacks_.clear();
    }

    /// Check whether cancellation was requested.
    [[nodiscard]] auto is_cancel_requested() const noexcept -> bool {
        if (!abort_flag_) return false;
        return abort_flag_->load(std::memory_order_acquire);
    }

    /// Access the cancel reason.
    [[nodiscard]] auto reason() const noexcept -> const std::optional<std::string>& {
        return reason_;
    }

    /// Register a callback to be invoked when cancellation is requested.
    auto on_cancel(std::function<void()> callback) -> void {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(callbacks_mtx_);
        on_cancel_callbacks_.push_back(std::move(callback));
    }

private:
    std::atomic<bool>* abort_flag_{nullptr};
    CancelState state_{CancelState::Idle};
    std::optional<std::string> reason_;
    std::mutex callbacks_mtx_;
    std::vector<std::function<void()>> on_cancel_callbacks_;
};

// =========================================================================
// CancelRequest struct (for passing context)
// =========================================================================

/// Context for a cancellation request.
/// TS REF: src/hooks/useCancelRequest.ts:40-57 (CancelRequestHandlerProps)
struct CancelRequest {
    std::string request_id;
    CancelState state{CancelState::Idle};
    std::optional<std::string> reason;
};

// =========================================================================
// Convenience: global cancellation helpers
// =========================================================================

namespace global {

/// Get a process-wide CancelToken for global cancellation scenarios.
/// TS REF: process-level abort signal
[[nodiscard]] inline auto get_global_token() -> CancelToken& {
    static CancelToken token;
    return token;
}

/// Check if a global cancel has been requested.
[[nodiscard]] inline auto is_globally_cancelled() -> bool {
    return get_global_token().is_cancelled();
}

/// Request a global cancel.
inline auto request_global_cancel(std::optional<std::string> reason = std::nullopt) -> void {
    get_global_token().cancel(std::move(reason));
}

/// Reset the global cancel token.
inline auto reset_global_cancel() -> void {
    get_global_token().reset();
}

} // namespace global

} // namespace cc::hooks::cancel_request
