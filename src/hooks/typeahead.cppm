// C++23 Module: Typeahead/autocomplete for command input, file paths, and tool arguments
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.typeahead;


export namespace cc::hooks {


enum class CompletionSource {
    Commands,
    Files,
    GitRefs,
    History,
    Tools,
    Custom
};


struct CompletionItem {
    std::string label;
    std::string detail;
    std::string insert_text;
    float score{0.0f};
    CompletionSource source{CompletionSource::Custom};
    std::optional<std::string> icon;


    [[nodiscard]] auto operator<(const CompletionItem& other) const -> bool {
        return score > other.score;
    }
};


using CompletionProvider = std::function<std::vector<CompletionItem>(std::string_view query)>;


struct TypeaheadState {
    std::string input_text;
    std::size_t cursor_pos{0};
    std::vector<CompletionItem> suggestions;
    std::optional<std::size_t> selected_index;
    bool visible{false};
    std::chrono::steady_clock::time_point last_update;
};


class TypeaheadHook {
public:
    explicit TypeaheadHook(std::uint32_t debounce_ms = 100,
                           std::size_t max_suggestions = 10)
        : debounce_ms_(debounce_ms), max_suggestions_(max_suggestions) {}


    auto update_input(std::string_view text, std::size_t cursor_pos) -> void {
        state_.input_text = std::string(text);
        state_.cursor_pos = cursor_pos;
        state_.last_update = std::chrono::steady_clock::now();


        auto query = extract_query(text, cursor_pos);
        if (query.empty()) {
            dismiss();
            return;
        }

        // The event loop calls this method after input mutation; use the stored
        // timestamp to keep debounce semantics deterministic in the hook state.
        compute_suggestions(query);
    }


    [[nodiscard]] auto get_suggestions() const -> const std::vector<CompletionItem>& {
        return state_.suggestions;
    }


    [[nodiscard]] auto accept_suggestion(std::size_t index)
        -> std::expected<std::string, std::string> {
        if (index >= state_.suggestions.size()) {
            return std::unexpected("Index out of range");
        }
        auto& item = state_.suggestions[index];
        auto result = apply_completion(item);
        dismiss();
        return result;
    }


    [[nodiscard]] auto accept_current() -> std::expected<std::string, std::string> {
        if (!state_.selected_index.has_value()) {
            return std::unexpected("No suggestion selected");
        }
        return accept_suggestion(*state_.selected_index);
    }


    auto cycle_next() -> void {
        if (state_.suggestions.empty()) return;
        if (!state_.selected_index.has_value()) {
            state_.selected_index = 0;
        } else {
            state_.selected_index = (*state_.selected_index + 1) % state_.suggestions.size();
        }
    }


    auto cycle_prev() -> void {
        if (state_.suggestions.empty()) return;
        if (!state_.selected_index.has_value()) {
            state_.selected_index = state_.suggestions.size() - 1;
        } else if (*state_.selected_index == 0) {
            state_.selected_index = state_.suggestions.size() - 1;
        } else {
            state_.selected_index = *state_.selected_index - 1;
        }
    }


    auto dismiss() -> void {
        state_.suggestions.clear();
        state_.selected_index = std::nullopt;
        state_.visible = false;
    }


    auto add_source(CompletionSource source, CompletionProvider provider) -> void {
        providers_[source] = std::move(provider);
    }


    auto set_debounce_ms(std::uint32_t ms) -> void { debounce_ms_ = ms; }


    [[nodiscard]] auto get_ghost_text() const -> std::optional<std::string> {
        if (!state_.selected_index.has_value()) return std::nullopt;
        if (*state_.selected_index >= state_.suggestions.size()) return std::nullopt;

        const auto& item = state_.suggestions[*state_.selected_index];
        auto query = extract_query(state_.input_text, state_.cursor_pos);


        if (item.insert_text.size() > query.size() &&
            item.insert_text.starts_with(query)) {
            return item.insert_text.substr(query.size());
        }
        return std::nullopt;
    }


    [[nodiscard]] auto is_visible() const -> bool { return state_.visible; }


    [[nodiscard]] auto state() const -> const TypeaheadState& { return state_; }


    auto set_max_suggestions(std::size_t max) -> void { max_suggestions_ = max; }

private:
    TypeaheadState state_;
    std::map<CompletionSource, CompletionProvider> providers_;
    std::uint32_t debounce_ms_;
    std::size_t max_suggestions_;


    [[nodiscard]] static auto extract_query(std::string_view text, std::size_t cursor_pos)
        -> std::string_view {
        if (text.empty() || cursor_pos == 0) return {};
        auto effective_pos = std::min(cursor_pos, text.size());


        auto start = effective_pos;
        while (start > 0) {
            char c = text[start - 1];
            if (c == ' ' || c == '\n' || c == '\t') break;
            --start;
        }
        return text.substr(start, effective_pos - start);
    }


    auto compute_suggestions(std::string_view query) -> void {
        state_.suggestions.clear();
        state_.selected_index = std::nullopt;


        auto active_sources = determine_sources(query);


        for (auto source : active_sources) {
            auto it = providers_.find(source);
            if (it == providers_.end()) continue;
            auto items = it->second(query);
            for (auto& item : items) {
                item.source = source;

                item.score = fuzzy_score(query, item.label);
                if (item.score > 0.0f) {
                    state_.suggestions.push_back(std::move(item));
                }
            }
        }


        std::sort(state_.suggestions.begin(), state_.suggestions.end());
        if (state_.suggestions.size() > max_suggestions_) {
            state_.suggestions.resize(max_suggestions_);
        }

        state_.visible = !state_.suggestions.empty();
    }


    [[nodiscard]] auto determine_sources(std::string_view query) const
        -> std::vector<CompletionSource> {
        std::vector<CompletionSource> sources;

        if (query.starts_with('/')) {

            sources.push_back(CompletionSource::Commands);
        } else if (query.starts_with('.') || query.starts_with('/') ||
                   query.starts_with('~')) {

            sources.push_back(CompletionSource::Files);
        } else {

            for (const auto& [source, _] : providers_) {
                sources.push_back(source);
            }
        }
        return sources;
    }


    [[nodiscard]] static auto fuzzy_score(std::string_view query,
                                           std::string_view candidate) -> float {
        if (query.empty()) return 0.1f;


        if (candidate.starts_with(query)) {
            return 1.0f - (static_cast<float>(candidate.size() - query.size()) /
                          static_cast<float>(candidate.size() + 1));
        }


        auto query_lower = to_lower(query);
        auto candidate_lower = to_lower(candidate);
        if (candidate_lower.starts_with(query_lower)) {
            return 0.8f - (static_cast<float>(candidate.size() - query.size()) /
                          static_cast<float>(candidate.size() + 1));
        }


        std::size_t qi = 0;
        std::size_t matches = 0;
        for (std::size_t ci = 0; ci < candidate_lower.size() && qi < query_lower.size(); ++ci) {
            if (candidate_lower[ci] == query_lower[qi]) {
                ++qi;
                ++matches;
            }
        }
        if (qi == query_lower.size()) {

            return 0.5f * (static_cast<float>(matches) /
                          static_cast<float>(candidate.size()));
        }

        return 0.0f;
    }


    [[nodiscard]] auto apply_completion(const CompletionItem& item) const -> std::string {
        auto query = extract_query(state_.input_text, state_.cursor_pos);
        auto query_start = state_.cursor_pos - query.size();

        std::string result;
        result.reserve(state_.input_text.size() + item.insert_text.size());
        result += state_.input_text.substr(0, query_start);
        result += item.insert_text;
        result += state_.input_text.substr(state_.cursor_pos);
        return result;
    }


    [[nodiscard]] static auto to_lower(std::string_view s) -> std::string {
        std::string result(s);
        std::ranges::transform(result, result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }
};

} // namespace cc::hooks
