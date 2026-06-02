/// @file cost_threshold_dialog.cppm
/// @brief Cost threshold warning dialog
module;
#include <string>
#include <format>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.cost_threshold_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
struct CostThresholdInfo { double current_cost{0}; double threshold{0}; std::string model; };
[[nodiscard]] inline Element render_cost_threshold(const CostThresholdInfo& info) {
    return vbox({
        text("Cost Threshold Reached") | bold | color(Color::Yellow),
        separator(),
        text(std::format("  Current: ${:.2f}", info.current_cost)),
        text(std::format("  Threshold: ${:.2f}", info.threshold)),
        text("  Model: " + info.model) | dim,
        separator(),
        text("Continue? [Y]es / [N]o") | bold,
    }) | border;
}
} // namespace
