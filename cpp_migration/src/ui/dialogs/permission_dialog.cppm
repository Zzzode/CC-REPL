module;
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <utility>

export module cc.ui.dialogs.permission_dialog;

export namespace cc::ui::dialogs {

[[nodiscard]] inline std::string repeat_permission_dialog(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) result += text;
    return result;
}

// A request for permission to execute a tool
struct PermissionRequest {
    std::string tool_name;
    std::string description;
    std::string risk_level;  // "low", "medium", "high"
    std::vector<std::string> affected_paths;

    // TODO(UI8, trust_utils): the string `risk_level` field should instead
    //   use `cc::ui::trust_utils::RiskLevel` (Low / Medium / High / Critical)
    //   so this dialog can share the classification logic with TrustDialog.
    //   Migration path:
    //
    //     1. import cc.ui.trust_utils;
    //     2. change `risk_level` to `cc::ui::trust_utils::RiskLevel level;`
    //     3. at build site, call:
    //          level = classify_risk(ActionType::Command, summary)
    //          or   level = from_danger_level(check_command_safety(cmd).level)
    //        to avoid duplicating the danger rules in bash_security.cppm.
    //     4. for path-access permissions, also call
    //          scan_paths_for_sensitive(affected_paths)
    //        and surface matches via TrustDialog instead of this headless
    //        string-render helper.
};

// User's choice on a permission prompt
enum class PermissionChoice { Allow, AlwaysAllow, Deny, Abort };

// Get all available permission choices with their display labels
inline auto get_permission_choices() -> std::vector<std::pair<PermissionChoice, std::string>> {
    return {
        {PermissionChoice::Allow,       "Allow (once)"},
        {PermissionChoice::AlwaysAllow, "Always allow"},
        {PermissionChoice::Deny,        "Deny"},
        {PermissionChoice::Abort,       "Abort"},
    };
}

// Render the permission dialog UI
inline auto render_permission_dialog(PermissionRequest request, int width) -> std::string {
    std::ostringstream out;
    int inner_width = std::max(40, width - 4);

    // Risk level color
    const char* risk_color = "\033[32m"; // green for low
    if (request.risk_level == "medium") risk_color = "\033[33m"; // yellow
    else if (request.risk_level == "high") risk_color = "\033[31m"; // red

    // Top border
    out << "╭" << repeat_permission_dialog("─", inner_width) << "╮\n";

    // Title
    out << "│ \033[1m🔐 Permission Required\033[0m"
        << std::string(std::max(0, inner_width - 23), ' ') << "│\n";
    out << "├" << repeat_permission_dialog("─", inner_width) << "┤\n";

    // Tool name
    out << "│ Tool: \033[36m" << request.tool_name << "\033[0m"
        << std::string(std::max(0, inner_width - 7 - static_cast<int>(request.tool_name.size())), ' ')
        << "│\n";

    // Risk level
    out << "│ Risk: " << risk_color << request.risk_level << "\033[0m"
        << std::string(std::max(0, inner_width - 7 - static_cast<int>(request.risk_level.size())), ' ')
        << "│\n";

    // Description
    out << "│" << std::string(inner_width, ' ') << "│\n";
    out << "│ " << request.description;
    int desc_pad = inner_width - 1 - static_cast<int>(request.description.size());
    if (desc_pad > 0) out << std::string(desc_pad, ' ');
    out << "│\n";

    // Affected paths
    if (!request.affected_paths.empty()) {
        out << "│" << std::string(inner_width, ' ') << "│\n";
        out << "│ \033[2mAffected paths:\033[0m"
            << std::string(std::max(0, inner_width - 16), ' ') << "│\n";
        for (const auto& path : request.affected_paths) {
            out << "│   \033[2m• " << path << "\033[0m";
            int path_pad = inner_width - 5 - static_cast<int>(path.size());
            if (path_pad > 0) out << std::string(path_pad, ' ');
            out << "│\n";
        }
    }

    // Choices
    out << "│" << std::string(inner_width, ' ') << "│\n";
    auto choices = get_permission_choices();
    for (size_t i = 0; i < choices.size(); ++i) {
        out << "│   \033[1m" << (i + 1) << ".\033[0m " << choices[i].second;
        int choice_pad = inner_width - 6 - static_cast<int>(choices[i].second.size());
        if (choice_pad > 0) out << std::string(choice_pad, ' ');
        out << "│\n";
    }

    // Bottom border
    out << "╰" << repeat_permission_dialog("─", inner_width) << "╯";

    return out.str();
}

} // namespace cc::ui::dialogs
