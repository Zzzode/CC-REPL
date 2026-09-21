/// @file spinner_teammate_tree.cppm
/// @brief Tree view for teammate/agent status spinners
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
export module cc.ui.components.spinner_teammate_tree;
export namespace cc::ui::components {
using namespace ftxui;
struct TeammateNode { std::string name; std::string status; std::optional<std::string> color; std::vector<TeammateNode> children; };
[[nodiscard]] inline Element render_teammate_tree(const std::vector<TeammateNode>& nodes, std::string prefix = "") {
    std::vector<Element> elements;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        auto connector = (i == nodes.size() - 1) ? "└─ " : "├─ ";
        elements.push_back(text(prefix + connector + node.name + " [" + node.status + "]"));
        if (!node.children.empty()) {
            auto child_prefix = prefix + ((i == nodes.size() - 1) ? "   " : "│  ");
            elements.push_back(render_teammate_tree(node.children, child_prefix));
        }
    }
    return vbox(elements);
}
} // namespace
