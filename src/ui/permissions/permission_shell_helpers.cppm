/// @file permission_shell_helpers.cppm
/// @brief Shared shell permission rendering helpers
module;

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <format>
#include <ftxui/dom/elements.hpp>

export module cc.ui.permissions.permission_shell_helpers;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Maximum command length before truncation
inline constexpr std::size_t kMaxCommandPreview = 120;

/// Truncate command for display
[[nodiscard]] inline std::string truncate_command(std::string_view cmd, std::size_t max_len = kMaxCommandPreview) {
    if (cmd.size() <= max_len) return std::string(cmd);
    return std::string(cmd.substr(0, max_len - 3)) + "...";
}

/// Render a list of environment variables that will be inherited
[[nodiscard]] inline Element render_env_vars(const std::vector<std::string>& vars) {
    if (vars.empty()) return text("");
    std::vector<Element> elements;
    elements.push_back(text("Environment:") | dim);
    for (const auto& var : vars) {
        elements.push_back(text("  " + var) | dim);
    }
    return vbox(elements);
}

/// Render permission decision footer
[[nodiscard]] inline Element render_permission_footer(bool allow_always = true) {
    if (allow_always) {
        return hbox({
            text("[Y]es") | bold, text(" / "),
            text("[N]o") | bold, text(" / "),
            text("[A]lways") | bold,
        });
    }
    return hbox({text("[Y]es") | bold, text(" / "), text("[N]o") | bold});
}

/// Render the permission rule matches
[[nodiscard]] inline Element render_rule_matches(const std::vector<std::string>& rules) {
    if (rules.empty()) return text("");
    std::vector<Element> elements;
    elements.push_back(text("Matched rules:") | dim);
    for (const auto& rule : rules) {
        elements.push_back(text("  " + rule) | dim);
    }
    return vbox(elements);
}

} // namespace cc::ui::permissions
