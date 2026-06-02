module;
#include <string>
#include <functional>
#include <optional>
#include <vector>

export module cc.hooks.after_first_render;

import cc.state.app_state;

export namespace cc::hooks::after_first_render {

struct AfterFirstRenderState {
    bool has_rendered{false};
    bool callbacks_executed{false};
};

struct AfterFirstRenderOptions {
    bool execute_once{true};
};

class AfterFirstRenderHook {
public:
    explicit AfterFirstRenderHook(const AfterFirstRenderOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const AfterFirstRenderState& state() const { return state_; }

    /// Call this when the first render completes
    void mark_rendered() {
        if (state_.has_rendered && options_.execute_once) return;
        state_.has_rendered = true;
        execute_callbacks();
    }

    /// Register a callback to run after first render
    void on_first_render(std::function<void()> callback) {
        if (state_.has_rendered && options_.execute_once) {
            callback(); // Already rendered, execute immediately
            return;
        }
        callbacks_.push_back(std::move(callback));
    }

    void on_change(std::function<void(const AfterFirstRenderState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void execute_callbacks() {
        for (const auto& cb : callbacks_) cb();
        state_.callbacks_executed = true;
        if (options_.execute_once) callbacks_.clear();
        notify();
    }

    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    AfterFirstRenderState state_;
    AfterFirstRenderOptions options_;
    std::vector<std::function<void()>> callbacks_;
    std::vector<std::function<void(const AfterFirstRenderState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::after_first_render
