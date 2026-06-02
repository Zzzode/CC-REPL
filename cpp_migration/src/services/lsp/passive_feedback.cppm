module;
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
export module cc.services.lsp.passive_feedback;

export namespace cc::services::lsp {

// LSP health metrics
struct LspHealth {
    int requests{0};
    int errors{0};
    std::chrono::milliseconds avg_latency{0};
};

namespace detail {
    inline std::mutex metrics_mutex;
    inline std::atomic<int> total_requests{0};
    inline std::atomic<int> total_errors{0};
    inline std::atomic<long long> total_latency_ms{0};
} // namespace detail

// Record an LSP event for passive feedback
auto record_lsp_event(std::string_view event_type, std::map<std::string, std::string> data)
    -> void {
    detail::total_requests.fetch_add(1);
    if (event_type == "error") {
        detail::total_errors.fetch_add(1);
    }
    // Extract latency if present
    if (auto it = data.find("latency_ms"); it != data.end()) {
        detail::total_latency_ms.fetch_add(std::stoll(it->second));
    }
}

// Get current LSP health metrics
auto get_lsp_health() -> LspHealth {
    int requests = detail::total_requests.load();
    int errors = detail::total_errors.load();
    long long total_ms = detail::total_latency_ms.load();

    auto avg = requests > 0
        ? std::chrono::milliseconds{total_ms / requests}
        : std::chrono::milliseconds{0};

    return LspHealth{requests, errors, avg};
}

// Reset all LSP metrics
auto reset_lsp_metrics() -> void {
    detail::total_requests.store(0);
    detail::total_errors.store(0);
    detail::total_latency_ms.store(0);
}

} // namespace cc::services::lsp
