/// @file prompt_history_search.cppm
/// @brief History search overlay for prompt input
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.prompt.prompt_history_search;
export namespace cc::ui::prompt {
using namespace ftxui;
struct HistorySearchState {
    std::string query;
    std::vector<std::string> matches;
    std::size_t selected_index{0};
    bool is_active{false};
};
[[nodiscard]] inline Element render_history_search(const HistorySearchState& state) {
    if (!state.is_active) return text("");
    std::vector<Element> elements;
    elements.push_back(hbox({text("search: ") | dim, text(state.query) | bold}));
    for (std::size_t i = 0; i < state.matches.size() && i < 10; ++i) {
        auto style = i == state.selected_index ? inverted : nothing;
        elements.push_back(text("  " + state.matches[i]) | style);
    }
    return vbox(elements) | border;
}
} // namespace
