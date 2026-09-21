/// @file history_search_dialog.cppm
/// @brief Session history search dialog
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.history_search_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
struct HistoryEntry { std::string session_id; std::string first_message; std::string timestamp; std::optional<std::string> project_name; };
[[nodiscard]] inline Element render_history_search(const std::vector<HistoryEntry>& entries, std::size_t selected) {
    std::vector<Element> elements;
    elements.push_back(text("Session History") | bold);
    elements.push_back(separator());
    for (std::size_t i = 0; i < entries.size() && i < 15; ++i) {
        auto style = i == selected ? inverted : nothing;
        auto& e = entries[i];
        elements.push_back(hbox({text(e.timestamp) | dim, text(" "), text(e.first_message)}) | style);
    }
    return vbox(elements) | border;
}
} // namespace
