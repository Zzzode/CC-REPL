module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <chrono>

export module cc.hooks.teammate_view_auto_exit;

import cc.state.app_state;

export namespace cc::hooks::teammate_view_auto_exit {

struct TeammateViewAutoExitState {
    bool in_teammate_view{false};
    bool auto_exit_scheduled{false};
    std::chrono::steady_clock::time_point entered_at;
    std::optional<std::chrono::seconds> timeout;
};

struct TeammateViewAutoExitOptions {
    std::chrono::seconds default_timeout{300}; // 5 minutes
    bool exit_on_user_input{true};
    bool exit_on_teammate_disconnect{true};
};

class TeammateViewAutoExitHook {
public:
    explicit TeammateViewAutoExitHook(const TeammateViewAutoExitOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const TeammateViewAutoExitState& state() const { return state_; }

    void enter_teammate_view() {
        state_.in_teammate_view = true;
        state_.entered_at = std::chrono::steady_clock::now();
        state_.timeout = options_.default_timeout;
        state_.auto_exit_scheduled = true;
        notify();
    }

    void exit_teammate_view() {
        state_.in_teammate_view = false;
        state_.auto_exit_scheduled = false;
        notify();
    }

    /// Check if timeout has elapsed
    [[nodiscard]] bool should_auto_exit() const {
        if (!state_.in_teammate_view || !state_.auto_exit_scheduled) return false;
        if (!state_.timeout) return false;
        auto elapsed = std::chrono::steady_clock::now() - state_.entered_at;
        return elapsed >= *state_.timeout;
    }

    void cancel_auto_exit() {
        state_.auto_exit_scheduled = false;
        notify();
    }

    void on_user_input() {
        if (options_.exit_on_user_input && state_.in_teammate_view) {
            exit_teammate_view();
        }
    }

    void on_teammate_disconnect() {
        if (options_.exit_on_teammate_disconnect && state_.in_teammate_view) {
            exit_teammate_view();
        }
    }

    void on_change(std::function<void(const TeammateViewAutoExitState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    TeammateViewAutoExitState state_;
    TeammateViewAutoExitOptions options_;
    std::vector<std::function<void(const TeammateViewAutoExitState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::teammate_view_auto_exit
