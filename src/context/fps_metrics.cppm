/// @file fps_metrics.cppm
/// @brief FPS and render performance metrics context.
/// Migrated from src/context/fpsMetrics.tsx
module;

#include <chrono>
#include <deque>
#include <numeric>
#include <algorithm>
#include <cstddef>

export module cc.context.fps_metrics;

export namespace cc::context {

/// Tracks frame render timing for performance monitoring
class FpsMetrics {
    static constexpr std::size_t MAX_SAMPLES = 60;
    std::deque<std::chrono::steady_clock::time_point> frame_times_;

public:
    /// Record a frame render
    void record_frame() {
        auto now = std::chrono::steady_clock::now();
        frame_times_.push_back(now);
        while (frame_times_.size() > MAX_SAMPLES) {
            frame_times_.pop_front();
        }
    }
    
    /// Get current FPS estimate
    [[nodiscard]] double get_fps() const {
        if (frame_times_.size() < 2) return 0.0;
        
        auto duration = frame_times_.back() - frame_times_.front();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        if (ms == 0) return 0.0;
        
        return static_cast<double>(frame_times_.size() - 1) * 1000.0 / static_cast<double>(ms);
    }
    
    /// Get average frame time in ms
    [[nodiscard]] double get_avg_frame_time_ms() const {
        auto fps = get_fps();
        if (fps == 0.0) return 0.0;
        return 1000.0 / fps;
    }
    
    /// Reset metrics
    void reset() { frame_times_.clear(); }
};

} // namespace cc::context
