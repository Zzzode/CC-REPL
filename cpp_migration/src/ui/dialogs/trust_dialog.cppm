module;
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <expected>
#include <algorithm>
#include <cstdint>
#include <sstream>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.trust_dialog;

export namespace cc::ui::trust_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Trust decision made by the user
enum class TrustDecision {
    trust,   // "Yes, I trust this folder"
    exit,    // "No, exit"
};

/// Sources of potential security concern detected in workspace
struct SecuritySources {
    bool has_mcp_servers = false;
    bool has_hooks = false;
    bool has_bash_execution = false;
    bool has_api_key_helper = false;
    bool has_aws_commands = false;
    bool has_gcp_commands = false;
    bool has_otel_headers_helper = false;
    bool has_dangerous_env_vars = false;
};

/// Props for the trust dialog component
struct TrustDialogProps {
    std::function<void()> on_done;
    std::string workspace_path;
    SecuritySources sources;
    std::vector<std::string> commands;
};

/// Selection option for the dialog
struct SelectOption {
    std::string label;
    TrustDecision value;
};

// ============================================================
// Detection Helpers
// ============================================================

/// Check if there are any security concerns to show
[[nodiscard]] inline bool has_any_concerns(const SecuritySources& src) {
    return src.has_mcp_servers || src.has_hooks || src.has_bash_execution
        || src.has_api_key_helper || src.has_aws_commands
        || src.has_gcp_commands || src.has_otel_headers_helper
        || src.has_dangerous_env_vars;
}

/// Get the list of concerns as displayable strings
[[nodiscard]] inline std::vector<std::string> list_concerns(
    const SecuritySources& src) {
    std::vector<std::string> concerns;
    if (src.has_mcp_servers) concerns.emplace_back("MCP servers configured");
    if (src.has_hooks) concerns.emplace_back("Hooks defined");
    if (src.has_bash_execution) concerns.emplace_back("Bash command execution");
    if (src.has_api_key_helper) concerns.emplace_back("API key helper commands");
    if (src.has_aws_commands) concerns.emplace_back("AWS CLI commands");
    if (src.has_gcp_commands) concerns.emplace_back("GCP CLI commands");
    if (src.has_otel_headers_helper) concerns.emplace_back("OTEL headers helper");
    if (src.has_dangerous_env_vars) concerns.emplace_back("Dangerous environment variables");
    return concerns;
}

// ============================================================
// Element Rendering
// ============================================================

/// Render the workspace path line
[[nodiscard]] inline Element RenderWorkspacePath(std::string_view path) {
    return hbox({
        text("Accessing workspace: ") | dim,
        text(std::string{path}) | bold,
    });
}

/// Render the safety explanation text
[[nodiscard]] inline Element RenderSafetyText() {
    return vbox({
        paragraph("Quick safety check: Is this a project you created or "
                  "one you trust? (Like your own code, a well-known open "
                  "source project, or work from your team). If not, take "
                  "a moment to review what's in this folder first."),
        text(""),
        paragraph("Claude Code'll be able to read, edit, and execute "
                  "files here.") | dim,
    });
}

/// Render the security concerns section
[[nodiscard]] inline Element RenderConcerns(const SecuritySources& src) {
    auto concerns = list_concerns(src);
    if (concerns.empty()) return text("");

    Elements items;
    items.push_back(text(" Detected:") | bold | color(Color::Yellow));
    for (const auto& concern : concerns) {
        items.push_back(hbox({
            text("  \u2022 ") | color(Color::Yellow),
            text(concern),
        }));
    }
    return vbox(items);
}

/// Render the selection options
[[nodiscard]] inline Element RenderOptions(int selected) {
    auto make_option = [](std::string_view label, bool is_selected) {
        auto prefix = is_selected
            ? text("\u276F ") | color(Color::Cyan)
            : text("  ");
        auto label_el = text(std::string{label});
        if (is_selected) label_el = label_el | bold;
        return hbox({prefix, label_el});
    };

    return vbox({
        make_option("Yes, I trust this folder", selected == 0),
        make_option("No, exit", selected == 1),
    });
}

/// Render the full trust dialog element
[[nodiscard]] inline Element RenderTrustDialog(
    std::string_view workspace_path,
    const SecuritySources& sources,
    int selected_option) {

    auto content = vbox({
        RenderWorkspacePath(workspace_path),
        text(""),
        RenderSafetyText(),
        text(""),
        RenderConcerns(sources),
        text(""),
        RenderOptions(selected_option),
        text(""),
        hbox({
            text("Security guide") | color(Color::Cyan) | underlined,
        }) | dim,
        text(""),
        hbox({
            text("Enter") | bold,
            text(" to confirm \u00B7 "),
            text("Esc") | bold,
            text(" to cancel"),
        }) | dim,
    });

    return window(
        text(" \u26A0\uFE0F  Accessing workspace ") | color(Color::Yellow) | bold,
        content | xflex
    ) | color(Color::Yellow) | size(WIDTH, LESS_THAN, 80);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the trust dialog interactive component
[[nodiscard]] inline Component TrustDialogComponent(TrustDialogProps props) {
    struct State {
        TrustDialogProps props;
        int selected = 0;
    };
    constexpr int kOptionCount = 2;

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderTrustDialog(
            state->props.workspace_path,
            state->props.sources,
            state->selected);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = std::max(0, state->selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected = std::min(kOptionCount - 1,
                                       state->selected + 1);
            return true;
        }
        if (event == Event::Return) {
            if (state->selected == 0) {
                // Trust accepted
                if (state->props.on_done) state->props.on_done();
            } else {
                // Exit requested — caller handles process exit
            }
            return true;
        }
        if (event == Event::Escape) {
            // Treat as exit
            return true;
        }
        return false;
    });
}

/// Create trust dialog with pre-configured workspace path (overload)
[[nodiscard]] inline Component TrustDialog(
    std::string workspace_path,
    std::function<void()> on_done) {

    TrustDialogProps props;
    props.workspace_path = std::move(workspace_path);
    props.on_done = std::move(on_done);
    return TrustDialogComponent(std::move(props));
}

/// Create trust dialog with full security source detection (overload)
[[nodiscard]] inline Component TrustDialog(
    std::string workspace_path,
    SecuritySources sources,
    std::function<void()> on_done) {

    TrustDialogProps props;
    props.workspace_path = std::move(workspace_path);
    props.sources = sources;
    props.on_done = std::move(on_done);
    return TrustDialogComponent(std::move(props));
}

} // namespace cc::ui::trust_dialog
