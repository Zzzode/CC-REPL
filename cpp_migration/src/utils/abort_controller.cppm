/// @file abort_controller.cppm
/// @brief AbortController/AbortSignal C++ equivalent, combined signal composition, cancellation token pattern
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <string_view>

export module cc.utils.abort_controller;

export namespace cc::utils::abort_controller {

// ---------------------------------------------------------------------------
// AbortSignal - Cancellation observation token
// ---------------------------------------------------------------------------

/// Reason for abort
struct AbortReason {
    std::string message;
    bool is_timeout{false};
};

/// Forward declaration
class AbortController;

/// AbortSignal provides cancellation observation.
/// It is a shared, thread-safe token that listeners can poll or subscribe to.
class AbortSignal {
public:
    using Listener = std::function<void(const AbortReason&)>;

    AbortSignal() = default;

    /// Check if the signal has been aborted
    [[nodiscard]] auto aborted() const -> bool;

    /// Get the abort reason (nullopt if not aborted)
    [[nodiscard]] auto reason() const -> std::optional<AbortReason>;

    /// Register a listener to be called when abort fires.
    /// If already aborted, the listener is called immediately.
    /// Returns a token for removing the listener.
    auto on_abort(Listener listener) -> std::uint64_t;

    /// Remove a previously registered listener by token
    void remove_listener(std::uint64_t token);

    /// Create a signal that fires after the given timeout
    [[nodiscard]] static auto timeout(std::chrono::milliseconds ms)
        -> std::shared_ptr<AbortSignal>;

private:
    friend class AbortController;
    friend class CombinedAbortSignal;

    struct State {
        std::atomic<bool> aborted{false};
        mutable std::mutex mu;
        std::optional<AbortReason> reason;
        std::vector<std::pair<std::uint64_t, Listener>> listeners;
        std::uint64_t next_token{1};
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();

    void fire(AbortReason reason);
};

// ---------------------------------------------------------------------------
// AbortController - Cancellation trigger
// ---------------------------------------------------------------------------

/// Default max listeners for standard operations
inline constexpr int DEFAULT_MAX_LISTENERS = 50;

/// AbortController owns an AbortSignal and provides the ability to trigger abort.
class AbortController {
public:
    /// Create a controller with configurable listener limit
    explicit AbortController(int max_listeners = DEFAULT_MAX_LISTENERS);

    /// Get the associated signal (shared ownership)
    [[nodiscard]] auto signal() const -> std::shared_ptr<AbortSignal>;

    /// Trigger abort with an optional reason message
    void abort(std::string_view reason = "aborted");

    /// Trigger abort with a specific reason
    void abort(AbortReason reason);

    /// Check if already aborted
    [[nodiscard]] auto aborted() const -> bool;

private:
    std::shared_ptr<AbortSignal> signal_;
    int max_listeners_;
};

// ---------------------------------------------------------------------------
// Child AbortController - propagates parent abort to child
// ---------------------------------------------------------------------------

/// Creates a child AbortController that aborts when its parent aborts.
/// Aborting the child does NOT affect the parent.
/// Memory-safe: Uses weak references so the parent doesn't retain abandoned children.
[[nodiscard]] auto create_child_abort_controller(
    AbortController& parent,
    int max_listeners = DEFAULT_MAX_LISTENERS)
    -> std::unique_ptr<AbortController>;

// ---------------------------------------------------------------------------
// Combined AbortSignal - composition of multiple signals
// ---------------------------------------------------------------------------

/// Result of creating a combined signal, includes cleanup handle
struct CombinedSignalResult {
    std::shared_ptr<AbortSignal> signal;

    /// Call cleanup to remove event listeners and clear internal timer.
    /// Must be called when the combined signal is no longer needed.
    std::function<void()> cleanup;
};

/// Options for creating a combined abort signal
struct CombinedSignalOptions {
    std::shared_ptr<AbortSignal> signal_b;
    std::optional<std::chrono::milliseconds> timeout_ms;
};

/// Creates a combined AbortSignal that aborts when either:
/// - The primary signal aborts
/// - The optional secondary signal (signal_b) aborts
/// - The optional timeout elapses
///
/// Returns the combined signal and a cleanup function that removes
/// event listeners and clears the internal timeout timer.
[[nodiscard]] auto create_combined_abort_signal(
    std::shared_ptr<AbortSignal> primary_signal,
    const CombinedSignalOptions& opts) -> CombinedSignalResult;

/// Overload without options for simple wrapping
[[nodiscard]] auto create_combined_abort_signal(
    std::shared_ptr<AbortSignal> primary_signal) -> CombinedSignalResult;

// ---------------------------------------------------------------------------
// Cancellation token pattern (simplified interface)
// ---------------------------------------------------------------------------

/// A lightweight cancellation token for passing through async call chains
class CancellationToken {
public:
    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<AbortSignal> signal);

    /// Check if cancellation has been requested
    [[nodiscard]] auto is_cancelled() const -> bool;

    /// Throw if cancelled (convenience for checking at suspension points)
    [[nodiscard]] auto throw_if_cancelled() const
        -> std::expected<void, std::string>;

    /// Get the underlying signal
    [[nodiscard]] auto signal() const -> std::shared_ptr<AbortSignal>;

    /// Create a token that is never cancelled
    [[nodiscard]] static auto none() -> CancellationToken;

private:
    std::shared_ptr<AbortSignal> signal_;
};

} // namespace cc::utils::abort_controller
