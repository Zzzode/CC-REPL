/// @file auto_updater.cppm
/// @brief Auto-update notification UI component
module;
#include <string>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.components.auto_updater;
export namespace cc::ui::components {
using namespace ftxui;
struct UpdateInfo { std::string current_version; std::string new_version; bool is_downloading{false}; std::optional<uint32_t> progress_pct; };
[[nodiscard]] inline Element render_update_notification(const UpdateInfo& info) {
    std::vector<Element> elements;
    elements.push_back(text("Update available: v" + info.new_version) | bold | color(Color::Cyan));
    elements.push_back(text("  Current: v" + info.current_version) | dim);
    if (info.is_downloading && info.progress_pct) elements.push_back(text(std::format("  Downloading: {}%", *info.progress_pct)));
    return vbox(elements);
}
} // namespace
