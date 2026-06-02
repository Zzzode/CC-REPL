/// @file logo_feed.cppm
/// @brief Logo area feed messages (notifications during idle)
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
export module cc.ui.logo.logo_feed;
export namespace cc::ui::logo {
using namespace ftxui;
struct FeedEntry { std::string message; bool is_new{false}; };
[[nodiscard]] inline Element render_feed(const std::vector<FeedEntry>& entries) {
    if (entries.empty()) return text("");
    std::vector<Element> elements;
    for (const auto& entry : entries) {
        auto style = entry.is_new ? bold : dim;
        elements.push_back(text(entry.message) | style);
    }
    return vbox(elements);
}
} // namespace
