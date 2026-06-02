/// @file permission_file_edit.cppm
/// @brief File edit permission request UI with diff preview
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_file_edit;

import cc.types.types;

export namespace cc::ui::permissions {

using namespace ftxui;

/// File edit operation type
enum class FileEditOp {
    Create,
    Modify,
    Delete,
    Rename,
};

/// File edit permission display options
struct FileEditPermissionOptions {
    std::string file_path;
    FileEditOp operation{FileEditOp::Modify};
    std::optional<std::string> diff_preview;
    std::optional<std::size_t> lines_added;
    std::optional<std::size_t> lines_removed;
    bool is_new_file{false};
};

/// Get operation label
[[nodiscard]] inline std::string_view op_label(FileEditOp op) {
    switch (op) {
        case FileEditOp::Create: return "CREATE";
        case FileEditOp::Modify: return "MODIFY";
        case FileEditOp::Delete: return "DELETE";
        case FileEditOp::Rename: return "RENAME";
    }
    return "UNKNOWN";
}

/// Render file edit permission request
[[nodiscard]] inline Element render_file_edit_permission(const FileEditPermissionOptions& opts) {
    auto op_color = [&]() -> Color {
        switch (opts.operation) {
            case FileEditOp::Create: return Color::Green;
            case FileEditOp::Modify: return Color::Yellow;
            case FileEditOp::Delete: return Color::Red;
            case FileEditOp::Rename: return Color::Cyan;
        }
        return Color::White;
    }();

    std::vector<Element> elements;
    elements.push_back(hbox({
        text(std::string(op_label(opts.operation))) | bold | color(op_color),
        text(" "),
        text(opts.file_path),
    }));

    if (opts.lines_added || opts.lines_removed) {
        std::string stats;
        if (opts.lines_added) stats += std::format("+{}", *opts.lines_added);
        if (opts.lines_removed) stats += std::format(" -{}", *opts.lines_removed);
        elements.push_back(text("  " + stats) | dim);
    }

    if (opts.diff_preview) {
        elements.push_back(separator());
        elements.push_back(text(*opts.diff_preview) | dim);
    }

    return vbox(elements);
}

/// Create file edit permission component
[[nodiscard]] inline Component file_edit_permission_dialog(
    const FileEditPermissionOptions& opts,
    std::function<void(bool)> on_decision) {
    return Renderer([opts, on_decision = std::move(on_decision)] {
        return vbox({
            text("Allow file edit?") | bold,
            separator(),
            render_file_edit_permission(opts),
            separator(),
            hbox({text("[Y]es") | bold, text(" / "), text("[N]o") | bold}),
        }) | border;
    });
}

} // namespace cc::ui::permissions
