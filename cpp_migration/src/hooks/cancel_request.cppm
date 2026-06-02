// cc.hooks.cancel_request — handles request cancellation
// Migrated from: useCancelRequest.ts
module;

#include <atomic>
#include <string>
#include <string_view>
#include <expected>
#include <optional>

export module cc.hooks.cancel_request;

export namespace cc::hooks::cancel_request {

enum class CancelState {
    Idle,
    Pending,
    Cancelling,
    Cancelled
};

struct CancelRequest {
    std::string request_id;
    CancelState state;
    std::optional<std::string> reason;
};

class CancelController {
public:
    CancelController() = default;

    /// Bind an external abort flag for cooperative cancellation.
    void bind(std::atomic<bool>& ref) noexcept {
        abort_flag_ = &ref;
    }

    /// Signal cancellation: sets the atomic flag and transitions to Cancelling.
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
        return {};
    }

    /// Returns the current cancellation state.
    [[nodiscard]] auto get_state() const noexcept -> CancelState {
        if (!abort_flag_) {
            return CancelState::Idle;
        }
        // If the flag is set but state hasn't been explicitly advanced, reflect it.
        if (abort_flag_->load(std::memory_order_acquire) && state_ == CancelState::Idle) {
            return CancelState::Cancelling;
        }
        return state_;
    }

    /// Called when cancellation has been fully processed; resets state to Idle.
    void on_cancel_complete() noexcept {
        state_ = CancelState::Idle;
        reason_.reset();
    }

    /// Full reset: clears the atomic flag and returns to Idle.
    void reset() noexcept {
        if (abort_flag_) {
            abort_flag_->store(false, std::memory_order_release);
        }
        state_ = CancelState::Idle;
        reason_.reset();
    }

    /// Check whether cancellation was requested (reads the atomic flag directly).
    [[nodiscard]] auto is_cancel_requested() const noexcept -> bool {
        if (!abort_flag_) return false;
        return abort_flag_->load(std::memory_order_acquire);
    }

    /// Access the reason provided with the cancel request, if any.
    [[nodiscard]] auto reason() const noexcept -> const std::optional<std::string>& {
        return reason_;
    }

private:
    std::atomic<bool>* abort_flag_{nullptr};
    CancelState state_{CancelState::Idle};
    std::optional<std::string> reason_;
};

} // namespace cc::hooks::cancel_request
