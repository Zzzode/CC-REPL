module;
#include <chrono>
#include <string>
#include <format>

export module cc.hooks.elapsed_time;

export namespace cc::hooks {


class ElapsedTimeTracker {
public:

    void start() {
        start_time_ = std::chrono::steady_clock::now();
        running_ = true;
    }


    std::chrono::milliseconds get_elapsed() const {
        if (!running_) return std::chrono::milliseconds{0};
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_);
    }


    std::string format_elapsed() const {
        auto ms = get_elapsed();
        auto total_seconds = ms.count() / 1000;

        if (total_seconds < 60) {

            auto sec = ms.count() / 1000;
            auto tenth = (ms.count() % 1000) / 100;
            return std::format("{}.{}s", sec, tenth);
        }

        auto minutes = total_seconds / 60;
        auto seconds = total_seconds % 60;
        return std::format("{}m {}s", minutes, seconds);
    }


    void reset() {
        running_ = false;
        start_time_ = std::chrono::steady_clock::time_point{};
    }

private:
    bool running_ = false;
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace cc::hooks
