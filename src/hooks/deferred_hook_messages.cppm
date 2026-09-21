module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <memory>

export module cc.hooks.deferred_hook_messages;

import cc.state.app_state;

export namespace cc::hooks::deferred_hook_messages {

struct DeferredMessage {
    std::string id;
    std::string source;
    std::string content;
    int priority{0};
};

struct DeferredHookMessagesState {
    std::vector<DeferredMessage> pending;
    std::vector<DeferredMessage> processed;
    bool processing{false};
};

struct DeferredHookMessagesOptions {
    std::size_t batch_size{10};
    bool auto_process{true};
};

class DeferredHookMessagesHook {
public:
    explicit DeferredHookMessagesHook(const DeferredHookMessagesOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const DeferredHookMessagesState& state() const { return state_; }

    void enqueue(DeferredMessage msg) {
        state_.pending.push_back(std::move(msg));
        notify();
    }

    /// Process pending messages (call during safe render phase)
    std::vector<DeferredMessage> flush() {
        state_.processing = true;
        auto batch_size = std::min(options_.batch_size, state_.pending.size());
        std::vector<DeferredMessage> batch(
            state_.pending.begin(),
            state_.pending.begin() + static_cast<std::ptrdiff_t>(batch_size));
        state_.pending.erase(
            state_.pending.begin(),
            state_.pending.begin() + static_cast<std::ptrdiff_t>(batch_size));
        for (const auto& msg : batch) {
            state_.processed.push_back(msg);
        }
        state_.processing = false;
        notify();
        return batch;
    }

    [[nodiscard]] bool has_pending() const { return !state_.pending.empty(); }
    [[nodiscard]] std::size_t pending_count() const { return state_.pending.size(); }

    void clear() {
        state_.pending.clear();
        state_.processed.clear();
        notify();
    }

    void on_change(std::function<void(const DeferredHookMessagesState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    DeferredHookMessagesState state_;
    DeferredHookMessagesOptions options_;
    std::vector<std::function<void(const DeferredHookMessagesState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::deferred_hook_messages
