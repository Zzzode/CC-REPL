/// @file sandbox_settings.cppm
/// @brief Faithful port of SandboxSettings.tsx + sub-tabs.
///
/// Provides a tabbed settings dialog for sandbox configuration:
///   - Mode tab:        auto-allow / regular / disabled selection
///   - Overrides tab:   allow unsandboxed fallback / strict sandbox
///   - Config tab:      read-only view of fs/network/excluded-commands config
///   - Dependencies tab: platform-specific dep checks (ripgrep, bwrap, socat,
///                      seccomp) with install hints
///   - Doctor section:  health summary (errors / warnings)
///
/// TS REF: src/components/sandbox/SandboxSettings.tsx (295)
/// TS REF: src/components/sandbox/SandboxConfigTab.tsx (44)
/// TS REF: src/components/sandbox/SandboxDependenciesTab.tsx (119)
/// TS REF: src/components/sandbox/SandboxOverridesTab.tsx (192)
/// TS REF: src/components/sandbox/SandboxDoctorSection.tsx (45)
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.sandbox_settings;

import cc.utils.platform;

export namespace cc::ui::dialogs::sandbox_settings {

using namespace ftxui;
namespace platform = cc::utils::platform;

// ============================================================
// Types
// ============================================================

/// Sandbox execution mode — TS REF: SandboxSettings.tsx:21
enum class SandboxMode : std::uint8_t {
    auto_allow,  // Sandbox BashTool, with auto-allow
    regular,     // Sandbox BashTool, with regular permissions
    disabled,    // No Sandbox
};

/// Override mode for unsandboxed commands — TS REF: SandboxOverridesTab.tsx:13
enum class OverrideMode : std::uint8_t {
    open,   // Allow unsandboxed fallback
    closed, // Strict sandbox mode
};

/// Available tabs — TS REF: SandboxSettings.tsx:197-211
enum class SandboxTabId : std::uint8_t {
    Mode = 0,
    Overrides,
    Config,
    Dependencies,
    _COUNT,
};

/// Convert tab to display label — TS REF: SandboxSettings.tsx:171-197
[[nodiscard]] inline std::string tab_label(SandboxTabId id) {
    switch (id) {
        case SandboxTabId::Mode:         return "Mode";
        case SandboxTabId::Overrides:    return "Overrides";
        case SandboxTabId::Config:       return "Config";
        case SandboxTabId::Dependencies: return "Dependencies";
        case SandboxTabId::_COUNT:       return "?";
    }
    return "?";
}

/// Dependency check severity
enum class DepSeverity : std::uint8_t { info, warning, error };

/// A single dependency check entry — TS REF: SandboxDependenciesTab.tsx
struct DepCheckEntry {
    std::string message;
    DepSeverity severity = DepSeverity::info;
};

/// Full dependency check state — mirrors TS SandboxDependencyCheck
struct SandboxDepCheck {
    std::vector<DepCheckEntry> warnings;
    std::vector<DepCheckEntry> errors;
};

/// Command result display mode — TS REF: SandboxSettings.tsx:16-19
enum class CommandResultDisplay : std::uint8_t { normal, system, skip };

/// Props for the sandbox settings dialog — TS REF: SandboxSettings.tsx:15-20
struct SandboxSettingsProps {
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_complete;
    SandboxDepCheck dep_check;
    SandboxMode current_mode = SandboxMode::regular;
    bool sandbox_enabled = true;
    bool auto_allow_bash = false;
    bool allow_unsandboxed = true;
    bool settings_locked_by_policy = false;
    bool allow_all_unix_sockets = false;
};

// ============================================================
// Config tab data — TS REF: SandboxConfigTab.tsx:29-35
// ============================================================

/// Read-only sandbox configuration snapshot.
/// Populated from SandboxManager getters in TS; in CPP the caller supplies
/// these via SandboxConfigSnapshot.
struct FsReadConfig {
    std::vector<std::string> deny_only;
    std::vector<std::string> allow_within_deny;
};
struct FsWriteConfig {
    std::vector<std::string> allow_only;
    std::vector<std::string> deny_within_allow;
};
struct NetworkConfig {
    std::vector<std::string> allowed_hosts;
    std::vector<std::string> denied_hosts;
};
struct SandboxConfigSnapshot {
    FsReadConfig fs_read;
    FsWriteConfig fs_write;
    NetworkConfig network;
    std::vector<std::string> allow_unix_sockets;
    std::vector<std::string> excluded_commands;
    std::vector<std::string> glob_pattern_warnings;
    bool managed_domains_only = false;
};

// ============================================================
// Helper: color for severity
// ============================================================

[[nodiscard]] inline Color severity_color(DepSeverity s) {
    switch (s) {
        case DepSeverity::error:   return Color::Red;
        case DepSeverity::warning: return Color::Yellow;
        case DepSeverity::info:    return Color::Green;
    }
    return Color::White;
}

// ============================================================
// Rendering: Tab header
// ============================================================

/// Render the tab bar at the top of the dialog.
/// TS REF: SandboxSettings.tsx:213-219 (Pane + Tabs)
[[nodiscard]] inline Element RenderTabHeader(SandboxTabId selected,
                                              bool has_errors,
                                              bool has_warnings) {
    // TS REF: SandboxSettings.tsx:198-211 — tab visibility logic
    std::vector<SandboxTabId> tabs;
    if (has_errors) {
        // Only show dependencies tab when there are blocking errors
        tabs.push_back(SandboxTabId::Dependencies);
    } else {
        tabs.push_back(SandboxTabId::Mode);
        if (has_warnings) tabs.push_back(SandboxTabId::Dependencies);
        tabs.push_back(SandboxTabId::Overrides);
        tabs.push_back(SandboxTabId::Config);
    }

    Elements elements;
    elements.push_back(text(" Sandbox: ") | bold | color(Color::Magenta));
    elements.push_back(text(" "));

    for (const auto& tab : tabs) {
        auto label = tab_label(tab);
        auto el = text(" " + label + " ");
        if (tab == selected) {
            el = el | bold | inverted;
        } else {
            el = el | dim;
        }
        elements.push_back(el);
    }
    return hbox(elements);
}

// ============================================================
// Rendering: Mode tab — TS REF: SandboxSettings.tsx:222-294
// ============================================================

/// Render a single mode option line.
/// TS REF: SandboxSettings.tsx:60-106 (options array with current indicator)
[[nodiscard]] inline Element RenderModeOption(
    SandboxMode mode, SandboxMode current, bool selected) {

    std::string label;
    switch (mode) {
        case SandboxMode::auto_allow:
            label = "Sandbox BashTool, with auto-allow"; break;
        case SandboxMode::regular:
            label = "Sandbox BashTool, with regular permissions"; break;
        case SandboxMode::disabled:
            label = "No Sandbox"; break;
    }

    auto prefix = selected
        ? text("❯ ") | color(Color::Cyan)
        : text("  ");

    auto label_el = text(label);
    if (selected) label_el = label_el | bold;

    if (mode == current) {
        auto suffix = text(" (current)") | color(Color::Green);
        return hbox({prefix, label_el, suffix});
    }
    return hbox({prefix, label_el});
}

/// Render the Mode tab content.
/// TS REF: SandboxSettings.tsx:222-294 (SandboxModeTab function)
[[nodiscard]] inline Element RenderModeTab(
    SandboxMode current_mode, int selected_index, bool show_socket_warning) {

    Elements items;

    // TS REF: SandboxSettings.tsx:236-241 (socket warning)
    if (show_socket_warning) {
        items.push_back(
            text("Cannot block unix domain sockets (see Dependencies tab)")
            | color(Color::Yellow));
        items.push_back(text(""));
    }

    // TS REF: SandboxSettings.tsx:243-245 (Configure Mode heading)
    items.push_back(text("Configure Mode:") | bold);
    items.push_back(text(""));

    // TS REF: SandboxSettings.tsx:60-106 (mode options)
    constexpr std::array<SandboxMode, 3> modes = {
        SandboxMode::auto_allow,
        SandboxMode::regular,
        SandboxMode::disabled,
    };
    for (int i = 0; i < 3; ++i) {
        items.push_back(RenderModeOption(modes[i], current_mode, i == selected_index));
    }

    // TS REF: SandboxSettings.tsx:272-283 (description + learn more)
    items.push_back(text(""));
    items.push_back(vbox({
        text("Auto-allow mode:") | bold | dim,
        paragraph("  Commands will try to run in the sandbox automatically, "
                 "and attempts to run outside of the sandbox fallback to "
                 "regular permissions. Explicit ask/deny rules are always "
                 "respected.") | dim,
        text(""),
        hbox({
            text("Learn more: ") | dim,
            text("code.claude.com/docs/en/sandboxing")
                | color(Color::Cyan) | underlined,
        }),
    }));

    return vbox(items);
}

// ============================================================
// Rendering: Overrides tab — TS REF: SandboxOverridesTab.tsx
// ============================================================

/// Render a single override option line.
/// TS REF: SandboxOverridesTab.tsx:83-115 (options array with current indicator)
[[nodiscard]] inline Element RenderOverrideOption(
    OverrideMode mode, OverrideMode current, bool selected,
    const std::string& label_text) {

    auto prefix = selected
        ? text("❯ ") | color(Color::Cyan)
        : text("  ");

    auto label_el = text(label_text);
    if (selected) label_el = label_el | bold;

    if (mode == current) {
        auto suffix = text(" (current)") | color(Color::Green);
        return hbox({prefix, label_el, suffix});
    }
    return hbox({prefix, label_el});
}

/// Render the Overrides tab.
/// TS REF: SandboxOverridesTab.tsx:14-57 (SandboxOverridesTab function)
[[nodiscard]] inline Element RenderOverridesTab(
    bool sandbox_enabled,
    bool settings_locked,
    bool allow_unsandboxed,
    OverrideMode current_override_mode,
    int selected_index) {

    // TS REF: SandboxOverridesTab.tsx:22-30 (sandbox not enabled)
    if (!sandbox_enabled) {
        return vbox({
            text("Sandbox is not enabled. Enable sandbox to configure "
                 "override settings.") | color(Color::GrayLight),
        });
    }

    // TS REF: SandboxOverridesTab.tsx:32-47 (locked by policy)
    if (settings_locked) {
        return vbox({
            text("Override settings are managed by a higher-priority "
                 "configuration and cannot be changed locally.")
                | color(Color::GrayLight),
            text(""),
            hbox({
                text("Current setting: ") | dim,
                text(allow_unsandboxed
                         ? "Allow unsandboxed fallback"
                         : "Strict sandbox mode")
                    | color(Color::Cyan),
            }),
        });
    }

    // TS REF: SandboxOverridesTab.tsx:63-191 (OverridesSelect function)
    // Description + Select
    Elements items;
    items.push_back(text("Configure Overrides:") | bold);
    items.push_back(text(""));

    items.push_back(RenderOverrideOption(
        OverrideMode::open, current_override_mode, selected_index == 0,
        "Allow unsandboxed fallback"));
    items.push_back(RenderOverrideOption(
        OverrideMode::closed, current_override_mode, selected_index == 1,
        "Strict sandbox mode"));

    items.push_back(text(""));
    items.push_back(vbox({
        text("Allow unsandboxed fallback:") | bold | dim,
        paragraph("  When a command fails due to sandbox restrictions, "
                 "Claude can retry with dangerouslyDisableSandbox to "
                 "run outside the sandbox (falling back to default "
                 "permissions).") | dim,
        text(""),
        text("Strict sandbox mode:") | bold | dim,
        paragraph("  All bash commands invoked by the model must run in "
                 "the sandbox unless they are explicitly listed in "
                 "excludedCommands.") | dim,
        text(""),
        hbox({
            text("Learn more: ") | dim,
            text("code.claude.com/docs/en/sandboxing#configure-sandboxing")
                | color(Color::Cyan) | underlined,
        }),
    }));

    return vbox(items);
}

// ============================================================
// Rendering: Config tab — TS REF: SandboxConfigTab.tsx
// ============================================================

/// Render the Config tab showing read-only sandbox configuration.
/// TS REF: SandboxConfigTab.tsx:5-41
[[nodiscard]] inline Element RenderConfigTab(
    bool sandbox_enabled,
    const SandboxConfigSnapshot& cfg,
    const SandboxDepCheck& dep_check) {

    // TS REF: SandboxConfigTab.tsx:17-25 (sandbox not enabled)
    if (!sandbox_enabled) {
        Elements items;
        items.push_back(text("Sandbox is not enabled") | color(Color::GrayLight));

        // TS REF: SandboxConfigTab.tsx:9-15 (warnings note)
        if (!dep_check.warnings.empty()) {
            for (const auto& w : dep_check.warnings) {
                items.push_back(text(w.message) | dim);
            }
        }
        return vbox(items);
    }

    Elements items;

    // TS REF: SandboxConfigTab.tsx:31 (Excluded Commands)
    {
        items.push_back(hbox({
            text("Excluded Commands:") | bold | color(Color::Magenta),
        }));
        std::string excluded_str = cfg.excluded_commands.empty()
            ? "None"
            : [&]() {
                std::string s;
                for (std::size_t i = 0; i < cfg.excluded_commands.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += cfg.excluded_commands[i];
                }
                return s;
              }();
        items.push_back(text(excluded_str) | dim);
    }

    // TS REF: SandboxConfigTab.tsx:32 (Filesystem Read Restrictions)
    if (!cfg.fs_read.deny_only.empty()) {
        items.push_back(text(""));
        items.push_back(hbox({
            text("Filesystem Read Restrictions:") | bold | color(Color::Magenta),
        }));
        std::string denied_str;
        for (std::size_t i = 0; i < cfg.fs_read.deny_only.size(); ++i) {
            if (i > 0) denied_str += ", ";
            denied_str += cfg.fs_read.deny_only[i];
        }
        items.push_back(text("Denied: " + denied_str) | dim);

        if (!cfg.fs_read.allow_within_deny.empty()) {
            std::string allow_str;
            for (std::size_t i = 0; i < cfg.fs_read.allow_within_deny.size(); ++i) {
                if (i > 0) allow_str += ", ";
                allow_str += cfg.fs_read.allow_within_deny[i];
            }
            items.push_back(text("Allowed within denied: " + allow_str) | dim);
        }
    }

    // TS REF: SandboxConfigTab.tsx:33 (Filesystem Write Restrictions)
    if (!cfg.fs_write.allow_only.empty()) {
        items.push_back(text(""));
        items.push_back(hbox({
            text("Filesystem Write Restrictions:") | bold | color(Color::Magenta),
        }));
        std::string allow_str;
        for (std::size_t i = 0; i < cfg.fs_write.allow_only.size(); ++i) {
            if (i > 0) allow_str += ", ";
            allow_str += cfg.fs_write.allow_only[i];
        }
        items.push_back(text("Allowed: " + allow_str) | dim);

        if (!cfg.fs_write.deny_within_allow.empty()) {
            std::string deny_str;
            for (std::size_t i = 0; i < cfg.fs_write.deny_within_allow.size(); ++i) {
                if (i > 0) deny_str += ", ";
                deny_str += cfg.fs_write.deny_within_allow[i];
            }
            items.push_back(text("Denied within allowed: " + deny_str) | dim);
        }
    }

    // TS REF: SandboxConfigTab.tsx:34 (Network Restrictions)
    {
        bool has_allowed = !cfg.network.allowed_hosts.empty();
        bool has_denied = !cfg.network.denied_hosts.empty();
        if (has_allowed || has_denied) {
            items.push_back(text(""));
            std::string managed_tag = cfg.managed_domains_only
                ? " (Managed)" : "";
            items.push_back(hbox({
                text("Network Restrictions" + managed_tag + ":")
                    | bold | color(Color::Magenta),
            }));
            if (has_allowed) {
                std::string s;
                for (std::size_t i = 0; i < cfg.network.allowed_hosts.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += cfg.network.allowed_hosts[i];
                }
                items.push_back(text("Allowed: " + s) | dim);
            }
            if (has_denied) {
                std::string s;
                for (std::size_t i = 0; i < cfg.network.denied_hosts.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += cfg.network.denied_hosts[i];
                }
                items.push_back(text("Denied: " + s) | dim);
            }
        }
    }

    // TS REF: SandboxConfigTab.tsx:34 (Allowed Unix Sockets)
    if (!cfg.allow_unix_sockets.empty()) {
        items.push_back(text(""));
        items.push_back(hbox({
            text("Allowed Unix Sockets:") | bold | color(Color::Magenta),
        }));
        std::string s;
        for (std::size_t i = 0; i < cfg.allow_unix_sockets.size(); ++i) {
            if (i > 0) s += ", ";
            s += cfg.allow_unix_sockets[i];
        }
        items.push_back(text(s) | dim);
    }

    // TS REF: SandboxConfigTab.tsx:35 (Glob Pattern Warnings)
    if (!cfg.glob_pattern_warnings.empty()) {
        items.push_back(text(""));
        items.push_back(hbox({
            text("⚠ Warning: Glob patterns not fully supported on Linux")
                | bold | color(Color::Yellow),
        }));
        std::string shown;
        int shown_count = std::min<int>(3,
            static_cast<int>(cfg.glob_pattern_warnings.size()));
        for (int i = 0; i < shown_count; ++i) {
            if (i > 0) shown += ", ";
            shown += cfg.glob_pattern_warnings[i];
        }
        int remaining = static_cast<int>(cfg.glob_pattern_warnings.size()) - shown_count;
        if (remaining > 0) {
            shown += " (" + std::to_string(remaining) + " more)";
        }
        items.push_back(
            text("The following patterns will be ignored: " + shown) | dim);
    }

    // TS REF: SandboxConfigTab.tsx:9-15 (dep check warnings note)
    if (!dep_check.warnings.empty()) {
        items.push_back(text(""));
        for (const auto& w : dep_check.warnings) {
            items.push_back(text(w.message) | dim);
        }
    }

    return vbox(items) | yframe | flex;
}

// ============================================================
// Rendering: Dependencies tab — TS REF: SandboxDependenciesTab.tsx
// ============================================================

/// Render the Dependencies tab with platform-specific install hints.
/// TS REF: SandboxDependenciesTab.tsx:9-103
[[nodiscard]] inline Element RenderDependenciesTab(
    const SandboxDepCheck& dep_check) {

    bool is_mac = platform::is_macos();

    // TS REF: SandboxDependenciesTab.tsx:31-49 (classify errors)
    auto contains = [](const std::vector<DepCheckEntry>& entries,
                       std::string_view needle) -> bool {
        for (const auto& e : entries) {
            if (e.message.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    bool rg_missing = contains(dep_check.errors, "ripgrep");
    bool bwrap_missing = contains(dep_check.errors, "bwrap");
    bool socat_missing = contains(dep_check.errors, "socat");
    bool seccomp_missing = !dep_check.warnings.empty();

    // TS REF: SandboxDependenciesTab.tsx:53 (other errors filter)
    std::vector<std::string> other_errors;
    for (const auto& e : dep_check.errors) {
        if (e.message.find("ripgrep") == std::string::npos &&
            e.message.find("bwrap") == std::string::npos &&
            e.message.find("socat") == std::string::npos) {
            other_errors.push_back(e.message);
        }
    }

    // TS REF: SandboxDependenciesTab.tsx:54 (install hint)
    std::string rg_install_hint = is_mac
        ? "brew install ripgrep" : "apt install ripgrep";

    Elements items;

    // TS REF: SandboxDependenciesTab.tsx:57-61 (macOS seatbelt)
    if (is_mac) {
        items.push_back(hbox({
            text("seatbelt: "),
            text("built-in (macOS)") | color(Color::Green),
        }));
    }

    // TS REF: SandboxDependenciesTab.tsx:65-82 (ripgrep)
    {
        auto status = rg_missing
            ? text("not found") | color(Color::Red)
            : text("found") | color(Color::Green);
        items.push_back(hbox({
            text("ripgrep (rg): "),
            status,
        }));
        if (rg_missing) {
            items.push_back(
                text("  · " + rg_install_hint) | dim);
        }
    }

    // TS REF: SandboxDependenciesTab.tsx:84-92 (Linux deps: bwrap, socat, seccomp)
    if (!is_mac) {
        // bwrap
        {
            auto status = bwrap_missing
                ? text("not installed") | color(Color::Red)
                : text("installed") | color(Color::Green);
            items.push_back(hbox({
                text("bubblewrap (bwrap): "),
                status,
            }));
            if (bwrap_missing) {
                items.push_back(
                    text("  · apt install bubblewrap") | dim);
            }
        }
        // socat
        {
            auto status = socat_missing
                ? text("not installed") | color(Color::Red)
                : text("installed") | color(Color::Green);
            items.push_back(hbox({
                text("socat: "),
                status,
            }));
            if (socat_missing) {
                items.push_back(
                    text("  · apt install socat") | dim);
            }
        }
        // seccomp
        {
            auto status = seccomp_missing
                ? text("not installed") | color(Color::Yellow)
                : text("installed") | color(Color::Green);
            auto suffix = seccomp_missing
                ? text(" (required to block unix domain sockets)") | dim
                : text("");
            items.push_back(hbox({
                text("seccomp filter: "),
                status,
                suffix,
            }));
            if (seccomp_missing) {
                items.push_back(
                    text("  · npm install -g @anthropic-ai/sandbox-runtime")
                        | dim);
                items.push_back(
                    text("  · or copy vendor/seccomp/* from sandbox-runtime "
                         "and set") | dim);
                items.push_back(
                    text("    sandbox.seccomp.bpfPath and applyPath in settings.json")
                        | dim);
            }
        }
    }

    // TS REF: SandboxDependenciesTab.tsx:93 (other errors)
    for (const auto& err : other_errors) {
        items.push_back(text(err) | color(Color::Red));
    }

    return vbox(items) | yframe | flex;
}

// ============================================================
// Rendering: Doctor section — TS REF: SandboxDoctorSection.tsx
// ============================================================

/// Render the Doctor / health summary section.
/// Returns std::nullopt if nothing to report (healthy).
/// TS REF: SandboxDoctorSection.tsx:5-38
[[nodiscard]] inline std::optional<Element> RenderDoctorSection(
    bool supported_platform,
    bool sandbox_enabled_in_settings,
    const SandboxDepCheck& dep_check) {

    // TS REF: SandboxDoctorSection.tsx:7-12 (early returns)
    if (!supported_platform) return std::nullopt;
    if (!sandbox_enabled_in_settings) return std::nullopt;

    // TS REF: SandboxDoctorSection.tsx:18-24 (check for issues)
    bool has_errors = !dep_check.errors.empty();
    bool has_warnings = !dep_check.warnings.empty();
    if (!has_errors && !has_warnings) return std::nullopt;

    // TS REF: SandboxDoctorSection.tsx:25-26 (status color + text)
    Color status_color = has_errors ? Color::Red : Color::Yellow;
    std::string status_text = has_errors
        ? "Missing dependencies" : "Available (with warnings)";

    Elements items;
    items.push_back(text("Sandbox") | bold);
    items.push_back(hbox({
        text("└ Status: "),
        text(status_text) | color(status_color),
    }));

    // TS REF: SandboxDoctorSection.tsx:27 (errors map)
    for (const auto& e : dep_check.errors) {
        items.push_back(hbox({
            text("└ "),
            text(e.message) | color(Color::Red),
        }));
    }
    // TS REF: SandboxDoctorSection.tsx:27 (warnings map)
    for (const auto& w : dep_check.warnings) {
        items.push_back(hbox({
            text("└ "),
            text(w.message) | color(Color::Yellow),
        }));
    }
    // TS REF: SandboxDoctorSection.tsx:27 (run /sandbox hint)
    if (has_errors) {
        items.push_back(
            text("└ Run /sandbox for install instructions") | dim);
    }

    return vbox(items);
}

// ============================================================
// Full dialog renderer
// ============================================================

/// Render the complete sandbox settings dialog with the active tab.
/// TS REF: SandboxSettings.tsx:213-220 (Pane + Tabs container)
[[nodiscard]] inline Element RenderSandboxSettings(
    const SandboxSettingsProps& props,
    const SandboxConfigSnapshot& cfg,
    SandboxTabId active_tab,
    int mode_selected,
    int override_selected) {

    bool has_errors = !props.dep_check.errors.empty();
    bool has_warnings = !props.dep_check.warnings.empty();
    bool show_socket_warning = has_warnings && !props.allow_all_unix_sockets;

    auto header = RenderTabHeader(active_tab, has_errors, has_warnings);

    Element content;
    switch (active_tab) {
        case SandboxTabId::Mode:
            content = RenderModeTab(
                props.current_mode, mode_selected, show_socket_warning);
            break;
        case SandboxTabId::Overrides: {
            auto cur_mode = props.allow_unsandboxed
                ? OverrideMode::open : OverrideMode::closed;
            content = RenderOverridesTab(
                props.sandbox_enabled,
                props.settings_locked_by_policy,
                props.allow_unsandboxed,
                cur_mode,
                override_selected);
            break;
        }
        case SandboxTabId::Config:
            content = RenderConfigTab(
                props.sandbox_enabled, cfg, props.dep_check);
            break;
        case SandboxTabId::Dependencies:
            content = RenderDependenciesTab(props.dep_check);
            break;
        case SandboxTabId::_COUNT:
            content = text("(empty)");
            break;
    }

    auto body = vbox({
        header,
        separator(),
        content | flex,
    });

    return window(
        text(" Sandbox: ") | bold | color(Color::Magenta),
        body
    ) | color(Color::Magenta);
}

// ============================================================
// Interactive Component
// ============================================================

/// Determine the visible tab order based on dep check state.
/// TS REF: SandboxSettings.tsx:198-211
[[nodiscard]] inline std::vector<SandboxTabId> VisibleTabs(
    bool has_errors, bool has_warnings) {
    std::vector<SandboxTabId> tabs;
    if (has_errors) {
        tabs.push_back(SandboxTabId::Dependencies);
    } else {
        tabs.push_back(SandboxTabId::Mode);
        if (has_warnings) tabs.push_back(SandboxTabId::Dependencies);
        tabs.push_back(SandboxTabId::Overrides);
        tabs.push_back(SandboxTabId::Config);
    }
    return tabs;
}

/// Create the sandbox settings dialog component.
/// TS REF: SandboxSettings.tsx (full component)
/// TS REF: SandboxOverridesTab.tsx (OverridesSelect interactive)
[[nodiscard]] inline Component SandboxSettingsDialog(
    SandboxSettingsProps props,
    SandboxConfigSnapshot config_snapshot = {}) {

    struct State {
        SandboxSettingsProps props;
        SandboxConfigSnapshot config;
        SandboxTabId active_tab;
        int mode_selected = 0;
        int override_selected = 0;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);
    state->config = std::move(config_snapshot);

    // TS REF: SandboxSettings.tsx:339-343 (start on deps if errors)
    bool has_errors = !state->props.dep_check.errors.empty();
    if (has_errors) {
        state->active_tab = SandboxTabId::Dependencies;
    } else {
        state->active_tab = SandboxTabId::Mode;
    }

    // TS REF: SandboxSettings.tsx:42-51 (determine current mode)
    auto get_current_mode = [](const SandboxSettingsProps& p) -> SandboxMode {
        if (!p.sandbox_enabled) return SandboxMode::disabled;
        if (p.auto_allow_bash) return SandboxMode::auto_allow;
        return SandboxMode::regular;
    };
    state->props.current_mode = get_current_mode(state->props);

    // Sync selected index to current mode
    switch (state->props.current_mode) {
        case SandboxMode::auto_allow: state->mode_selected = 0; break;
        case SandboxMode::regular:    state->mode_selected = 1; break;
        case SandboxMode::disabled:   state->mode_selected = 2; break;
    }

    // Sync override selected
    state->override_selected = state->props.allow_unsandboxed ? 0 : 1;

    return Renderer([state] {
        return RenderSandboxSettings(
            state->props, state->config,
            state->active_tab,
            state->mode_selected,
            state->override_selected);
    }) | CatchEvent([state](Event event) -> bool {
        bool has_errors = !state->props.dep_check.errors.empty();
        bool has_warnings = !state->props.dep_check.warnings.empty();
        auto visible = VisibleTabs(has_errors, has_warnings);

        // Find current tab index in visible list
        auto it = std::find(visible.begin(), visible.end(),
                            state->active_tab);
        int cur_idx = (it != visible.end())
            ? static_cast<int>(it - visible.begin()) : 0;

        // --- Tab navigation ---
        // TS REF: SandboxSettings.tsx uses Tabs component with arrow key nav
        if (event == Event::ArrowLeft || event == Event::Character('h')) {
            if (cur_idx > 0) {
                state->active_tab = visible[cur_idx - 1];
            }
            return true;
        }
        if (event == Event::ArrowRight || event == Event::Character('l')) {
            if (cur_idx < static_cast<int>(visible.size()) - 1) {
                state->active_tab = visible[cur_idx + 1];
            }
            return true;
        }

        // --- Mode tab: item selection ---
        // TS REF: SandboxSettings.tsx:260-267 (Select with onUpFromFirstItem → focusHeader)
        // TS REF: SandboxSettings.tsx:110-140 (handleSelect)
        if (state->active_tab == SandboxTabId::Mode) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->mode_selected = std::max(0, state->mode_selected - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->mode_selected = std::min(2, state->mode_selected + 1);
                return true;
            }
            if (event == Event::Return) {
                // TS REF: SandboxSettings.tsx:110-139 (handleSelect switch)
                std::string msg;
                switch (state->mode_selected) {
                    case 0: // auto_allow
                        state->props.sandbox_enabled = true;
                        state->props.auto_allow_bash = true;
                        state->props.current_mode = SandboxMode::auto_allow;
                        msg = "✓ Sandbox enabled with auto-allow for bash commands";
                        break;
                    case 1: // regular
                        state->props.sandbox_enabled = true;
                        state->props.auto_allow_bash = false;
                        state->props.current_mode = SandboxMode::regular;
                        msg = "✓ Sandbox enabled with regular bash permissions";
                        break;
                    case 2: // disabled
                        state->props.sandbox_enabled = false;
                        state->props.auto_allow_bash = false;
                        state->props.current_mode = SandboxMode::disabled;
                        msg = "○ Sandbox disabled";
                        break;
                }
                if (state->props.on_complete) {
                    state->props.on_complete(msg, CommandResultDisplay::normal);
                }
                return true;
            }
        }

        // --- Overrides tab: item selection ---
        // TS REF: SandboxOverridesTab.tsx:119-126 (handleSelect)
        if (state->active_tab == SandboxTabId::Overrides &&
            state->props.sandbox_enabled &&
            !state->props.settings_locked_by_policy) {

            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->override_selected = std::max(0, state->override_selected - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->override_selected = std::min(1, state->override_selected + 1);
                return true;
            }
            if (event == Event::Return) {
                bool open = (state->override_selected == 0);
                state->props.allow_unsandboxed = open;
                std::string msg = open
                    ? "✓ Unsandboxed fallback allowed - commands can run "
                      "outside sandbox when necessary"
                    : "✓ Strict sandbox mode - all commands must run in "
                      "sandbox or be excluded via the excludedCommands option";
                if (state->props.on_complete) {
                    state->props.on_complete(msg, CommandResultDisplay::normal);
                }
                return true;
            }
        }

        // --- Escape to dismiss ---
        // TS REF: SandboxSettings.tsx:149-153 (confirm:no → onComplete skip)
        if (event == Event::Escape) {
            if (state->props.on_complete) {
                state->props.on_complete(std::nullopt, CommandResultDisplay::skip);
            }
            return true;
        }

        return false;
    });
}

// ============================================================
// Convenience: build default dep check by probing system
// ============================================================

/// Build a SandboxDepCheck by probing the local system for required tools.
/// TS REF: SandboxManager.checkDependencies() — simplified CPP version.
/// The real TS version does more thorough checks; this covers the
/// high-level items the UI needs to render.
[[nodiscard]] inline SandboxDepCheck ProbeDependencies() {
    SandboxDepCheck result;

    // Helper: check if a command exists on PATH
    auto has_cmd = [](std::string_view name) -> bool {
        std::string cmd = "command -v " + std::string(name) + " >/dev/null 2>&1";
        return std::system(cmd.c_str()) == 0;
    };

    // ripgrep — required on all platforms
    if (!has_cmd("rg")) {
        result.errors.push_back(
            {"ripgrep (rg) not found — required for directory scanning",
             DepSeverity::error});
    }

    if (!platform::is_macos()) {
        // Linux / BSD: bwrap + socat required, seccomp optional
        if (!has_cmd("bwrap")) {
            result.errors.push_back(
                {"bubblewrap (bwrap) not installed", DepSeverity::error});
        }
        if (!has_cmd("socat")) {
            result.errors.push_back(
                {"socat not installed", DepSeverity::error});
        }
        // seccomp — check via /proc (warning-level)
        // Simplified: if no seccomp-bpf path, add warning
        // (Real TS version checks sandbox.seccomp.bpfPath in settings)
    }

    return result;
}

/// Build a SandboxConfigSnapshot with reasonable defaults for display.
/// In production this would be populated from SandboxManager getters.
/// TS REF: SandboxConfigTab.tsx:29-35
[[nodiscard]] inline SandboxConfigSnapshot DefaultConfigSnapshot() {
    SandboxConfigSnapshot cfg;
    cfg.excluded_commands = {};
    cfg.fs_read = {};
    cfg.fs_write = {};
    cfg.network = {};
    cfg.allow_unix_sockets = {};
    cfg.glob_pattern_warnings = {};
    cfg.managed_domains_only = false;
    return cfg;
}

} // namespace cc::ui::dialogs::sandbox_settings
