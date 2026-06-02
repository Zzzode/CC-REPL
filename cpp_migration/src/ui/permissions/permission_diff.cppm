module;
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.ui.permission_diff;

export namespace cc::ui::permissions {

// --- Individual permission change entry ---
struct PermissionChange {
    std::string tool_name;
    std::string old_value;
    std::string new_value;
    bool is_new;
    bool is_removed;
};

// --- Aggregated diff of permission changes ---
struct PermissionDiff {
    std::vector<PermissionChange> changes;
    std::size_t additions;
    std::size_t removals;
    std::size_t modifications;
};

// --- Diff computation ---

// Compute a permission diff between old and new rule sets
[[nodiscard]] inline auto compute_permission_diff(
    const std::vector<std::pair<std::string, std::string>>& old_rules,
    const std::vector<std::pair<std::string, std::string>>& new_rules) -> PermissionDiff {

    PermissionDiff diff{};
    diff.additions = 0;
    diff.removals = 0;
    diff.modifications = 0;

    // Find removals and modifications
    for (const auto& [old_key, old_val] : old_rules) {
        bool found = false;
        for (const auto& [new_key, new_val] : new_rules) {
            if (old_key == new_key) {
                found = true;
                if (old_val != new_val) {
                    diff.changes.push_back(PermissionChange{
                        .tool_name = old_key,
                        .old_value = old_val,
                        .new_value = new_val,
                        .is_new = false,
                        .is_removed = false
                    });
                    ++diff.modifications;
                }
                break;
            }
        }
        if (!found) {
            diff.changes.push_back(PermissionChange{
                .tool_name = old_key,
                .old_value = old_val,
                .new_value = {},
                .is_new = false,
                .is_removed = true
            });
            ++diff.removals;
        }
    }

    // Find additions
    for (const auto& [new_key, new_val] : new_rules) {
        bool found = false;
        for (const auto& [old_key, old_val] : old_rules) {
            if (new_key == old_key) {
                found = true;
                break;
            }
        }
        if (!found) {
            diff.changes.push_back(PermissionChange{
                .tool_name = new_key,
                .old_value = {},
                .new_value = new_val,
                .is_new = true,
                .is_removed = false
            });
            ++diff.additions;
        }
    }

    return diff;
}

// --- Formatting functions ---

// Format a single permission change for terminal display
[[nodiscard]] inline auto format_permission_change(const PermissionChange& change) -> std::string {
    std::string result;
    if (change.is_new) {
        result += "\033[32m+ " + change.tool_name + ": " + change.new_value + "\033[0m";
    } else if (change.is_removed) {
        result += "\033[31m- " + change.tool_name + ": " + change.old_value + "\033[0m";
    } else {
        result += "\033[33m~ " + change.tool_name + ": ";
        result += "\033[31m" + change.old_value + "\033[33m -> ";
        result += "\033[32m" + change.new_value + "\033[0m";
    }
    return result;
}

// Format the full permission diff for terminal display
[[nodiscard]] inline auto format_permission_diff(const PermissionDiff& diff) -> std::string {
    if (diff.changes.empty()) {
        return "\033[2mNo permission changes.\033[0m\n";
    }

    std::string result;
    result += "\033[1mPermission Changes:\033[0m\n";
    result += "\033[2m  " + std::to_string(diff.additions) + " added, "
           + std::to_string(diff.removals) + " removed, "
           + std::to_string(diff.modifications) + " modified\033[0m\n\n";

    for (const auto& change : diff.changes) {
        result += "  " + format_permission_change(change) + "\n";
    }

    return result;
}

// Check if the diff contains breaking changes (removals or downgrades)
[[nodiscard]] inline auto has_breaking_changes(const PermissionDiff& diff) -> bool {
    if (diff.removals > 0) {
        return true;
    }
    // A modification from "Allow" to "Deny" is breaking
    for (const auto& change : diff.changes) {
        if (!change.is_new && !change.is_removed) {
            if (change.old_value == "Allow" && change.new_value == "Deny") {
                return true;
            }
        }
    }
    return false;
}

} // namespace cc::ui::permissions
