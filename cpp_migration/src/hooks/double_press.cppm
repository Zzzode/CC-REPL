module;
#include <chrono>

export module cc.hooks.double_press;

export namespace cc::hooks {

// 双击检测器：在指定时间阈值内连续两次按键视为双击
class DoublePressDetector {
public:
    explicit DoublePressDetector(std::chrono::milliseconds threshold = std::chrono::milliseconds{300})
        : threshold_(threshold) {}

    // 记录一次按键，如果构成双击返回 true
    bool on_press() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_press_);
        last_press_ = now;

        if (elapsed <= threshold_ && press_count_ > 0) {
            // 双击检测成功，重置状态
            press_count_ = 0;
            return true;
        }
        press_count_ = 1;
        return false;
    }

    // 重置检测器状态
    void reset() {
        press_count_ = 0;
        last_press_ = std::chrono::steady_clock::time_point{};
    }

private:
    std::chrono::milliseconds threshold_;
    int press_count_ = 0;
    std::chrono::steady_clock::time_point last_press_{};
};

} // namespace cc::hooks
