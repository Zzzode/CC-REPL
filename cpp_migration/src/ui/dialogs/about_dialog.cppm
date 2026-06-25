/// @file about_dialog.cppm
/// @brief Faithful About dialog — port of TS About.tsx / Settings About tab.
///        Shows version info, build info, credits, and links.
///
/// MODULE:   cc.ui.dialogs.about
/// LICENCE:  Exported.  Imported by settings dialog (About tab) and
///           as a standalone dialog.
///
/// TS REFERENCE:
///   src/components/Settings/About.tsx
///
/// VISUAL STRUCTURE (faithful to TS):
///   ┌─ About Claude Code ──────────────────────────────┐
///   │  CC-REPL v0.0.0 — C++ native port                 │
///   ├───────────────────────────────────────────────────┤
///   │                                                   │
///   │   .--_ /\ _.-.                                   │
///   │   \      /    \      Claude Code                  │
///   │    \    /      \     C++ native port              │
///   │     \  /       /                                  │
///   │      \/_     _/                                   │
///   │                                                   │
///   │  Version      1.0.0                               │
///   │  Build        2026-06-20                          │
///   │  Runtime      C++23 modules                       │
///   │  Framework    FTXUI + libuv                       │
///   │                                                   │
///   │  Website  https://code.claude.com                 │
///   │  Docs     https://code.claude.com/docs            │
///   │                                                   │
///   ├───────────────────────────────────────────────────┤
///   │  Made with ♥ by the Anthropics                        │
///   │  esc to close                                     │
///   └───────────────────────────────────────────────────┘
///
/// KEYBOARD:
///   Esc  — close dialog
module;

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.about;

import cc.constants.product;
import cc.ui.dialogs.frame;
import cc.ui.design.theme;
import cc.ui.design.tokens;

export namespace cc::ui::dialogs::about {

using namespace ftxui;
namespace dframe = cc::ui::dialogs::frame;
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;

// ============================================================
// Types
// ============================================================

/// About dialog properties.
struct AboutDialogProps {
    std::string app_name = "Claude Code";
    std::string app_version = "0.0.0";
    std::string build_date = std::string(cc::constants::product::BUILD_DATE);
    std::string build_time = std::string(cc::constants::product::BUILD_TIME);
    std::string runtime = "C++23 modules";
    std::string framework = "FTXUI + libuv";
    std::string website = "https://code.claude.com";
    std::string docs_url = "https://code.claude.com/docs";
    std::string license = "MIT";
    /// Called when dialog is dismissed.
    std::function<void()> on_close;
};

// ============================================================
// Helpers
// ============================================================

namespace detail {

/// Render a simple info row: label + value (two-column layout).
[[nodiscard]] inline Element InfoRow(const std::string& label,
                                       const std::string& value,
                                       const Theme& theme) {
    return hbox({
        text("  " + label) | dim | size(WIDTH, EQUAL, 14),
        text(value) | color(theme.color_for(Role::Info)),
        filler(),
    });
}

/// Render a link row.
[[nodiscard]] inline Element LinkRow(const std::string& label,
                                       const std::string& url,
                                       const Theme& theme) {
    return hbox({
        text("  " + label) | dim | size(WIDTH, EQUAL, 14),
        text(url) | color(theme.color_for(Role::Info)) | underlined,
        filler(),
    });
}

/// Render the Claude logo (ASCII art — simplified).
[[nodiscard]] inline Element RenderLogo(const Theme& theme) {
    // Simplified ASCII clawd logo
    Elements logo_lines = {
        text("    .--_ /\\ _.-.    ") | color(theme.color_for(Role::Info)),
        text("    \\      /    \\   ") | color(theme.color_for(Role::Info)),
        text("     \\    /      \\  ") | color(theme.color_for(Role::Info)),
        text("      \\  /       /  ") | color(theme.color_for(Role::Info)),
        text("       \\/_     _/   ") | color(theme.color_for(Role::Info)),
    };
    return vbox(std::move(logo_lines)) | center;
}

/// Render the credit / footer text.
[[nodiscard]] inline Element RenderCredits(const Theme&) {
    return vbox({
        text(""),
        hbox({
            text("  Made with ") | dim,
            text("♥") | color(Color::Red),
            text(" by the Anthropic team") | dim,
            filler(),
        }),
        hbox({
            text("  "),
            text("esc") | dim,
            text(" to close") | dim,
            filler(),
        }),
    });
}

} // namespace detail

// ============================================================
// Main About dialog renderer (pure Element version)
// ============================================================

/// Render the about dialog (read-only version — no interaction).
/// Faithful to TS Settings/About.tsx visual structure.
[[nodiscard]] inline Element RenderAboutDialog(
    const AboutDialogProps& props,
    const Theme& theme)
{
    using namespace detail;

    Elements content_els;

    // Logo + title block
    content_els.push_back(hbox({
        RenderLogo(theme),
        text("  "),
        vbox({
            text(props.app_name) | bold | color(theme.color_for(Role::Info)),
            text(" v" + props.app_version) | dim,
            text(" C++ native port") | dim,
        }),
        filler(),
    }));

    content_els.push_back(text(""));
    content_els.push_back(text(" Info") | bold);

    // Info rows
    content_els.push_back(InfoRow("Version", props.app_version, theme));
    content_els.push_back(InfoRow("Build", props.build_date + " " + props.build_time, theme));
    content_els.push_back(InfoRow("Runtime", props.runtime, theme));
    content_els.push_back(InfoRow("Framework", props.framework, theme));
    content_els.push_back(InfoRow("License", props.license, theme));

    content_els.push_back(text(""));
    content_els.push_back(text(" Links") | bold);

    // Links
    content_els.push_back(LinkRow("Website", props.website, theme));
    content_els.push_back(LinkRow("Docs", props.docs_url, theme));

    // Credits
    content_els.push_back(RenderCredits(theme));

    auto content = vbox(std::move(content_els));

    // Wrap in DialogFrame
    dframe::DialogFrameProps frame_props;
    frame_props.title = "About " + props.app_name;
    frame_props.subtitle = "Version " + props.app_version;
    frame_props.style = dframe::FrameStyle::Info;
    frame_props.content = content;
    frame_props.full_border = true;
    frame_props.inner_padding_x = 1;
    frame_props.inner_padding_y = 0;

    return dframe::DialogFrame(frame_props, theme);
}

// ============================================================
// Interactive AboutDialog Component
// ============================================================

/// Create an interactive AboutDialog component.
/// Faithful to TS About tab behavior.
[[nodiscard]] inline Component MakeAboutDialog(
    AboutDialogProps props,
    const Theme& theme)
{
    auto renderer = Renderer([props = std::move(props), &theme]() -> Element {
        return RenderAboutDialog(props, theme);
    });

    auto with_events = CatchEvent([props = std::move(props)](Event event) -> bool {
        if (event == Event::Escape) {
            if (props.on_close) props.on_close();
            return true;
        }
        if (event == Event::Return) {
            if (props.on_close) props.on_close();
            return true;
        }
        return false;
    });

    return renderer | with_events;
}

} // namespace cc::ui::dialogs::about
