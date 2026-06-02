module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <algorithm>

export module cc.hooks.unified_suggestions;

import cc.state.app_state;

export namespace cc::hooks::unified_suggestions {

enum class SuggestionSource { file, command, history, skill, snippet };

struct Suggestion {
    std::string id;
    std::string label;
    std::string description;
    std::string insert_text;
    SuggestionSource source{SuggestionSource::command};
    double score{0.0};
};

struct UnifiedSuggestionsState {
    std::vector<Suggestion> items;
    std::optional<std::size_t> selected_index;
    std::string query;
    bool visible{false};
};

struct UnifiedSuggestionsOptions {
    std::size_t max_results{15};
    bool deduplicate{true};
    std::vector<SuggestionSource> enabled_sources;
};

class UnifiedSuggestionsHook {
public:
    explicit UnifiedSuggestionsHook(const UnifiedSuggestionsOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; state_.visible = false; }

    [[nodiscard]] const UnifiedSuggestionsState& state() const { return state_; }

    void update_query(std::string query) {
        state_.query = std::move(query);
        state_.selected_index = state_.items.empty() ? std::nullopt : std::optional<std::size_t>(0);
        notify();
    }

    void set_items(std::vector<Suggestion> items) {
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            return a.score > b.score;
        });
        if (items.size() > options_.max_results) {
            items.resize(options_.max_results);
        }
        state_.items = std::move(items);
        state_.visible = !state_.items.empty();
        state_.selected_index = state_.visible ? std::optional<std::size_t>(0) : std::nullopt;
        notify();
    }

    void select_next() {
        if (state_.items.empty()) return;
        state_.selected_index = (state_.selected_index.value_or(0) + 1) % state_.items.size();
        notify();
    }

    void select_prev() {
        if (state_.items.empty()) return;
        auto idx = state_.selected_index.value_or(0);
        state_.selected_index = (idx == 0) ? state_.items.size() - 1 : idx - 1;
        notify();
    }

    void dismiss() {
        state_.visible = false;
        state_.items.clear();
        state_.selected_index = std::nullopt;
        notify();
    }

    [[nodiscard]] std::optional<Suggestion> accept() {
        if (!state_.selected_index || *state_.selected_index >= state_.items.size()) return std::nullopt;
        auto result = state_.items[*state_.selected_index];
        dismiss();
        return result;
    }

    void on_change(std::function<void(const UnifiedSuggestionsState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    UnifiedSuggestionsState state_;
    UnifiedSuggestionsOptions options_;
    std::vector<std::function<void(const UnifiedSuggestionsState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::unified_suggestions
