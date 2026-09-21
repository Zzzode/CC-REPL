module;

#include <functional>
#include <vector>
#include <initializer_list>
#include <csignal>
#include <cstring>
#include <map>
#include <mutex>

export module cc.utils.signal_utils;

export namespace cc::utils {

// RAII signal mask — restores original mask on destruction
class ScopedSignalMask {
public:
    ScopedSignalMask() { sigemptyset(&original_mask_); }

    explicit ScopedSignalMask(sigset_t original)
        : original_mask_(original), active_(true) {}

    ~ScopedSignalMask() { restore(); }

    // Non-copyable, movable
    ScopedSignalMask(const ScopedSignalMask&) = delete;
    ScopedSignalMask& operator=(const ScopedSignalMask&) = delete;

    ScopedSignalMask(ScopedSignalMask&& other) noexcept
        : original_mask_(other.original_mask_), active_(other.active_) {
        other.active_ = false;
    }

    ScopedSignalMask& operator=(ScopedSignalMask&& other) noexcept {
        if (this != &other) {
            restore();
            original_mask_ = other.original_mask_;
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    // Manually restore the original signal mask
    void restore() {
        if (active_) {
            sigprocmask(SIG_SETMASK, &original_mask_, nullptr);
            active_ = false;
        }
    }

private:
    sigset_t original_mask_{};
    bool active_ = false;
};

// Block a set of signals, returns an RAII guard that restores the mask
inline ScopedSignalMask block_signals(std::initializer_list<int> signals) {
    sigset_t new_mask, old_mask;
    sigemptyset(&new_mask);
    for (int sig : signals) {
        sigaddset(&new_mask, sig);
    }
    sigprocmask(SIG_BLOCK, &new_mask, &old_mask);
    return ScopedSignalMask(old_mask);
}

// Ignore a specific signal
inline void ignore_signal(int sig) {
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);
}

// Reset a signal to its default behavior
inline void reset_signal(int sig) {
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);
}

namespace detail {

// Global handler map for custom signal handlers
inline std::mutex& signal_handlers_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline std::map<int, std::function<void(int)>>& signal_handlers_map() {
    static std::map<int, std::function<void(int)>> handlers;
    return handlers;
}

// C-compatible signal handler that dispatches to stored std::function
inline void dispatch_signal(int sig) {
    std::lock_guard lock(signal_handlers_mutex());
    auto& handlers = signal_handlers_map();
    auto it = handlers.find(sig);
    if (it != handlers.end() && it->second) {
        it->second(sig);
    }
}

} // namespace detail

// Set a custom signal handler using std::function
inline void set_signal_handler(int sig, std::function<void(int)> handler) {
    {
        std::lock_guard lock(detail::signal_handlers_mutex());
        detail::signal_handlers_map()[sig] = std::move(handler);
    }

    struct sigaction sa{};
    sa.sa_handler = detail::dispatch_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);
}

// Get the list of signals that are pending delivery
inline std::vector<int> pending_signals() {
    std::vector<int> result;
    sigset_t pending;
    if (sigpending(&pending) == 0) {
        // Check common signals
        for (int sig = 1; sig < 32; ++sig) {
            if (sigismember(&pending, sig)) {
                result.push_back(sig);
            }
        }
    }
    return result;
}

} // namespace cc::utils
