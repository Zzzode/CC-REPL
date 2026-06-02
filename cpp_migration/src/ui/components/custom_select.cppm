/// @file custom_select.cppm
/// @brief Filterable select dropdown with keyboard navigation, multi-select,
/// and inline descriptions. Migrated from CustomSelect/select.tsx.
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

export module cc.ui.custom_select;

import cc.types.types;

export namespace cc::ui::custom_select {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Layout mode for displaying options
enum class SelectLayout : std::uint8_t {
    Compact,            // One line per option
    Expanded,           // Multiple lines with spacing
    CompactVertical,    // Compact index with descriptions below
};

/// A single option in the select dropdown
struct SelectOption {
    std::string label;
    std::string value;
    std::string description;
    bool disabled = false;
    bool dim_description = false;
};

/// Props for the Select component
struct SelectProps {
    std::vector<SelectOption> options;
    std::optional<std::string> default_value;
    bool is_disabled = false;
    bool disable_selection = false;
    bool hide_indexes = false;
    bool multi_select = false;
    bool inline_descriptions = false;
    int visible_option_count = 5;
    std::string highlight_text;
    SelectLayout layout = SelectLayout::Compact;

    std::function<void(const std::string& value)> on_change;
    std::function<void(const std::string& value)> on_focus;
    std::function<void()> on_cancel;
    std::function<void()> on_up_from_first;
    std::function<void()> on_down_from_last;
};

/// Multi-select result
struct MultiSelectResult {
    std::vector<std::string> selected_values;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Highlight matching text within a label
[[nodiscard]] inline Element RenderHighlightedLabel(
    const std::string& label, const std::string& highlight) {

    if (highlight.empty()) {
        return text(label);
    }

    // Simple case-insensitive find
    std::string lower_label = label;
    std::string lower_highlight = highlight;
    std::transform(lower_label.begin(), lower_label.end(),
                   lower_label.begin(), ::tolower);
    std::transform(lower_highlight.begin(), lower_highlight.end(),
                   lower_highlight.begin(), ::tolower);

    auto pos = lower_label.find(lower_highlight);
    if (pos == std::string::npos) {
        return text(label);
    }

    return hbox({
        text(label.substr(0, pos)),
        text(label.substr(pos, highlight.size()))
            | bold | color(Color::Yellow),
        text(label.substr(pos + highlight.size())),
    });
}

/// Render a single option row
[[nodiscard]] inline Element RenderOption(
    const SelectOption& option, bool focused, bool selected,
    int index, bool hide_indexes, bool inline_desc,
    const std::string& highlight) {

    Elements parts;

    // Selection indicator
    if (selected) {
        parts.push_back(text(" ✓ ") | color(Color::Green) | bold);
    } else if (focused) {
        parts.push_back(text(" › ") | color(Color::Cyan) | bold);
    } else {
        parts.push_back(text("   "));
    }

    // Index number
    if (!hide_indexes) {
        parts.push_back(text(std::format("{}.", index + 1)) | dim);
        parts.push_back(text(" "));
    }

    // Label with optional highlighting
    auto label_el = RenderHighlightedLabel(option.label, highlight);
    if (option.disabled) {
        label_el = label_el | dim;
    } else if (focused) {
        label_el = label_el | bold;
    }
    parts.push_back(label_el);

    // Inline description
    if (inline_desc && !option.description.empty()) {
        auto desc_el = text(" — " + option.description);
        if (option.dim_description) {
            desc_el = desc_el | dim;
        } else {
            desc_el = desc_el | color(Color::GrayLight);
        }
        parts.push_back(desc_el);
    }

    auto row = hbox(parts);
    if (focused) {
        row = row | bgcolor(Color::RGB(25, 35, 50));
    }
    return row;
}

/// Render a scroll indicator
[[nodiscard]] inline Element RenderScrollHint(bool above, bool below) {
    Elements items;
    if (above) {
        items.push_back(text("   ↑ more") | dim | color(Color::GrayDark));
    }
    return vbox(items);
}

// ============================================================
// Element Rendering
// ============================================================

/// Render the complete select view
[[nodiscard]] inline Element RenderSelect(
    const SelectProps& props,
    int focused_index,
    const std::vector<bool>& selected_set) {

    int total = static_cast<int>(props.options.size());
    if (total == 0) {
        return text("  (no options)") | dim;
    }

    // Compute visible window
    int visible = std::min(props.visible_option_count, total);
    int start = std::max(0, std::min(focused_index - visible / 2,
                                      total - visible));
    int end = std::min(start + visible, total);

    Elements items;

    // Scroll-up indicator
    if (start > 0) {
        items.push_back(text("   ↑ more") | dim | color(Color::GrayDark));
    }

    // Visible options
    for (int i = start; i < end; ++i) {
        bool is_focused = (i == focused_index);
        bool is_selected = (i < static_cast<int>(selected_set.size()))
                           && selected_set[i];
        items.push_back(RenderOption(
            props.options[i], is_focused, is_selected,
            i, props.hide_indexes, props.inline_descriptions,
            props.highlight_text));

        // Expanded layout: add description below and spacing
        if (props.layout == SelectLayout::Expanded &&
            !props.inline_descriptions &&
            !props.options[i].description.empty()) {
            auto desc = text("     " + props.options[i].description)
                        | dim | color(Color::GrayLight);
            items.push_back(desc);
            if (i < end - 1) items.push_back(text(""));
        }
    }

    // Scroll-down indicator
    if (end < total) {
        items.push_back(text("   ↓ more") | dim | color(Color::GrayDark));
    }

    return vbox(items);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive select component
[[nodiscard]] inline Component Select(SelectProps props) {
    struct State {
        SelectProps props;
        int focused_index = 0;
        std::vector<bool> selected_set;
    };
    auto state = std::make_shared<State>();
    state->props = std::move(props);
    state->selected_set.resize(state->props.options.size(), false);

    // Set default focus
    if (state->props.default_value.has_value()) {
        for (int i = 0; i < static_cast<int>(state->props.options.size()); ++i) {
            if (state->props.options[i].value == *state->props.default_value) {
                state->focused_index = i;
                break;
            }
        }
    }

    return Renderer([state] {
        return RenderSelect(state->props, state->focused_index,
                            state->selected_set);
    }) | CatchEvent([state](Event event) -> bool {
        if (state->props.is_disabled) return false;

        int count = static_cast<int>(state->props.options.size());
        if (count == 0) return false;

        // Navigation
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (state->focused_index > 0) {
                state->focused_index--;
                // Skip disabled options
                while (state->focused_index > 0 &&
                       state->props.options[state->focused_index].disabled) {
                    state->focused_index--;
                }
            } else if (state->props.on_up_from_first) {
                state->props.on_up_from_first();
            }
            if (state->props.on_focus) {
                state->props.on_focus(
                    state->props.options[state->focused_index].value);
            }
            return true;
        }

        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (state->focused_index < count - 1) {
                state->focused_index++;
                while (state->focused_index < count - 1 &&
                       state->props.options[state->focused_index].disabled) {
                    state->focused_index++;
                }
            } else if (state->props.on_down_from_last) {
                state->props.on_down_from_last();
            }
            if (state->props.on_focus) {
                state->props.on_focus(
                    state->props.options[state->focused_index].value);
            }
            return true;
        }

        // Selection
        if (event == Event::Return && !state->props.disable_selection) {
            const auto& opt = state->props.options[state->focused_index];
            if (opt.disabled) return true;

            if (state->props.multi_select) {
                state->selected_set[state->focused_index] =
                    !state->selected_set[state->focused_index];
            }
            if (state->props.on_change) {
                state->props.on_change(opt.value);
            }
            return true;
        }

        // Numeric quick-select
        if (event.is_character() && !state->props.hide_indexes) {
            char ch = event.character()[0];
            if (ch >= '1' && ch <= '9') {
                int idx = ch - '1';
                if (idx < count && !state->props.options[idx].disabled) {
                    state->focused_index = idx;
                    if (!state->props.disable_selection && state->props.on_change) {
                        state->props.on_change(state->props.options[idx].value);
                    }
                    return true;
                }
            }
        }

        // Cancel
        if (event == Event::Escape) {
            if (state->props.on_cancel) state->props.on_cancel();
            return true;
        }

        return false;
    });
}

/// Convenience: create a simple single-select returning the chosen value
[[nodiscard]] inline Component SimpleSelect(
    std::vector<SelectOption> options,
    std::function<void(const std::string&)> on_select,
    std::function<void()> on_cancel) {

    SelectProps props;
    props.options = std::move(options);
    props.on_change = std::move(on_select);
    props.on_cancel = std::move(on_cancel);
    return Select(std::move(props));
}

} // namespace cc::ui::custom_select
