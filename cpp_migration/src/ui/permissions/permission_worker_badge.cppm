/// @file permission_worker_badge.cppm
/// @brief Badge/indicator for worker permission mode
module;

#include <string>
#include <string_view>
#include <ftxui/dom/elements.hpp>

export module cc.ui.permissions.permission_worker_badge;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Permission mode types
enum class PermissionMode {
    Normal,
    AcceptEdits,
    PlanMode,
    BypassAll,
};

/// Get display text for permission mode
[[nodiscard]] inline std::string_view mode_label(PermissionMode mode) {
    switch (mode) {
        case PermissionMode::Normal: return "normal";
        case PermissionMode::AcceptEdits: return "accept-edits";
        case PermissionMode::PlanMode: return "plan-mode";
        case PermissionMode::BypassAll: return "bypass";
    }
    return "unknown";
}

/// Render the permission mode badge
[[nodiscard]] inline Element render_permission_badge(PermissionMode mode) {
    auto label = std::string(mode_label(mode));
    auto badge_color = [&]() -> Color {
        switch (mode) {
            case PermissionMode::Normal: return Color::Green;
            case PermissionMode::AcceptEdits: return Color::Yellow;
            case PermissionMode::PlanMode: return Color::Cyan;
            case PermissionMode::BypassAll: return Color::Red;
        }
        return Color::White;
    }();
    return text("[" + label + "]") | bold | color(badge_color);
}

/// Render worker badge with agent name
[[nodiscard]] inline Element render_worker_badge(
    std::string_view agent_name, PermissionMode mode) {
    return hbox({
        text(std::string(agent_name)) | dim,
        text(" "),
        render_permission_badge(mode),
    });
}

} // namespace cc::ui::permissions
