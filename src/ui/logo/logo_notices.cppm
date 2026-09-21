/// @file logo_notices.cppm
/// @brief Important notices display in logo area
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
export module cc.ui.logo.logo_notices;
export namespace cc::ui::logo {
using namespace ftxui;
enum class NoticeLevel { Info, Warning, Critical };
struct Notice { std::string message; NoticeLevel level{NoticeLevel::Info}; };
[[nodiscard]] inline Element render_notices(const std::vector<Notice>& notices) {
    if (notices.empty()) return text("");
    std::vector<Element> elements;
    for (const auto& n : notices) {
        auto c = n.level == NoticeLevel::Critical ? Color::Red : n.level == NoticeLevel::Warning ? Color::Yellow : Color::Cyan;
        elements.push_back(text(n.message) | color(c));
    }
    return vbox(elements);
}
} // namespace
