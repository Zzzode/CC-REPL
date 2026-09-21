module;
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

export module cc.utils.fps_tracker;

export namespace cc::utils::fps {

struct FpsMetrics {
    double average_fps = 0.0;
    double low_1_pct_fps = 0.0;
};

namespace detail {

[[nodiscard]] inline double round_to_two_decimals(double value) {
    return std::round(value * 100.0) / 100.0;
}

[[nodiscard]] inline double now_ms() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
}

} // namespace detail

class FpsTracker {
public:
    void record(double duration_ms) {
        record(duration_ms, detail::now_ms());
    }

    void record(double duration_ms, double now_ms) {
        if (!first_render_time_.has_value()) {
            first_render_time_ = now_ms;
        }
        last_render_time_ = now_ms;
        frame_durations_.push_back(duration_ms);
    }

    [[nodiscard]] std::optional<FpsMetrics> get_metrics() const {
        if (frame_durations_.empty() || !first_render_time_.has_value() || !last_render_time_.has_value()) {
            return std::nullopt;
        }

        const double total_time_ms = *last_render_time_ - *first_render_time_;
        if (total_time_ms <= 0.0) {
            return std::nullopt;
        }

        const auto total_frames = static_cast<double>(frame_durations_.size());
        const double average_fps = total_frames / (total_time_ms / 1000.0);

        auto sorted = frame_durations_;
        std::sort(sorted.begin(), sorted.end(), std::greater<>{});
        const auto p99_index = static_cast<std::size_t>(
            std::max(0.0, std::ceil(static_cast<double>(sorted.size()) * 0.01) - 1.0));
        const double p99_frame_time_ms = sorted[p99_index];
        const double low_1_pct_fps = p99_frame_time_ms > 0.0 ? 1000.0 / p99_frame_time_ms : 0.0;

        return FpsMetrics{
            .average_fps = detail::round_to_two_decimals(average_fps),
            .low_1_pct_fps = detail::round_to_two_decimals(low_1_pct_fps),
        };
    }

private:
    std::vector<double> frame_durations_;
    std::optional<double> first_render_time_ = std::nullopt;
    std::optional<double> last_render_time_ = std::nullopt;
};

} // namespace cc::utils::fps
