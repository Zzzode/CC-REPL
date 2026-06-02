module;
#include <string>
#include <functional>
#include <optional>
#include <vector>

export module cc.hooks.claude_code_hint_recommendation;

import cc.state.app_state;

export namespace cc::hooks::claude_code_hint_recommendation {

struct CodeHint {
    std::string id;
    std::string title;
    std::string description;
    std::string action_label;
    double relevance{0.0};
    bool dismissed{false};
};

struct ClaudeCodeHintRecommendationState {
    std::vector<CodeHint> hints;
    std::optional<std::string> active_hint_id;
    bool enabled{true};
};

struct ClaudeCodeHintRecommendationOptions {
    std::size_t max_hints{3};
    bool show_on_startup{true};
};

class ClaudeCodeHintRecommendationHook {
public:
    explicit ClaudeCodeHintRecommendationHook(const ClaudeCodeHintRecommendationOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const ClaudeCodeHintRecommendationState& state() const { return state_; }

    void add_hint(CodeHint hint) {
        if (state_.hints.size() >= options_.max_hints) return;
        state_.hints.push_back(std::move(hint));
        notify();
    }

    void dismiss_hint(std::string_view id) {
        for (auto& h : state_.hints) {
            if (h.id == id) { h.dismissed = true; break; }
        }
        notify();
    }

    void set_active(std::string_view id) {
        state_.active_hint_id = std::string(id);
        notify();
    }

    void clear_all() {
        state_.hints.clear();
        state_.active_hint_id = std::nullopt;
        notify();
    }

    void set_enabled(bool enabled) {
        state_.enabled = enabled;
        notify();
    }

    void on_change(std::function<void(const ClaudeCodeHintRecommendationState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    ClaudeCodeHintRecommendationState state_;
    ClaudeCodeHintRecommendationOptions options_;
    std::vector<std::function<void(const ClaudeCodeHintRecommendationState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::claude_code_hint_recommendation
