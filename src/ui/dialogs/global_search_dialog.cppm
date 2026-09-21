/// @file global_search_dialog.cppm
/// @brief Global search dialog (files, commands, sessions)
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.global_search_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
enum class SearchCategory { Files, Commands, Sessions, All };
struct SearchResult { std::string title; std::string subtitle; SearchCategory category{SearchCategory::All}; };
[[nodiscard]] inline Element render_search_results(const std::vector<SearchResult>& results, std::size_t selected) {
    std::vector<Element> elements;
    for (std::size_t i = 0; i < results.size() && i < 20; ++i) {
        auto style = i == selected ? inverted : nothing;
        elements.push_back(hbox({text(results[i].title) | bold, text(" " + results[i].subtitle) | dim}) | style);
    }
    return vbox(elements);
}
} // namespace
