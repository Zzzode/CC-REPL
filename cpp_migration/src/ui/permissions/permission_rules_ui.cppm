/// @file permission_rules_ui.cppm
/// @brief UI for displaying and managing permission rules
module;

#include <string>
#include <vector>
#include <format>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_rules_ui;

import cc.types.types;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Display format for a permission rule
struct RuleDisplayEntry {
    std::string pattern;
    std::string scope;       // "tool", "path", "command"
    std::string action;      // "allow", "deny"
    int priority{0};
    bool is_active{true};
};

/// Render a single rule entry
[[nodiscard]] inline Element render_rule_entry(const RuleDisplayEntry& entry) {
    auto action_color = entry.action == "allow" ? Color::Green : Color::Red;
    return hbox({
        text(entry.action) | bold | color(action_color),
        text(" "),
        text(entry.scope) | dim,
        text(": "),
        text(entry.pattern),
        text(std::format(" (pri={})", entry.priority)) | dim,
    });
}

/// Render the full rules list
[[nodiscard]] inline Element render_rules_list(const std::vector<RuleDisplayEntry>& rules) {
    if (rules.empty()) return text("No permission rules configured") | dim;
    std::vector<Element> elements;
    for (const auto& rule : rules) {
        elements.push_back(render_rule_entry(rule));
    }
    return vbox(elements);
}

/// Create rules management component
[[nodiscard]] inline Component rules_list_component(
    const std::vector<RuleDisplayEntry>& rules) {
    return Renderer([rules] {
        return vbox({
            text("Permission Rules") | bold,
            separator(),
            render_rules_list(rules),
        }) | border;
    });
}

} // namespace cc::ui::permissions
