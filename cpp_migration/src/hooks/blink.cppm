module;
#include <chrono>

export module cc.hooks.blink;

export namespace cc::hooks {


class BlinkState {
public:
    explicit BlinkState(std::chrono::milliseconds interval = std::chrono::milliseconds{500})
        : interval_(interval) {}


    bool is_visible() const {
        return visible_;
    }


    void tick() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_);

        if (elapsed >= interval_) {
            visible_ = !visible_;
            last_tick_ = now;
        }
    }


    void reset() {
        visible_ = true;
        last_tick_ = std::chrono::steady_clock::now();
    }

private:
    std::chrono::milliseconds interval_;
    bool visible_ = true;
    std::chrono::steady_clock::time_point last_tick_ = std::chrono::steady_clock::now();
};

} // namespace cc::hooks
