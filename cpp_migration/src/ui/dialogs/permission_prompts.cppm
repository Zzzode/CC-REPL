/// @file permission_prompts.cppm
/// @brief File/command/tool permission request dialogs with FTXUI rendering.
/// Provides interactive permission prompts for file access, shell commands, and tool use.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.permission_prompts;

import cc.types.types;

export namespace cc::ui::dialogs::permission_prompts {
using namespace ftxui;

// ============================================================
// Permission Types
// ============================================================

/// Category of permission being requested
enum class PermissionCategory : std::uint8_t {
    FileRead,       // Reading a file
    FileWrite,      // Writing/modifying a file
    FileDelete,     // Deleting a file
    ShellCommand,   // Running a shell command
    ToolUse,        // Using an MCP tool
    NetworkAccess,  // Network access
};

/// Risk level for the permission request
enum class RiskLevel : std::uint8_t {
    Low,        // Safe, read-only operations
    Medium,     // Modifications with limited scope
    High,       // Destructive or broad-scope changes
    Critical,   // Irreversible or system-wide changes
};

/// User's response to a permission prompt
enum class PermissionResponse : std::uint8_t {
    Allow,          // Allow this invocation only
    AllowSession,   // Allow for this session
    AllowAlways,    // Always allow (persist)
    Deny,           // Deny this invocation
    DenyAlways,     // Never allow (persist)
    Abort,          // Abort the current operation
};

/// Data for a single permission request
struct PermissionPromptData {
    PermissionCategory category;
    RiskLevel risk;
    std::string tool_name;
    std::string description;
    std::string command_preview;    // The command/path being requested
    std::vector<std::string> affected_paths;
    std::optional<std::string> working_directory;
    std::optional<std::chrono::seconds> timeout;
};

/// Options for the permission prompts component
struct PermissionPromptsOptions {
    PermissionPromptData request;
    int selected_choice = 0;
    std::function<void(PermissionResponse)> on_respond;
    bool show_details = false;
    bool remember_choice = false;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get display string for permission category
[[nodiscard]] inline std::string category_label(PermissionCategory cat) {
    switch (cat) {
        case PermissionCategory::FileRead:      return "File Read";
        case PermissionCategory::FileWrite:     return "File Write";
        case PermissionCategory::FileDelete:    return "File Delete";
        case PermissionCategory::ShellCommand:  return "Shell Command";
        case PermissionCategory::ToolUse:       return "Tool Use";
        case PermissionCategory::NetworkAccess: return "Network Access";
    }
    return "Unknown";
}

/// Get icon for permission category
[[nodiscard]] inline std::string category_icon(PermissionCategory cat) {
    switch (cat) {
        case PermissionCategory::FileRead:      return "📖";
        case PermissionCategory::FileWrite:     return "✏️";
        case PermissionCategory::FileDelete:    return "🗑️";
        case PermissionCategory::ShellCommand:  return "⚡";
        case PermissionCategory::ToolUse:       return "🔧";
        case PermissionCategory::NetworkAccess: return "🌐";
    }
    return "❓";
}

/// Get color for risk level
[[nodiscard]] inline Color risk_color(RiskLevel risk) {
    switch (risk) {
        case RiskLevel::Low:      return Color::Green;
        case RiskLevel::Medium:   return Color::Yellow;
        case RiskLevel::High:     return Color::Red;
        case RiskLevel::Critical: return Color::RedLight;
    }
    return Color::White;
}

/// Get label for risk level
[[nodiscard]] inline std::string risk_label(RiskLevel risk) {
    switch (risk) {
        case RiskLevel::Low:      return "Low Risk";
        case RiskLevel::Medium:   return "Medium Risk";
        case RiskLevel::High:     return "High Risk";
        case RiskLevel::Critical: return "CRITICAL";
    }
    return "Unknown";
}

// ============================================================
// Permission Prompt Rendering
// ============================================================

/// Render the main permission prompt element
[[nodiscard]] inline Element RenderPermissionPrompt(const PermissionPromptsOptions& opts) {
    const auto& req = opts.request;

    // Title bar with category icon and risk badge
    auto title = hbox({
        text(category_icon(req.category) + " ") | bold,
        text("Permission Required: ") | color(Color::Yellow) | bold,
        text(req.tool_name) | color(Color::Magenta) | bold,
        filler(),
        text(" " + risk_label(req.risk) + " ") | color(Color::White)
            | bgcolor(risk_color(req.risk)),
    });

    // Category label
    auto category_line = hbox({
        text("  Category: ") | color(Color::GrayLight),
        text(category_label(req.category)) | color(Color::Cyan),
    });

    // Description
    auto desc_line = hbox({
        text("  ") | dim,
        paragraph(req.description) | color(Color::White),
    });

    // Command/path preview
    auto preview_line = hbox({
        text("  Command:  ") | color(Color::GrayLight),
        text(req.command_preview) | color(Color::Yellow) | bold,
    });

    Elements elements = {title, separator(), category_line, desc_line, preview_line};

    // Working directory if present
    if (req.working_directory) {
        elements.push_back(hbox({
            text("  CWD:      ") | color(Color::GrayLight),
            text(*req.working_directory) | color(Color::Cyan) | dim,
        }));
    }

    // Affected paths
    if (!req.affected_paths.empty()) {
        elements.push_back(text(""));
        elements.push_back(text("  Affected paths:") | bold | color(Color::White));
        int shown = 0;
        for (const auto& path : req.affected_paths) {
            if (shown >= 8) {
                elements.push_back(text(std::format("    ... and {} more",
                    req.affected_paths.size() - 8)) | dim);
                break;
            }
            elements.push_back(hbox({
                text("    ") | dim,
                text("• " + path) | color(Color::Cyan),
            }));
            ++shown;
        }
    }

    // Separator before choices
    elements.push_back(text(""));
    elements.push_back(separator());

    // Choice options
    struct ChoiceItem {
        std::string key;
        std::string label;
        PermissionResponse response;
    };
    std::vector<ChoiceItem> choices = {
        {"y", "Allow (this time)",    PermissionResponse::Allow},
        {"s", "Allow (session)",      PermissionResponse::AllowSession},
        {"a", "Always allow",         PermissionResponse::AllowAlways},
        {"n", "Deny",                 PermissionResponse::Deny},
        {"d", "Never allow",          PermissionResponse::DenyAlways},
        {"x", "Abort operation",      PermissionResponse::Abort},
    };

    for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
        auto& c = choices[i];
        auto line = hbox({
            text("  [") | dim,
            text(c.key) | color(Color::Cyan) | bold,
            text("] ") | dim,
            text(c.label) | (i == opts.selected_choice ? bold : nothing),
        });
        if (i == opts.selected_choice) {
            line = line | inverted;
        }
        elements.push_back(line);
    }

    // Border style based on risk
    auto border_col = risk_color(req.risk);
    return vbox(elements) | borderDouble | color(border_col);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive permission prompt component
[[nodiscard]] inline Component PermissionPrompt(PermissionPromptsOptions options) {
    constexpr size_t kChoiceCount = 6;
    struct State {
        PermissionPromptsOptions opts;
        int selected = 0;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(options);
    state->selected = state->opts.selected_choice;

    return Renderer([state] {
        state->opts.selected_choice = state->selected;
        return RenderPermissionPrompt(state->opts);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = (state->selected - 1 + kChoiceCount) % kChoiceCount;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected = (state->selected + 1) % kChoiceCount;
            return true;
        }
        if (event == Event::Return) {
            static constexpr PermissionResponse responses[] = {
                PermissionResponse::Allow,
                PermissionResponse::AllowSession,
                PermissionResponse::AllowAlways,
                PermissionResponse::Deny,
                PermissionResponse::DenyAlways,
                PermissionResponse::Abort,
            };
            if (state->opts.on_respond) {
                state->opts.on_respond(responses[state->selected]);
            }
            return true;
        }
        // Quick key shortcuts
        if (event == Event::Character('y')) {
            if (state->opts.on_respond) state->opts.on_respond(PermissionResponse::Allow);
            return true;
        }
        if (event == Event::Character('s')) {
            if (state->opts.on_respond) state->opts.on_respond(PermissionResponse::AllowSession);
            return true;
        }
        if (event == Event::Character('a')) {
            if (state->opts.on_respond) state->opts.on_respond(PermissionResponse::AllowAlways);
            return true;
        }
        if (event == Event::Character('n')) {
            if (state->opts.on_respond) state->opts.on_respond(PermissionResponse::Deny);
            return true;
        }
        if (event == Event::Character('d')) {
            if (state->opts.on_respond) state->opts.on_respond(PermissionResponse::DenyAlways);
            return true;
        }
        if (event == Event::Character('x') || event == Event::Escape) {
            if (state->opts.on_respond) state->opts.on_respond(PermissionResponse::Abort);
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::dialogs::permission_prompts
