/// @file auto_mode_dialog.cppm
/// @brief Auto-mode configuration dialog
module;
#include <string>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.auto_mode_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
struct AutoModeConfig { bool enabled{false}; std::optional<uint32_t> max_turns; bool allow_file_edits{true}; bool allow_bash{false}; };
[[nodiscard]] inline Element render_auto_mode_config(const AutoModeConfig& config) {
    std::vector<Element> elements;
    elements.push_back(text("Auto Mode Configuration") | bold);
    elements.push_back(separator());
    elements.push_back(text(std::string("  Enabled: ") + (config.enabled ? "yes" : "no")));
    if (config.max_turns) elements.push_back(text(std::format("  Max turns: {}", *config.max_turns)));
    elements.push_back(text(std::string("  File edits: ") + (config.allow_file_edits ? "allowed" : "denied")));
    elements.push_back(text(std::string("  Bash: ") + (config.allow_bash ? "allowed" : "denied")));
    return vbox(elements) | border;
}
} // namespace
