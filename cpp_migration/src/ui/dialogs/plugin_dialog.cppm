/// @file plugin_dialog.cppm
/// @brief Plugin management interface - displays installed plugins/skills,
/// their status, and allows enable/disable/configure operations.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.plugin_dialog;

import cc.types.types;

export namespace cc::ui::dialogs::plugin_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Plugin installation source
enum class PluginSource : std::uint8_t {
    Builtin,    // Shipped with the application
    User,       // User-installed (local)
    Registry,   // From plugin registry
    Git,        // From git repository
};

/// Plugin status
enum class PluginStatus : std::uint8_t {
    Active,     // Running and functional
    Disabled,   // Installed but disabled
    Error,      // Failed to load/init
    Updating,   // Currently being updated
    NotInstalled, // Available but not installed
};

/// A single plugin entry
struct PluginEntry {
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    std::string author;
    PluginSource source;
    PluginStatus status;
    std::optional<std::string> error_message;
    std::vector<std::string> provided_tools;
    std::vector<std::string> provided_commands;
    bool has_update = false;
    std::optional<std::string> new_version;
};

/// Options for the plugin dialog
struct PluginDialogOptions {
    std::vector<PluginEntry> plugins;
    int selected_index = 0;
    std::string filter_text;

    std::function<void(const std::string& plugin_id)> on_enable;
    std::function<void(const std::string& plugin_id)> on_disable;
    std::function<void(const std::string& plugin_id)> on_install;
    std::function<void(const std::string& plugin_id)> on_uninstall;
    std::function<void(const std::string& plugin_id)> on_update;
    std::function<void(const std::string& plugin_id)> on_configure;
    std::function<void()> on_close;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get status display icon and color
[[nodiscard]] inline std::pair<std::string, Color> status_display(PluginStatus s) {
    switch (s) {
        case PluginStatus::Active:       return {"●", Color::Green};
        case PluginStatus::Disabled:     return {"○", Color::GrayDark};
        case PluginStatus::Error:        return {"✗", Color::Red};
        case PluginStatus::Updating:     return {"⟳", Color::Yellow};
        case PluginStatus::NotInstalled: return {"◇", Color::Cyan};
    }
    return {"?", Color::White};
}

/// Get source badge
[[nodiscard]] inline std::pair<std::string, Color> source_badge(PluginSource src) {
    switch (src) {
        case PluginSource::Builtin:  return {"built-in", Color::Green};
        case PluginSource::User:     return {"local", Color::Blue};
        case PluginSource::Registry: return {"registry", Color::Cyan};
        case PluginSource::Git:      return {"git", Color::Magenta};
    }
    return {"?", Color::White};
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single plugin list item
[[nodiscard]] inline Element RenderPluginItem(
    const PluginEntry& plugin, bool selected) {

    auto [icon, icon_color] = status_display(plugin.status);
    auto [src_text, src_color] = source_badge(plugin.source);

    Elements parts = {
        text(" " + icon + " ") | color(icon_color),
        text(plugin.name) | bold | color(selected ? Color::White : Color::GrayLight),
    };

    if (plugin.has_update) {
        parts.push_back(text(" ⬆") | color(Color::Yellow));
    }

    parts.push_back(filler());
    parts.push_back(text("v" + plugin.version) | dim);
    parts.push_back(text(" "));
    parts.push_back(text("[" + src_text + "]") | dim | color(src_color));
    parts.push_back(text(" "));

    auto line = hbox(parts);
    if (selected) {
        line = line | bgcolor(Color::RGB(30, 40, 60));
    }
    return line;
}

/// Render plugin detail panel
[[nodiscard]] inline Element RenderPluginDetail(const PluginEntry& plugin) {
    Elements elements;

    auto [icon, icon_color] = status_display(plugin.status);
    elements.push_back(hbox({
        text(" " + plugin.name + " ") | bold,
        text(icon + " ") | color(icon_color),
        text("v" + plugin.version) | dim,
    }));
    elements.push_back(separator());

    // Description
    elements.push_back(text(""));
    elements.push_back(paragraph(" " + plugin.description) | dim);
    elements.push_back(text(""));

    // Metadata
    if (!plugin.author.empty()) {
        elements.push_back(hbox({
            text("  Author:  ") | dim,
            text(plugin.author) | color(Color::Cyan),
        }));
    }
    elements.push_back(hbox({
        text("  ID:      ") | dim,
        text(plugin.id) | color(Color::GrayLight),
    }));

    // Error
    if (plugin.error_message) {
        elements.push_back(text(""));
        elements.push_back(hbox({
            text("  ✗ Error: ") | color(Color::Red),
            text(*plugin.error_message) | color(Color::Red) | dim,
        }));
    }

    // Update available
    if (plugin.has_update && plugin.new_version) {
        elements.push_back(text(""));
        elements.push_back(hbox({
            text("  ⬆ Update available: ") | color(Color::Yellow),
            text("v" + *plugin.new_version) | color(Color::Yellow) | bold,
        }));
    }

    // Provided tools
    if (!plugin.provided_tools.empty()) {
        elements.push_back(text(""));
        elements.push_back(text(std::format("  Tools ({})", plugin.provided_tools.size())) | bold);
        for (const auto& tool : plugin.provided_tools) {
            elements.push_back(hbox({
                text("    🔧 ") | dim,
                text(tool) | color(Color::Magenta),
            }));
        }
    }

    // Provided commands
    if (!plugin.provided_commands.empty()) {
        elements.push_back(text(""));
        elements.push_back(text(std::format("  Commands ({})", plugin.provided_commands.size())) | bold);
        for (const auto& cmd : plugin.provided_commands) {
            elements.push_back(hbox({
                text("    ⌘ ") | dim,
                text(cmd) | color(Color::Cyan),
            }));
        }
    }

    return vbox(elements) | border;
}

/// Render the full plugin dialog
[[nodiscard]] inline Element RenderPluginDialog(const PluginDialogOptions& opts) {
    // Title
    auto title = hbox({
        text(" 🧩 Plugin Manager ") | bold | color(Color::Magenta),
        filler(),
        text(std::format("{} plugins", opts.plugins.size())) | dim,
        text(" "),
    });

    // Plugin list
    Elements list_items;
    for (int i = 0; i < static_cast<int>(opts.plugins.size()); ++i) {
        list_items.push_back(RenderPluginItem(opts.plugins[i], i == opts.selected_index));
    }

    auto list_panel = vbox(list_items) | vscroll_indicator | yframe
                      | size(HEIGHT, LESS_THAN, 15) | border;

    // Detail panel
    Element detail_panel;
    if (!opts.plugins.empty() && opts.selected_index < static_cast<int>(opts.plugins.size())) {
        detail_panel = RenderPluginDetail(opts.plugins[opts.selected_index]);
    } else {
        detail_panel = text(" No plugin selected") | dim | center | border;
    }

    // Action bar
    auto actions = hbox({
        text(" [e]") | color(Color::Cyan), text("nable "),
        text("[d]") | color(Color::Cyan), text("isable "),
        text("[u]") | color(Color::Cyan), text("pdate "),
        text("[c]") | color(Color::Cyan), text("onfigure "),
        text("[x]") | color(Color::Cyan), text(" uninstall "),
        text("[Esc]") | color(Color::Cyan), text(" close"),
    }) | dim;

    return vbox({
        title,
        separator(),
        hbox({
            list_panel | size(WIDTH, EQUAL, 45),
            detail_panel | flex,
        }) | flex,
        separator(),
        actions,
    }) | borderDouble | bgcolor(Color::RGB(15, 15, 25));
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the plugin dialog component
[[nodiscard]] inline Component PluginDialog(PluginDialogOptions options) {
    auto state = std::make_shared<PluginDialogOptions>(std::move(options));

    return Renderer([state] {
        return RenderPluginDialog(*state);
    }) | CatchEvent([state](Event event) -> bool {
        int count = static_cast<int>(state->plugins.size());

        if (event == Event::Escape) {
            if (state->on_close) state->on_close();
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index = std::max(0, state->selected_index - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index = std::min(count - 1, state->selected_index + 1);
            return true;
        }

        if (count == 0) return false;
        const auto& plugin = state->plugins[state->selected_index];

        if (event == Event::Character('e')) {
            if (state->on_enable) state->on_enable(plugin.id);
            return true;
        }
        if (event == Event::Character('d')) {
            if (state->on_disable) state->on_disable(plugin.id);
            return true;
        }
        if (event == Event::Character('u')) {
            if (state->on_update) state->on_update(plugin.id);
            return true;
        }
        if (event == Event::Character('c')) {
            if (state->on_configure) state->on_configure(plugin.id);
            return true;
        }
        if (event == Event::Character('x')) {
            if (state->on_uninstall) state->on_uninstall(plugin.id);
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::dialogs::plugin_dialog
