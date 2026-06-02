module;
#include <chrono>
#include <string>
#include <format>

export module cc.hooks.elapsed_time;

export namespace cc::hooks {

// 计时器：追踪操作耗时并格式化为可读字符串
class ElapsedTimeTracker {
public:
    // 启动计时
    void start() {
        start_time_ = std::chrono::steady_clock::now();
        running_ = true;
    }

    // 获取已经过的毫秒数
    std::chrono::milliseconds get_elapsed() const {
        if (!running_) return std::chrono::milliseconds{0};
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_);
    }

    // 格式化为人类可读字符串（如 "2.3s", "1m 5s"）
    std::string format_elapsed() const {
        auto ms = get_elapsed();
        auto total_seconds = ms.count() / 1000;

        if (total_seconds < 60) {
            // 小于 60 秒：显示秒和十分位
            auto sec = ms.count() / 1000;
            auto tenth = (ms.count() % 1000) / 100;
            return std::format("{}.{}s", sec, tenth);
        }
        // 大于等于 60 秒：显示分和秒
        auto minutes = total_seconds / 60;
        auto seconds = total_seconds % 60;
        return std::format("{}m {}s", minutes, seconds);
    }

    // 重置计时器
    void reset() {
        running_ = false;
        start_time_ = std::chrono::steady_clock::time_point{};
    }

private:
    bool running_ = false;
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace cc::hooks
