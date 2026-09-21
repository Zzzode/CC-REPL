/// @file mcp_elicitation.cppm
/// @brief MCP elicitation request UI (server asking for user input)
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.mcp.mcp_elicitation;
export namespace cc::ui::mcp {
using namespace ftxui;
struct ElicitationField { std::string name; std::string description; std::string type; bool required{false}; std::optional<std::string> default_value; };
struct ElicitationRequest { std::string server_name; std::string message; std::vector<ElicitationField> fields; };
[[nodiscard]] inline Element render_elicitation(const ElicitationRequest& req) {
    std::vector<Element> elements;
    elements.push_back(hbox({text(req.server_name) | bold, text(" requests input:") | dim}));
    elements.push_back(text(req.message));
    elements.push_back(separator());
    for (const auto& f : req.fields) {
        auto label = f.required ? f.name + " *" : f.name;
        elements.push_back(text(label + " (" + f.type + ")") | dim);
    }
    return vbox(elements) | border;
}
} // namespace
