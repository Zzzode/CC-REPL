/// @file file_tree.cppm
/// @brief File tree view component - displays a hierarchical file/directory tree
/// with expand/collapse, icons, filtering, and selection support.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.file_tree;

import cc.types.types;

export namespace cc::ui::components::file_tree {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Type of tree node
enum class NodeType : std::uint8_t {
    File,
    Directory,
    Symlink,
    Unknown,
};

/// Git status of a file
enum class GitStatus : std::uint8_t {
    Unmodified,
    Modified,
    Added,
    Deleted,
    Renamed,
    Untracked,
    Ignored,
    Conflict,
};

/// A node in the file tree
struct TreeNode {
    std::string name;
    std::string full_path;
    NodeType type;
    GitStatus git_status = GitStatus::Unmodified;
    bool is_expanded = false;
    bool is_selected = false;
    int depth = 0;
    std::optional<std::size_t> file_size;
    std::vector<TreeNode> children;
};

/// Options for the file tree component
struct FileTreeOptions {
    std::vector<TreeNode> roots;    // Root nodes (could be multiple)
    int selected_index = 0;
    int scroll_offset = 0;
    bool show_hidden = false;
    bool show_git_status = true;
    bool show_file_size = false;
    bool multi_select = false;
    std::string filter;             // Filter pattern

    std::function<void(const TreeNode& node)> on_select;    // Single click
    std::function<void(const TreeNode& node)> on_open;      // Double click / Enter
    std::function<void(const TreeNode& node)> on_toggle;    // Expand/collapse
    std::function<void(std::vector<std::string> paths)> on_multi_select;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get icon for file type based on extension
[[nodiscard]] inline std::string file_icon(const std::string& name, NodeType type) {
    if (type == NodeType::Directory) return "📁";
    if (type == NodeType::Symlink) return "🔗";

    // Extension-based icons
    auto dot_pos = name.rfind('.');
    if (dot_pos != std::string::npos) {
        auto ext = name.substr(dot_pos + 1);
        if (ext == "cpp" || ext == "cxx" || ext == "cc") return "⚙️";
        if (ext == "cppm") return "📦";
        if (ext == "hpp" || ext == "h") return "📋";
        if (ext == "ts" || ext == "tsx") return "🟦";
        if (ext == "js" || ext == "jsx") return "🟨";
        if (ext == "py") return "🐍";
        if (ext == "rs") return "🦀";
        if (ext == "go") return "🔵";
        if (ext == "md") return "📝";
        if (ext == "json") return "📄";
        if (ext == "toml" || ext == "yaml" || ext == "yml") return "⚙️";
        if (ext == "sh" || ext == "bash") return "💻";
        if (ext == "txt") return "📃";
        if (ext == "lock") return "🔒";
    }

    // Special filenames
    if (name == "CMakeLists.txt") return "🔧";
    if (name == "Makefile") return "🔧";
    if (name == "Dockerfile") return "🐳";
    if (name == ".gitignore") return "🚫";

    return "📄";
}

/// Get git status indicator
[[nodiscard]] inline std::pair<std::string, Color> git_indicator(GitStatus status) {
    switch (status) {
        case GitStatus::Modified:   return {"M", Color::Yellow};
        case GitStatus::Added:      return {"A", Color::Green};
        case GitStatus::Deleted:    return {"D", Color::Red};
        case GitStatus::Renamed:    return {"R", Color::Cyan};
        case GitStatus::Untracked:  return {"?", Color::GrayLight};
        case GitStatus::Conflict:   return {"!", Color::Red};
        case GitStatus::Ignored:    return {"I", Color::GrayDark};
        default:                    return {" ", Color::GrayDark};
    }
}

/// Format file size
[[nodiscard]] inline std::string format_size(std::size_t bytes) {
    if (bytes < 1024) return std::format("{}B", bytes);
    if (bytes < 1024 * 1024) return std::format("{:.1f}K", bytes / 1024.0);
    if (bytes < 1024 * 1024 * 1024) return std::format("{:.1f}M", bytes / (1024.0 * 1024));
    return std::format("{:.1f}G", bytes / (1024.0 * 1024 * 1024));
}

/// Tree branch characters
[[nodiscard]] inline std::string tree_branch(int depth, bool is_last) {
    if (depth == 0) return "";
    std::string result;
    for (int i = 0; i < depth - 1; ++i) {
        result += "│  ";
    }
    result += is_last ? "└─ " : "├─ ";
    return result;
}

// ============================================================
// Flatten tree for display
// ============================================================

struct FlatNode {
    const TreeNode* node;
    int display_depth;
    bool is_last;       // Last child at its level
};

inline void flatten_tree(
    const std::vector<TreeNode>& nodes,
    std::vector<FlatNode>& out,
    int depth = 0,
    bool filter_active = false,
    const std::string& filter = "") {

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        const auto& node = nodes[i];
        bool is_last = (i == static_cast<int>(nodes.size()) - 1);

        // Apply filter
        if (filter_active && !filter.empty()) {
            // Simple case-insensitive contains
            std::string name_lower = node.name;
            std::string filter_lower = filter;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                [](unsigned char c) { return std::tolower(c); });
            std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(),
                [](unsigned char c) { return std::tolower(c); });

            bool matches = name_lower.find(filter_lower) != std::string::npos;
            bool child_matches = false;

            // Check if any child matches
            if (node.type == NodeType::Directory && !node.children.empty()) {
                std::vector<FlatNode> temp;
                flatten_tree(node.children, temp, depth + 1, true, filter);
                child_matches = !temp.empty();
            }

            if (!matches && !child_matches) continue;
        }

        out.push_back({&node, depth, is_last});

        // Recurse into expanded directories
        if (node.type == NodeType::Directory && node.is_expanded && !node.children.empty()) {
            flatten_tree(node.children, out, depth + 1, filter_active, filter);
        }
    }
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single tree node line
[[nodiscard]] inline Element RenderTreeLine(
    const FlatNode& flat, bool selected, const FileTreeOptions& opts) {

    const auto& node = *flat.node;
    Elements parts;

    // Tree structure lines
    parts.push_back(text(tree_branch(flat.display_depth, flat.is_last))
                    | dim | color(Color::GrayDark));

    // Expand/collapse indicator for directories
    if (node.type == NodeType::Directory) {
        parts.push_back(text(node.is_expanded ? "▾ " : "▸ ")
                        | color(Color::GrayLight));
    } else {
        parts.push_back(text("  "));
    }

    // Icon
    parts.push_back(text(file_icon(node.name, node.type) + " ") | dim);

    // Name
    Color name_color = Color::White;
    if (node.type == NodeType::Directory) name_color = Color::Blue;
    if (node.git_status == GitStatus::Modified) name_color = Color::Yellow;
    if (node.git_status == GitStatus::Added) name_color = Color::Green;
    if (node.git_status == GitStatus::Deleted) name_color = Color::Red;

    parts.push_back(text(node.name) | color(name_color)
                    | (selected ? bold : nothing));

    parts.push_back(filler());

    // Git status
    if (opts.show_git_status && node.git_status != GitStatus::Unmodified) {
        auto [indicator, ind_color] = git_indicator(node.git_status);
        parts.push_back(text(indicator) | color(ind_color));
        parts.push_back(text(" "));
    }

    // File size
    if (opts.show_file_size && node.file_size && node.type == NodeType::File) {
        parts.push_back(text(format_size(*node.file_size)) | dim | color(Color::GrayDark));
        parts.push_back(text(" "));
    }

    // Selection checkbox for multi-select
    if (opts.multi_select) {
        parts.push_back(text(node.is_selected ? "[✓]" : "[ ]")
                        | color(node.is_selected ? Color::Green : Color::GrayDark));
        parts.push_back(text(" "));
    }

    auto line = hbox(parts);
    if (selected) {
        line = line | bgcolor(Color::RGB(25, 35, 50));
    }
    return line;
}

/// Render the complete file tree
[[nodiscard]] inline Element RenderFileTree(const FileTreeOptions& opts) {
    // Flatten tree
    std::vector<FlatNode> flat_nodes;
    bool filter_active = !opts.filter.empty();
    flatten_tree(opts.roots, flat_nodes, 0, filter_active, opts.filter);

    if (flat_nodes.empty()) {
        return vbox({
            text(" No files") | dim | center,
            opts.filter.empty()
                ? text("") | size(HEIGHT, EQUAL, 0)
                : text(" (filter: " + opts.filter + ")") | dim | center,
        }) | border;
    }

    // Render visible nodes
    Elements elements;

    // Filter indicator
    if (!opts.filter.empty()) {
        elements.push_back(hbox({
            text(" 🔍 ") | dim,
            text(opts.filter) | color(Color::Cyan),
            filler(),
            text(std::format("{} matches", flat_nodes.size())) | dim,
            text(" "),
        }));
        elements.push_back(separator() | dim);
    }

    int visible_start = opts.scroll_offset;
    int visible_end = std::min(
        static_cast<int>(flat_nodes.size()),
        opts.scroll_offset + 30);  // Max visible lines

    for (int i = visible_start; i < visible_end; ++i) {
        elements.push_back(
            RenderTreeLine(flat_nodes[i], i == opts.selected_index, opts));
    }

    return vbox(elements) | vscroll_indicator | yframe | border;
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a file tree component
[[nodiscard]] inline Component FileTree(FileTreeOptions options) {
    auto state = std::make_shared<FileTreeOptions>(std::move(options));

    return Renderer([state] {
        return RenderFileTree(*state);
    }) | CatchEvent([state](Event event) -> bool {
        // Flatten to get count and access nodes
        std::vector<FlatNode> flat_nodes;
        flatten_tree(state->roots, flat_nodes, 0, !state->filter.empty(), state->filter);
        int count = static_cast<int>(flat_nodes.size());

        if (count == 0) return false;

        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index = std::max(0, state->selected_index - 1);
            // Adjust scroll
            if (state->selected_index < state->scroll_offset) {
                state->scroll_offset = state->selected_index;
            }
            if (state->on_select && state->selected_index < count) {
                state->on_select(*flat_nodes[state->selected_index].node);
            }
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index = std::min(count - 1, state->selected_index + 1);
            if (state->selected_index >= state->scroll_offset + 30) {
                state->scroll_offset = state->selected_index - 29;
            }
            if (state->on_select && state->selected_index < count) {
                state->on_select(*flat_nodes[state->selected_index].node);
            }
            return true;
        }

        if (state->selected_index >= count) return false;
        const auto* node = flat_nodes[state->selected_index].node;

        if (event == Event::Return) {
            if (node->type == NodeType::Directory) {
                if (state->on_toggle) state->on_toggle(*node);
            } else {
                if (state->on_open) state->on_open(*node);
            }
            return true;
        }
        if (event == Event::ArrowRight) {
            if (node->type == NodeType::Directory && !node->is_expanded) {
                if (state->on_toggle) state->on_toggle(*node);
            }
            return true;
        }
        if (event == Event::ArrowLeft) {
            if (node->type == NodeType::Directory && node->is_expanded) {
                if (state->on_toggle) state->on_toggle(*node);
            }
            return true;
        }
        if (event == Event::Character(' ') && state->multi_select) {
            // Toggle selection in multi-select mode
            // The actual toggle would be done by the callback
            if (state->on_select) state->on_select(*node);
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::components::file_tree
