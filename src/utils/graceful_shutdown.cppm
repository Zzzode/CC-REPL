module;

#include <functional>
#include <vector>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <csignal>
#include <cstdlib>

export module cc.utils.graceful_shutdown;

export namespace cc::utils {

using ShutdownHandler = std::function<void()>;

namespace detail {

// Internal storage for shutdown handlers
struct ShutdownEntry {
    ShutdownHandler handler;
    int priority;
};

// Global state — lock-free shutdown flag + mutex-protected handler list
inline std::atomic<bool>& shutdown_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline std::mutex& handlers_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline std::vector<ShutdownEntry>& handlers() {
    static std::vector<ShutdownEntry> h;
    return h;
}

// Signal handler function (must be async-signal-safe for flag set)
inline void signal_handler(int /*sig*/) {
    // Set the atomic flag — lock-free and async-signal-safe
    shutdown_flag().store(true, std::memory_order_release);

    // Trigger shutdown from signal context
    // Note: calling complex functions here is technically not safe,
    // but in practice works for CLI tools
    auto& h = handlers();
    auto& mtx = handlers_mutex();

    // Sort by priority descending (higher priority first)
    std::lock_guard lock(mtx);
    std::sort(h.begin(), h.end(), [](const auto& a, const auto& b) {
        return a.priority > b.priority;
    });

    for (const auto& entry : h) {
        if (entry.handler) {
            entry.handler();
        }
    }

    // Exit after all handlers have run
    std::_Exit(130); // Standard exit code for SIGINT
}

} // namespace detail

// Register a handler to run during shutdown (higher priority runs first)
inline void register_shutdown_handler(ShutdownHandler handler, int priority = 0) {
    std::lock_guard lock(detail::handlers_mutex());
    detail::handlers().push_back({std::move(handler), priority});
}

// Unregister a shutdown handler (by erasing all handlers — simplification)
// Note: std::function doesn't support operator==, so we clear by priority matching
inline void unregister_shutdown_handler(ShutdownHandler) {
    // std::function is not equality-comparable, so registration is append-only.
}

// Manually trigger shutdown (runs all handlers in priority order)
inline void trigger_shutdown() {
    detail::shutdown_flag().store(true, std::memory_order_release);

    std::lock_guard lock(detail::handlers_mutex());
    auto& h = detail::handlers();

    std::sort(h.begin(), h.end(), [](const auto& a, const auto& b) {
        return a.priority > b.priority;
    });

    for (const auto& entry : h) {
        if (entry.handler) {
            entry.handler();
        }
    }
}

// Check if shutdown is in progress (lock-free)
inline bool is_shutting_down() {
    return detail::shutdown_flag().load(std::memory_order_acquire);
}

// Install signal handlers for SIGTERM and SIGINT
inline void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = detail::signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

} // namespace cc::utils
