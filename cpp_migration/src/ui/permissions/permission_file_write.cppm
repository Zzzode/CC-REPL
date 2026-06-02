/// @file permission_file_write.cppm
/// @brief File write permission UI for new file creation
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <format>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_file_write;

import cc.types.types;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Write permission details
struct FileWritePermissionOptions {
    std::string file_path;
    std::size_t content_size{0};
    bool is_binary{false};
    bool outside_project{false};
    std::optional<std::string> content_preview;
};

/// Render file write permission request
[[nodiscard]] inline Element render_file_write_permission(const FileWritePermissionOptions& opts) {
    std::vector<Element> elements;

    elements.push_back(hbox({
        text("WRITE") | bold | color(Color::Green),
        text(" "),
        text(opts.file_path),
    }));

    elements.push_back(text(std::format("  Size: {} bytes{}", opts.content_size,
        opts.is_binary ? " (binary)" : "")) | dim);

    if (opts.outside_project) {
        elements.push_back(text("  WARNING: Outside project directory") | color(Color::Yellow));
    }

    if (opts.content_preview) {
        elements.push_back(separator());
        elements.push_back(text(*opts.content_preview) | dim);
    }

    return vbox(elements);
}

/// Create file write permission component
[[nodiscard]] inline Component file_write_permission_dialog(
    const FileWritePermissionOptions& opts,
    std::function<void(bool)> on_decision) {
    return Renderer([opts, on_decision = std::move(on_decision)] {
        return vbox({
            text("Allow file write?") | bold,
            separator(),
            render_file_write_permission(opts),
            separator(),
            hbox({text("[Y]es") | bold, text(" / "), text("[N]o") | bold}),
        }) | border;
    });
}

} // namespace cc::ui::permissions
