module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <algorithm>

export module cc.hooks.prompt_suggestion;

import cc.state.app_state;

export namespace cc::hooks::prompt_suggestion {

struct PromptSuggestion {
    std::string id;
    std::string text;
    std::string category;
    double relevance{0.0};
};

struct PromptSuggestionState {
    std::vector<PromptSuggestion> suggestions;
    std::optional<std::size_t> selected_index;
    bool visible{false};
    std::string context_hint;
};

struct PromptSuggestionOptions {
    std::size_t max_suggestions{5};
    bool context_aware{true};
};

class PromptSuggestionHook {
public:
    explicit PromptSuggestionHook(const PromptSuggestionOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const PromptSuggestionState& state() const { return state_; }

    void update_context(std::string hint) {
        state_.context_hint = std::move(hint);
        notify();
    }

    void set_suggestions(std::vector<PromptSuggestion> suggestions) {
        std::sort(suggestions.begin(), suggestions.end(),
            [](const auto& a, const auto& b) { return a.relevance > b.relevance; });
        if (suggestions.size() > options_.max_suggestions) {
            suggestions.resize(options_.max_suggestions);
        }
        state_.suggestions = std::move(suggestions);
        state_.visible = !state_.suggestions.empty();
        state_.selected_index = state_.visible ? std::optional<std::size_t>(0) : std::nullopt;
        notify();
    }

    void select_next() {
        if (state_.suggestions.empty()) return;
        state_.selected_index = (state_.selected_index.value_or(0) + 1) % state_.suggestions.size();
        notify();
    }

    void select_prev() {
        if (state_.suggestions.empty()) return;
        auto idx = state_.selected_index.value_or(0);
        state_.selected_index = (idx == 0) ? state_.suggestions.size() - 1 : idx - 1;
        notify();
    }

    [[nodiscard]] std::optional<PromptSuggestion> accept() {
        if (!state_.selected_index || *state_.selected_index >= state_.suggestions.size())
            return std::nullopt;
        auto result = state_.suggestions[*state_.selected_index];
        dismiss();
        return result;
    }

    void dismiss() {
        state_.visible = false;
        state_.suggestions.clear();
        state_.selected_index = std::nullopt;
        notify();
    }

    void on_change(std::function<void(const PromptSuggestionState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    PromptSuggestionState state_;
    PromptSuggestionOptions options_;
    std::vector<std::function<void(const PromptSuggestionState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::prompt_suggestion
