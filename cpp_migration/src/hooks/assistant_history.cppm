module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <chrono>

export module cc.hooks.assistant_history;

import cc.state.app_state;

export namespace cc::hooks::assistant_history {

enum class MessageRole { user, assistant, system };

struct HistoryMessage {
    std::string id;
    MessageRole role;
    std::string content;
    std::chrono::system_clock::time_point timestamp;
};

struct AssistantHistoryState {
    std::vector<HistoryMessage> messages;
    std::optional<std::string> session_id;
    bool loading{false};
};

struct AssistantHistoryOptions {
    std::size_t max_messages{1000};
    bool auto_persist{true};
};

class AssistantHistoryHook {
public:
    explicit AssistantHistoryHook(const AssistantHistoryOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const AssistantHistoryState& state() const { return state_; }

    void add_message(HistoryMessage msg) {
        state_.messages.push_back(std::move(msg));
        if (state_.messages.size() > options_.max_messages) {
            state_.messages.erase(state_.messages.begin());
        }
        notify();
    }

    void set_session(std::string session_id) {
        state_.session_id = std::move(session_id);
        notify();
    }

    void clear() {
        state_.messages.clear();
        notify();
    }

    [[nodiscard]] std::vector<HistoryMessage> get_recent(std::size_t count) const {
        if (state_.messages.size() <= count) return state_.messages;
        return {state_.messages.end() - static_cast<std::ptrdiff_t>(count), state_.messages.end()};
    }

    void on_change(std::function<void(const AssistantHistoryState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    AssistantHistoryState state_;
    AssistantHistoryOptions options_;
    std::vector<std::function<void(const AssistantHistoryState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::assistant_history
