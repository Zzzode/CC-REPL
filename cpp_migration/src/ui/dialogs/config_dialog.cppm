/// @file config_dialog.cppm
/// @brief Configuration editor interface - allows viewing and editing of
/// JSON/TOML configuration files with syntax-aware editing.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <variant>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.config_dialog;

import cc.types.types;

export namespace cc::ui::dialogs::config_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Configuration value type
enum class ConfigValueType : std::uint8_t {
    String,
    Number,
    Boolean,
    Array,
    Object,
    Null,
};

/// A node in the config tree
struct ConfigNode {
    std::string key;
    std::string display_value;      // Rendered value string
    ConfigValueType type;
    int depth = 0;                  // Nesting depth
    bool is_expanded = true;        // For objects/arrays
    bool is_modified = false;       // Changed from file
    bool is_invalid = false;        // Validation error
    std::optional<std::string> validation_error;
    std::optional<std::string> comment;  // Inline comment
    std::vector<ConfigNode> children;
};

/// Configuration file metadata
struct ConfigFileInfo {
    std::string path;               // File path
    std::string format;             // "json", "toml", "yaml"
    bool exists = true;
    bool writable = true;
    std::optional<std::string> scope;  // "project", "user", "system"
};

/// Options for the config dialog
struct ConfigDialogOptions {
    ConfigFileInfo file_info;
    std::vector<ConfigNode> nodes;  // Flattened tree for display
    int selected_index = 0;
    bool editing = false;
    std::string edit_buffer;

    std::function<void(const std::string& path, const std::string& value)> on_edit;
    std::function<void(const std::string& path)> on_delete;
    std::function<void(const std::string& parent_path)> on_add;
    std::function<void()> on_save;
    std::function<void()> on_reload;
    std::function<void()> on_close;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get color for config value type
[[nodiscard]] inline Color type_color(ConfigValueType type) {
    switch (type) {
        case ConfigValueType::String:  return Color::Green;
        case ConfigValueType::Number:  return Color::Cyan;
        case ConfigValueType::Boolean: return Color::Yellow;
        case ConfigValueType::Array:   return Color::Magenta;
        case ConfigValueType::Object:  return Color::Blue;
        case ConfigValueType::Null:    return Color::GrayDark;
    }
    return Color::White;
}

/// Get type label
[[nodiscard]] inline std::string type_label(ConfigValueType type) {
    switch (type) {
        case ConfigValueType::String:  return "str";
        case ConfigValueType::Number:  return "num";
        case ConfigValueType::Boolean: return "bool";
        case ConfigValueType::Array:   return "arr";
        case ConfigValueType::Object:  return "obj";
        case ConfigValueType::Null:    return "null";
    }
    return "?";
}

/// Get fold indicator for expandable nodes
[[nodiscard]] inline std::string fold_indicator(const ConfigNode& node) {
    if (node.type == ConfigValueType::Object || node.type == ConfigValueType::Array) {
        return node.is_expanded ? "▾" : "▸";
    }
    return " ";
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single config node line
[[nodiscard]] inline Element RenderConfigLine(
    const ConfigNode& node, bool selected, bool editing, const std::string& edit_buffer) {

    // Indentation
    std::string indent(node.depth * 2, ' ');

    Elements parts;

    // Fold indicator
    parts.push_back(text(" " + indent + fold_indicator(node) + " ")
                    | color(Color::GrayDark));

    // Key
    parts.push_back(text(node.key) | bold | color(Color::White));
    parts.push_back(text(": ") | dim);

    // Value (or edit buffer if editing this line)
    if (editing && selected) {
        parts.push_back(text(edit_buffer) | color(Color::Yellow) | underlined);
        parts.push_back(text("|") | color(Color::Cyan) | blink);
    } else if (node.type == ConfigValueType::Object) {
        int child_count = static_cast<int>(node.children.size());
        parts.push_back(text(std::format("{{{} keys}}", child_count))
                        | dim | color(Color::Blue));
    } else if (node.type == ConfigValueType::Array) {
        int child_count = static_cast<int>(node.children.size());
        parts.push_back(text(std::format("[{} items]", child_count))
                        | dim | color(Color::Magenta));
    } else {
        parts.push_back(text(node.display_value) | color(type_color(node.type)));
    }

    // Type badge
    parts.push_back(filler());
    parts.push_back(text(" " + type_label(node.type) + " ")
                    | dim | color(type_color(node.type)));

    // Modified indicator
    if (node.is_modified) {
        parts.push_back(text("*") | color(Color::Yellow));
    }
    if (node.is_invalid) {
        parts.push_back(text("!") | color(Color::Red));
    }

    parts.push_back(text(" "));

    auto line = hbox(parts);
    if (selected) {
        line = line | bgcolor(Color::RGB(30, 35, 50));
    }

    return line;
}

/// Render the file info header
[[nodiscard]] inline Element RenderFileHeader(const ConfigFileInfo& info) {
    Elements parts = {
        text(" 📄 ") | dim,
        text(info.path) | bold | color(Color::Cyan),
        text(" "),
    };

    if (info.scope) {
        parts.push_back(text("[" + *info.scope + "]") | dim | color(Color::GrayLight));
    }

    parts.push_back(filler());
    parts.push_back(text(info.format) | dim | color(Color::Magenta));
    parts.push_back(text(" "));

    if (!info.writable) {
        parts.push_back(text(" 🔒 read-only ") | color(Color::Yellow));
    }

    return hbox(parts);
}

/// Flatten tree nodes for display (recursive)
inline void flatten_nodes(
    const std::vector<ConfigNode>& nodes,
    std::vector<const ConfigNode*>& out) {
    for (const auto& node : nodes) {
        out.push_back(&node);
        if (node.is_expanded && !node.children.empty()) {
            flatten_nodes(node.children, out);
        }
    }
}

/// Render the full config dialog
[[nodiscard]] inline Element RenderConfigDialog(const ConfigDialogOptions& opts) {
    // File header
    auto header = RenderFileHeader(opts.file_info);

    // Flatten nodes for display
    std::vector<const ConfigNode*> flat_nodes;
    flatten_nodes(opts.nodes, flat_nodes);

    // Config lines
    Elements config_lines;
    for (int i = 0; i < static_cast<int>(flat_nodes.size()); ++i) {
        config_lines.push_back(
            RenderConfigLine(*flat_nodes[i], i == opts.selected_index,
                            opts.editing, opts.edit_buffer));
    }

    auto content = vbox(config_lines) | vscroll_indicator | yframe | flex;

    // Validation error for selected node
    Element validation_el = text("") | size(HEIGHT, EQUAL, 0);
    if (opts.selected_index >= 0 &&
        opts.selected_index < static_cast<int>(flat_nodes.size())) {
        const auto* node = flat_nodes[opts.selected_index];
        if (node->is_invalid && node->validation_error) {
            validation_el = hbox({
                text(" ✗ ") | color(Color::Red),
                text(*node->validation_error) | color(Color::Red) | dim,
            });
        } else if (node->comment) {
            validation_el = hbox({
                text(" # ") | dim,
                text(*node->comment) | dim | color(Color::GrayLight),
            });
        }
    }

    // Action bar
    auto actions = hbox({
        text(" [Enter]") | color(Color::Cyan), text(" edit "),
        text("[a]") | color(Color::Cyan), text("dd "),
        text("[d]") | color(Color::Cyan), text("elete "),
        text("[s]") | color(Color::Cyan), text("ave "),
        text("[r]") | color(Color::Cyan), text("eload "),
        text("[Esc]") | color(Color::Cyan), text(" close"),
    }) | dim;

    return vbox({
        hbox({
            text(" ⚙️  Configuration Editor ") | bold | color(Color::Blue),
            filler(),
        }),
        header,
        separator(),
        content,
        validation_el,
        separator(),
        actions,
    }) | borderDouble | bgcolor(Color::RGB(15, 15, 20));
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the config dialog component
[[nodiscard]] inline Component ConfigDialog(ConfigDialogOptions options) {
    auto state = std::make_shared<ConfigDialogOptions>(std::move(options));

    return Renderer([state] {
        return RenderConfigDialog(*state);
    }) | CatchEvent([state](Event event) -> bool {
        // Flatten for counting
        std::vector<const ConfigNode*> flat;
        flatten_nodes(state->nodes, flat);
        int count = static_cast<int>(flat.size());

        if (state->editing) {
            if (event == Event::Escape) {
                state->editing = false;
                state->edit_buffer.clear();
                return true;
            }
            if (event == Event::Return) {
                if (state->on_edit && state->selected_index < count) {
                    state->on_edit(flat[state->selected_index]->key, state->edit_buffer);
                }
                state->editing = false;
                state->edit_buffer.clear();
                return true;
            }
            if (event == Event::Backspace) {
                if (!state->edit_buffer.empty()) {
                    state->edit_buffer.pop_back();
                }
                return true;
            }
            if (event.is_character()) {
                state->edit_buffer += event.character();
                return true;
            }
            return false;
        }

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
        if (event == Event::Return) {
            if (state->selected_index < count) {
                const auto* node = flat[state->selected_index];
                if (node->type == ConfigValueType::Object ||
                    node->type == ConfigValueType::Array) {
                    // Toggle expansion - we need mutable access
                    // In real impl this would modify the tree
                } else {
                    state->editing = true;
                    state->edit_buffer = node->display_value;
                }
            }
            return true;
        }
        if (event == Event::Character('a')) {
            if (state->on_add && state->selected_index < count) {
                state->on_add(flat[state->selected_index]->key);
            }
            return true;
        }
        if (event == Event::Character('d')) {
            if (state->on_delete && state->selected_index < count) {
                state->on_delete(flat[state->selected_index]->key);
            }
            return true;
        }
        if (event == Event::Character('s')) {
            if (state->on_save) state->on_save();
            return true;
        }
        if (event == Event::Character('r')) {
            if (state->on_reload) state->on_reload();
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::dialogs::config_dialog
