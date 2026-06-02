/// @file agent_model_selector.cppm
/// @brief Model selection UI for agent configuration
module;
#include <string>
#include <vector>
#include <functional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.agents.agent_model_selector;
export namespace cc::ui::agents {
using namespace ftxui;
struct ModelOption { std::string id; std::string display_name; bool is_recommended{false}; };
[[nodiscard]] inline Element render_model_selector(const std::vector<ModelOption>& models, std::size_t selected) {
    std::vector<Element> elements;
    elements.push_back(text("Select Model") | bold);
    elements.push_back(separator());
    for (std::size_t i = 0; i < models.size(); ++i) {
        auto& m = models[i];
        auto style = i == selected ? inverted : nothing;
        auto suffix = m.is_recommended ? " (recommended)" : "";
        elements.push_back(text(m.display_name + suffix) | style);
    }
    return vbox(elements) | border;
}
} // namespace
