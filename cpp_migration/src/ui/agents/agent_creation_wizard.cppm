/// @file agent_creation_wizard.cppm
/// @brief Multi-step wizard for creating new agents
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.agents.agent_creation_wizard;
export namespace cc::ui::agents {
using namespace ftxui;
enum class WizardStep { Name, Model, Tools, Permissions, Review };
struct WizardState {
    WizardStep current_step{WizardStep::Name};
    std::string name;
    std::string model;
    std::vector<std::string> selected_tools;
    std::vector<std::string> selected_permissions;
};
[[nodiscard]] inline Element render_wizard_progress(WizardStep current) {
    auto step_name = [](WizardStep s) -> std::string {
        switch (s) { case WizardStep::Name: return "Name"; case WizardStep::Model: return "Model"; case WizardStep::Tools: return "Tools"; case WizardStep::Permissions: return "Perms"; case WizardStep::Review: return "Review"; }
        return "";
    };
    std::vector<Element> elements;
    for (auto s : {WizardStep::Name, WizardStep::Model, WizardStep::Tools, WizardStep::Permissions, WizardStep::Review}) {
        auto style = s == current ? bold : dim;
        elements.push_back(text(step_name(s)) | style);
        if (s != WizardStep::Review) elements.push_back(text(" > ") | dim);
    }
    return hbox(elements);
}
} // namespace
