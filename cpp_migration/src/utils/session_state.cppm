module;
#include <functional>
#include <string>
#include <vector>

export module cc.utils.session_state;

export namespace cc::utils {

enum class SessionState {
    Initializing,
    Active,
    Paused,
    Compacting,
    Ending,
    Ended
};

class SessionStateMachine {
public:
    SessionStateMachine() : state_(SessionState::Initializing) {}

    SessionState current() const { return state_; }

    // Attempt a state transition; returns false if transition is invalid
    bool transition(SessionState target) {
        if (!can_transition(target)) return false;

        auto old = state_;
        state_ = target;

        // Notify listeners
        for (auto& cb : callbacks_) {
            cb(old, target);
        }
        return true;
    }

    // Check if a transition to the target state is valid
    bool can_transition(SessionState target) const {
        switch (state_) {
            case SessionState::Initializing:
                return target == SessionState::Active || target == SessionState::Ended;
            case SessionState::Active:
                return target == SessionState::Paused ||
                       target == SessionState::Compacting ||
                       target == SessionState::Ending;
            case SessionState::Paused:
                return target == SessionState::Active || target == SessionState::Ending;
            case SessionState::Compacting:
                return target == SessionState::Active || target == SessionState::Ending;
            case SessionState::Ending:
                return target == SessionState::Ended;
            case SessionState::Ended:
                return false; // Terminal state
        }
        return false;
    }

    // Register a callback for state transitions
    void on_transition(std::function<void(SessionState, SessionState)> callback) {
        callbacks_.push_back(std::move(callback));
    }

private:
    SessionState state_;
    std::vector<std::function<void(SessionState, SessionState)>> callbacks_;
};

} // namespace cc::utils
