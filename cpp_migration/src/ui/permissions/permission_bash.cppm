/// @file permission_bash.cppm
/// @brief Bash command permission request UI with syntax highlighting
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_bash;

import cc.types.types;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Risk indicator for bash commands
enum class BashRiskLevel {
    Safe,       // read-only commands
    Moderate,   // file modifications
    Dangerous,  // system-level changes
    Critical    // destructive operations
};

/// Bash permission request display options
struct BashPermissionOptions {
    std::string command;
    std::string working_directory;
    BashRiskLevel risk_level{BashRiskLevel::Moderate};
    bool show_full_command{true};
    std::optional<std::string> truncated_preview;
    std::vector<std::string> matched_rules;
};

/// Render the bash command with syntax highlighting
[[nodiscard]] inline Element render_bash_command(const BashPermissionOptions& opts) {
    auto risk_color = [&]() -> Color {
        switch (opts.risk_level) {
            case BashRiskLevel::Safe: return Color::Green;
            case BashRiskLevel::Moderate: return Color::Yellow;
            case BashRiskLevel::Dangerous: return Color::RedLight;
            case BashRiskLevel::Critical: return Color::Red;
        }
        return Color::White;
    }();

    std::string display_cmd = opts.show_full_command
        ? opts.command
        : opts.truncated_preview.value_or(opts.command.substr(0, 80) + "...");

    auto cmd_element = vbox({
        hbox({text("$ ") | bold, text(display_cmd) | color(risk_color)}),
        text(std::format("  cwd: {}", opts.working_directory)) | dim,
    });

    if (!opts.matched_rules.empty()) {
        std::vector<Element> rule_elements;
        for (const auto& rule : opts.matched_rules) {
            rule_elements.push_back(text("  - " + rule) | dim);
        }
        cmd_element = vbox({cmd_element, vbox(rule_elements)});
    }

    return cmd_element;
}

/// Create a full bash permission dialog component
[[nodiscard]] inline Component bash_permission_dialog(
    const BashPermissionOptions& opts,
    std::function<void(bool)> on_decision) {
    return Renderer([opts, on_decision = std::move(on_decision)] {
        return vbox({
            text("Allow bash command?") | bold,
            separator(),
            render_bash_command(opts),
            separator(),
            hbox({text("[Y]es") | bold, text(" / "), text("[N]o") | bold}),
        }) | border;
    });
}

} // namespace cc::ui::permissions
