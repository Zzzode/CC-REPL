module;
#include <chrono>

export module cc.hooks.blink;

export namespace cc::hooks {

// 光标闪烁状态管理器
class BlinkState {
public:
    explicit BlinkState(std::chrono::milliseconds interval = std::chrono::milliseconds{500})
        : interval_(interval) {}

    // 获取当前是否可见（基于闪烁周期）
    bool is_visible() const {
        return visible_;
    }

    // 推进一个 tick（切换可见性，每次调用间隔应为 interval）
    void tick() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_);

        if (elapsed >= interval_) {
            visible_ = !visible_;
            last_tick_ = now;
        }
    }

    // 重置为可见状态（如用户输入后重置光标闪烁）
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
