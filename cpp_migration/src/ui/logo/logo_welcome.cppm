/// @file logo_welcome.cppm
/// @brief Welcome screen with version and tips
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
export module cc.ui.logo.logo_welcome;
export namespace cc::ui::logo {
using namespace ftxui;
struct WelcomeInfo { std::string version; std::string model; std::vector<std::string> tips; };
[[nodiscard]] inline Element render_welcome(const WelcomeInfo& info) {
    std::vector<Element> elements;
    elements.push_back(text("Claude Code v" + info.version) | bold);
    elements.push_back(text("Model: " + info.model) | dim);
    if (!info.tips.empty()) {
        elements.push_back(separator());
        elements.push_back(text("Tips:") | dim);
        for (const auto& tip : info.tips) elements.push_back(text("  " + tip) | dim);
    }
    return vbox(elements);
}
} // namespace
