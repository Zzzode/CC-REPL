module;
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.ui.permission_rules;

export namespace cc::ui::permissions {

// --- Rule scope levels ---
enum class RuleScope {
    Global,
    Project,
    Session,
    OneTime
};

// --- Rule actions ---
enum class RuleAction {
    Allow,
    Deny,
    Ask
};

// --- Individual permission rule ---
struct PermissionRule {
    std::string id;
    std::string pattern;
    RuleAction action;
    RuleScope scope;
    std::optional<std::string> tool_name;
    bool is_default{false};
    std::string description;
};

// --- Group of related rules for display ---
struct RuleGroup {
    std::string category;
    std::vector<PermissionRule> rules;
    bool expanded{false};
};

// --- Label / icon accessors ---

// Get human-readable scope label
[[nodiscard]] inline auto get_scope_label(RuleScope scope) -> std::string_view {
    switch (scope) {
        case RuleScope::Global:  return "Global";
        case RuleScope::Project: return "Project";
        case RuleScope::Session: return "Session";
        case RuleScope::OneTime: return "One-time";
    }
    return "Unknown";
}

// Get human-readable action label
[[nodiscard]] inline auto get_action_label(RuleAction action) -> std::string_view {
    switch (action) {
        case RuleAction::Allow: return "Allow";
        case RuleAction::Deny:  return "Deny";
        case RuleAction::Ask:   return "Ask";
    }
    return "Unknown";
}

// Get icon/symbol for the action
[[nodiscard]] inline auto get_action_icon(RuleAction action) -> std::string_view {
    switch (action) {
        case RuleAction::Allow: return "\xe2\x9c\x93"; // checkmark
        case RuleAction::Deny:  return "\xe2\x9c\x97"; // cross
        case RuleAction::Ask:   return "?";
    }
    return " ";
}

// --- Formatting functions ---

// Format a single rule for terminal display
[[nodiscard]] inline auto format_rule_display(const PermissionRule& rule) -> std::string {
    std::string result;

    // Action icon and label
    result += std::string(get_action_icon(rule.action)) + " ";
    result += "\033[1m" + rule.pattern + "\033[0m";

    // Scope badge
    result += " \033[2m[" + std::string(get_scope_label(rule.scope)) + "]\033[0m";

    // Tool name constraint
    if (rule.tool_name.has_value()) {
        result += " \033[36m(" + *rule.tool_name + ")\033[0m";
    }

    // Default indicator
    if (rule.is_default) {
        result += " \033[2m(default)\033[0m";
    }

    // Description on next line if present
    if (!rule.description.empty()) {
        result += "\n    \033[2m" + rule.description + "\033[0m";
    }

    return result;
}

// Format a list of rule groups for terminal display
[[nodiscard]] inline auto format_rule_list(const std::vector<RuleGroup>& groups) -> std::string {
    if (groups.empty()) {
        return "\033[2mNo permission rules configured.\033[0m\n";
    }
    std::string result;
    for (const auto& group : groups) {
        // Category header
        result += "\033[1;4m" + group.category + "\033[0m";
        result += " \033[2m(" + std::to_string(group.rules.size()) + " rules)\033[0m\n";

        if (group.expanded) {
            for (const auto& rule : group.rules) {
                result += "  " + format_rule_display(rule) + "\n";
            }
        } else {
            result += "  \033[2m(collapsed)\033[0m\n";
        }
        result += "\n";
    }
    return result;
}

// --- Rule manipulation functions ---

// Sort rules by priority: Deny > Ask > Allow, then narrower scope first
inline auto sort_rules_by_priority(std::vector<PermissionRule>& rules) -> void {
    auto priority = [](const PermissionRule& r) -> int {
        int action_weight = 0;
        switch (r.action) {
            case RuleAction::Deny:  action_weight = 0; break;
            case RuleAction::Ask:   action_weight = 1; break;
            case RuleAction::Allow: action_weight = 2; break;
        }
        int scope_weight = 0;
        switch (r.scope) {
            case RuleScope::OneTime: scope_weight = 0; break;
            case RuleScope::Session: scope_weight = 1; break;
            case RuleScope::Project: scope_weight = 2; break;
            case RuleScope::Global:  scope_weight = 3; break;
        }
        return action_weight * 10 + scope_weight;
    };

    std::ranges::sort(rules, [&](const PermissionRule& a, const PermissionRule& b) {
        return priority(a) < priority(b);
    });
}

// Filter rules by tool name
[[nodiscard]] inline auto filter_rules_by_tool(const std::vector<PermissionRule>& rules,
                                                std::string_view tool) -> std::vector<PermissionRule> {
    std::vector<PermissionRule> result;
    for (const auto& rule : rules) {
        if (!rule.tool_name.has_value() || *rule.tool_name == tool) {
            result.push_back(rule);
        }
    }
    return result;
}

} // namespace cc::ui::permissions
