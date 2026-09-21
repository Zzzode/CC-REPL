// HTTP-level retry executor for libcurl-backed clients.
// Exposes the simple int-based return contract used by cc.services.api.sse:
//   return == 0          -> success
//   return > 0           -> HTTP status (retry if in cfg.retry_on_http)
//   return < 0           -> libcurl transport error (always retry up to max)
module;
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <thread>
#include <vector>

export module cc.services.api.with_retry_simple;

export namespace cc::services::api::with_retry_simple {

// =========================================================================
// RetryConfig
// =========================================================================

struct RetryConfig {
    int                max_attempts    = 3;
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{10000};
    std::vector<int>   retry_on_http   = {429, 500, 502, 503, 504};
    double             jitter_ratio    = 0.2;  // ±20% uniform jitter
};

// =========================================================================
// BackoffDelay (exposed for tests)
// =========================================================================

[[nodiscard]] inline std::chrono::milliseconds
BackoffDelay(const RetryConfig& cfg, int attempt_0based) noexcept {
    const auto attempt = static_cast<std::uint32_t>(std::max(0, attempt_0based));
    const double raw =
        static_cast<double>(cfg.base_delay.count()) *
        std::pow(2.0, static_cast<double>(attempt));
    auto capped = std::chrono::milliseconds{
        static_cast<std::int64_t>(std::min<double>(
            static_cast<double>(cfg.max_delay.count()), raw))};

    if (cfg.jitter_ratio > 0.0) {
        std::mt19937_64 gen{std::random_device{}()};
        const double lo = 1.0 - cfg.jitter_ratio;
        const double hi = 1.0 + cfg.jitter_ratio;
        std::uniform_real_distribution<double> dist(lo, hi);
        capped = std::chrono::milliseconds{static_cast<std::int64_t>(
            std::max<double>(0.0,
                             static_cast<double>(capped.count()) * dist(gen)))};
    }
    return capped;
}

// =========================================================================
// RunWithRetry
// =========================================================================

inline int RunWithRetry(const RetryConfig& cfg,
                        std::function<int(int /*attempt 0-based*/)> fn) {
    const int max_attempts = std::max(1, cfg.max_attempts);
    int last_rc = 0;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const int rc = fn(attempt);
        last_rc = rc;

        if (rc == 0) return 0;
        if (rc >= 200 && rc < 300) return rc;

        const bool is_transport = rc < 0;
        bool should_retry = is_transport;
        if (rc > 0) {
            for (int h : cfg.retry_on_http) {
                if (h == rc) { should_retry = true; break; }
            }
        }

        if (!should_retry) return rc;
        if (attempt + 1 >= max_attempts) return rc;

        const auto delay = BackoffDelay(cfg, attempt);
        std::this_thread::sleep_for(delay);
    }
    return last_rc;
}

}  // namespace cc::services::api::with_retry_simple
