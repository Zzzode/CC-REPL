module;
#include <chrono>

export module cc.hooks.double_press;

export namespace cc::hooks {


class DoublePressDetector {
public:
    explicit DoublePressDetector(std::chrono::milliseconds threshold = std::chrono::milliseconds{300})
        : threshold_(threshold) {}


    bool on_press() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_press_);
        last_press_ = now;

        if (elapsed <= threshold_ && press_count_ > 0) {

            press_count_ = 0;
            return true;
        }
        press_count_ = 1;
        return false;
    }


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
