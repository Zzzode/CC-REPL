module;
#include <string>
#include <functional>
#include <optional>
#include <vector>

export module cc.hooks.background_task_navigation;

import cc.state.app_state;

export namespace cc::hooks::background_task_navigation {

struct BackgroundTask {
    std::string id;
    std::string label;
    std::string status;
    bool is_active{false};
};

struct BackgroundTaskNavigationState {
    std::vector<BackgroundTask> tasks;
    std::optional<std::size_t> focused_index;
    bool panel_visible{false};
};

struct BackgroundTaskNavigationOptions {
    bool auto_focus_new{true};
};

class BackgroundTaskNavigationHook {
public:
    explicit BackgroundTaskNavigationHook(const BackgroundTaskNavigationOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const BackgroundTaskNavigationState& state() const { return state_; }

    void add_task(BackgroundTask task) {
        state_.tasks.push_back(std::move(task));
        if (options_.auto_focus_new) {
            state_.focused_index = state_.tasks.size() - 1;
        }
        notify();
    }

    void remove_task(std::string_view id) {
        std::erase_if(state_.tasks, [&](const auto& t) { return t.id == id; });
        if (state_.focused_index && *state_.focused_index >= state_.tasks.size()) {
            state_.focused_index = state_.tasks.empty() ? std::nullopt
                : std::optional<std::size_t>(state_.tasks.size() - 1);
        }
        notify();
    }

    void focus_next() {
        if (state_.tasks.empty()) return;
        state_.focused_index = (state_.focused_index.value_or(0) + 1) % state_.tasks.size();
        notify();
    }

    void focus_prev() {
        if (state_.tasks.empty()) return;
        auto idx = state_.focused_index.value_or(0);
        state_.focused_index = (idx == 0) ? state_.tasks.size() - 1 : idx - 1;
        notify();
    }

    void toggle_panel() {
        state_.panel_visible = !state_.panel_visible;
        notify();
    }

    void on_change(std::function<void(const BackgroundTaskNavigationState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    BackgroundTaskNavigationState state_;
    BackgroundTaskNavigationOptions options_;
    std::vector<std::function<void(const BackgroundTaskNavigationState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::background_task_navigation
