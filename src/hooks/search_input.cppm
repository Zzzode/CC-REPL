// C++23 Module: Incremental search within conversation/output with regex support
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.search_input;


export namespace cc::hooks {


enum class SearchDirection { forward, backward };


struct SearchMatch {
    std::size_t line_index{0};
    std::size_t col_start{0};
    std::size_t col_end{0};
    std::string context;


    [[nodiscard]] auto length() const -> std::size_t { return col_end - col_start; }


    [[nodiscard]] auto matched_text() const -> std::string_view {
        if (col_start >= context.size()) return {};
        return std::string_view(context).substr(col_start, col_end - col_start);
    }
};


struct SearchState {
    std::string query;
    SearchDirection direction{SearchDirection::forward};
    std::vector<SearchMatch> matches;
    std::size_t current_match_idx{0};
    bool is_regex{false};
    bool case_sensitive{false};
    bool active{false};
};


class SearchInputHook {
public:
    SearchInputHook() = default;


    auto start_search(SearchDirection direction = SearchDirection::forward) -> void {
        state_.active = true;
        state_.direction = direction;
        state_.query.clear();
        state_.matches.clear();
        state_.current_match_idx = 0;
    }


    auto update_query(std::string_view text) -> void {
        if (!state_.active) return;
        state_.query = std::string(text);
        execute_search();
    }


    auto next_match() -> void {
        if (state_.matches.empty()) return;
        state_.current_match_idx = (state_.current_match_idx + 1) % state_.matches.size();
    }


    auto prev_match() -> void {
        if (state_.matches.empty()) return;
        if (state_.current_match_idx == 0) {
            state_.current_match_idx = state_.matches.size() - 1;
        } else {
            --state_.current_match_idx;
        }
    }


    auto accept_match() -> std::optional<SearchMatch> {
        if (!state_.active || state_.matches.empty()) {
            cancel();
            return std::nullopt;
        }
        auto match = state_.matches[state_.current_match_idx];

        if (!state_.query.empty()) {
            add_to_history(state_.query);
        }
        state_.active = false;
        return match;
    }


    auto cancel() -> void {
        state_.active = false;
        state_.query.clear();
        state_.matches.clear();
        state_.current_match_idx = 0;
    }


    [[nodiscard]] auto get_matches() const -> std::span<const SearchMatch> {
        return state_.matches;
    }


    [[nodiscard]] auto get_current_match() const -> std::optional<SearchMatch> {
        if (state_.matches.empty()) return std::nullopt;
        if (state_.current_match_idx >= state_.matches.size()) return std::nullopt;
        return state_.matches[state_.current_match_idx];
    }


    auto toggle_case_sensitivity() -> void {
        state_.case_sensitive = !state_.case_sensitive;
        if (state_.active && !state_.query.empty()) {
            execute_search();
        }
    }


    auto toggle_regex() -> void {
        state_.is_regex = !state_.is_regex;
        if (state_.active && !state_.query.empty()) {
            execute_search();
        }
    }


    auto set_content(std::vector<std::string> lines) -> void {
        content_lines_ = std::move(lines);
    }


    auto append_content(std::string_view line) -> void {
        content_lines_.emplace_back(line);
    }


    [[nodiscard]] auto state() const -> const SearchState& { return state_; }


    [[nodiscard]] auto history() const -> std::span<const std::string> { return history_; }


    auto prev_history() -> void {
        if (history_.empty()) return;
        if (history_idx_ == 0) return;
        --history_idx_;
        state_.query = history_[history_idx_];
        execute_search();
    }


    auto next_history() -> void {
        if (history_.empty()) return;
        if (history_idx_ >= history_.size() - 1) {
            state_.query.clear();
            return;
        }
        ++history_idx_;
        state_.query = history_[history_idx_];
        execute_search();
    }


    [[nodiscard]] auto match_count_display() const -> std::string {
        if (state_.matches.empty()) return "0/0";
        return std::to_string(state_.current_match_idx + 1) + "/" +
               std::to_string(state_.matches.size());
    }

private:
    SearchState state_;
    std::vector<std::string> content_lines_;
    std::vector<std::string> history_;
    std::size_t history_idx_{0};
    static constexpr std::size_t max_history_size_ = 50;


    auto execute_search() -> void {
        state_.matches.clear();
        state_.current_match_idx = 0;

        if (state_.query.empty()) return;

        if (state_.is_regex) {
            search_regex();
        } else {
            search_literal();
        }


        if (state_.direction == SearchDirection::backward && !state_.matches.empty()) {
            std::reverse(state_.matches.begin(), state_.matches.end());
        }
    }


    auto search_literal() -> void {
        auto query = state_.query;

        auto prepare = [this](std::string_view s) -> std::string {
            if (!state_.case_sensitive) {
                std::string lower(s);
                std::ranges::transform(lower, lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                return lower;
            }
            return std::string(s);
        };

        auto prepared_query = prepare(query);

        for (std::size_t i = 0; i < content_lines_.size(); ++i) {
            auto prepared_line = prepare(content_lines_[i]);
            std::size_t pos = 0;
            while ((pos = prepared_line.find(prepared_query, pos)) != std::string::npos) {
                state_.matches.push_back(SearchMatch{
                    .line_index = i,
                    .col_start = pos,
                    .col_end = pos + prepared_query.size(),
                    .context = content_lines_[i]
                });
                pos += prepared_query.size();
            }
        }
    }


    auto search_regex() -> void {
        try {
            auto flags = std::regex::ECMAScript;
            if (!state_.case_sensitive) flags |= std::regex::icase;
            std::regex pattern(state_.query, flags);

            for (std::size_t i = 0; i < content_lines_.size(); ++i) {
                auto begin = std::sregex_iterator(
                    content_lines_[i].begin(), content_lines_[i].end(), pattern);
                auto end = std::sregex_iterator();

                for (auto it = begin; it != end; ++it) {
                    state_.matches.push_back(SearchMatch{
                        .line_index = i,
                        .col_start = static_cast<std::size_t>(it->position()),
                        .col_end = static_cast<std::size_t>(it->position() + it->length()),
                        .context = content_lines_[i]
                    });
                }
            }
        } catch (const std::regex_error&) {

        }
    }


    auto add_to_history(const std::string& query) -> void {

        std::erase(history_, query);
        history_.push_back(query);

        if (history_.size() > max_history_size_) {
            history_.erase(history_.begin());
        }
        history_idx_ = history_.size();
    }
};

} // namespace cc::hooks
