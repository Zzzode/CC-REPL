module;
#include <string>
#include <functional>
#include <optional>
#include <vector>

export module cc.hooks.arrow_key_history;

import cc.state.app_state;

export namespace cc::hooks::arrow_key_history {

struct ArrowKeyHistoryState {
    std::vector<std::string> entries;
    std::optional<std::size_t> current_index;
    std::string draft_input;
};

struct ArrowKeyHistoryOptions {
    std::size_t max_entries{100};
    bool persist{true};
};

class ArrowKeyHistoryHook {
public:
    explicit ArrowKeyHistoryHook(const ArrowKeyHistoryOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const ArrowKeyHistoryState& state() const { return state_; }

    void add_entry(std::string entry) {
        if (entry.empty()) return;
        // Remove duplicate if exists at end
        if (!state_.entries.empty() && state_.entries.back() == entry) return;
        state_.entries.push_back(std::move(entry));
        if (state_.entries.size() > options_.max_entries) {
            state_.entries.erase(state_.entries.begin());
        }
        state_.current_index = std::nullopt;
        notify();
    }

    /// Navigate up (older). Returns the history entry or nullopt.
    [[nodiscard]] std::optional<std::string> go_up(std::string_view current_input) {
        if (state_.entries.empty()) return std::nullopt;
        if (!state_.current_index.has_value()) {
            state_.draft_input = std::string(current_input);
            state_.current_index = state_.entries.size() - 1;
        } else if (*state_.current_index > 0) {
            --(*state_.current_index);
        }
        notify();
        return state_.entries[*state_.current_index];
    }

    /// Navigate down (newer). Returns the history entry or the draft input.
    [[nodiscard]] std::optional<std::string> go_down() {
        if (!state_.current_index.has_value()) return std::nullopt;
        if (*state_.current_index < state_.entries.size() - 1) {
            ++(*state_.current_index);
            notify();
            return state_.entries[*state_.current_index];
        }
        // At the end, return to draft
        state_.current_index = std::nullopt;
        notify();
        return state_.draft_input;
    }

    void reset_navigation() {
        state_.current_index = std::nullopt;
        notify();
    }

    void on_change(std::function<void(const ArrowKeyHistoryState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    ArrowKeyHistoryState state_;
    ArrowKeyHistoryOptions options_;
    std::vector<std::function<void(const ArrowKeyHistoryState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::arrow_key_history
