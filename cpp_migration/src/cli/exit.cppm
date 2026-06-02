module;
#include <functional>
#include <vector>
#include <mutex>
#include <cstdlib>
#include <atomic>

export module cc.cli.exit;

export namespace cc::cli {

namespace detail {
    // Global state for exit management
    inline std::vector<std::function<void()>>& get_exit_hooks() {
        static std::vector<std::function<void()>> hooks;
        return hooks;
    }

    inline std::mutex& get_exit_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    inline std::atomic<int>& get_exit_code_ref() {
        static std::atomic<int> code{0};
        return code;
    }
}

// Register a hook to be called before process exit
void register_exit_hook(std::function<void()> hook) {
    std::lock_guard lock(detail::get_exit_mutex());
    detail::get_exit_hooks().push_back(std::move(hook));
}

// Get the current planned exit code
int get_exit_code() {
    return detail::get_exit_code_ref().load();
}

// Set the exit code without actually exiting
void set_exit_code(int code) {
    detail::get_exit_code_ref().store(code);
}

// Exit the CLI process, running all registered hooks first
[[noreturn]] void cli_exit(int code) {
    // Run exit hooks in reverse registration order
    {
        std::lock_guard lock(detail::get_exit_mutex());
        auto& hooks = detail::get_exit_hooks();
        for (auto it = hooks.rbegin(); it != hooks.rend(); ++it) {
            try {
                (*it)();
            } catch (...) {
                // Swallow exceptions during exit — nothing we can do
            }
        }
        hooks.clear();
    }

    // Flush output streams before exiting
    std::fflush(stdout);
    std::fflush(stderr);

    std::exit(code);
}

} // namespace cc::cli
