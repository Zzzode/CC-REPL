/// @file output_style_picker.cppm
/// @brief Output style picker dialog — interactive radio-button selection of
/// output verbosity style (normal, verbose, compact, etc.) with live preview.
module;
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.output_style_picker;

export namespace cc::ui::dialogs {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Available output style options
enum class OutputStyle {
    normal,
    verbose,
    compact,
    json,
    quiet,
};

/// Convert output style enum to display label
[[nodiscard]] inline std::string style_label(OutputStyle style) {
    switch (style) {
        case OutputStyle::normal:  return "Normal";
        case OutputStyle::verbose: return "Verbose";
        case OutputStyle::compact: return "Compact";
        case OutputStyle::json:    return "JSON";
        case OutputStyle::quiet:   return "Quiet";
    }
    return "?";
}

/// Convert output style enum to a short description
[[nodiscard]] inline std::string style_description(OutputStyle style) {
    switch (style) {
        case OutputStyle::normal:
            return "Standard output with balanced detail";
        case OutputStyle::verbose:
            return "Detailed output including reasoning and metadata";
        case OutputStyle::compact:
            return "Condensed output, minimal formatting";
        case OutputStyle::json:
            return "Structured JSON output for programmatic use";
        case OutputStyle::quiet:
            return "Suppress all non-essential output";
    }
    return "";
}

/// A style option for the picker UI
struct StyleOption {
    OutputStyle style;
    std::string label;
    std::string description;
    bool is_current = false;
};

/// Props for the output style picker dialog component
struct OutputStylePickerProps {
    std::function<void(OutputStyle)> on_apply;
    std::function<void()> on_cancel;
    OutputStyle current_style = OutputStyle::normal;
    int dialog_width = 68;
};

// ============================================================
// Default Data
// ============================================================

/// Build the full list of style options from the enum
[[nodiscard]] inline std::vector<StyleOption> all_style_options(
    OutputStyle current) {

    std::vector<StyleOption> options;
    for (int i = 0; i <= static_cast<int>(OutputStyle::quiet); ++i) {
        auto s = static_cast<OutputStyle>(i);
        options.push_back({
            s,
            style_label(s),
            style_description(s),
            s == current,
        });
    }
    return options;
}

// ============================================================
// Preview Generation
// ============================================================

/// Generate a sample preview string for a given output style
[[nodiscard]] inline std::string style_preview_text(OutputStyle style) {
    switch (style) {
        case OutputStyle::normal:
            return "Found 3 files matching pattern.\n"
                   "  src/main.cpp\n"
                   "  src/utils.cpp\n"
                   "  src/app.cpp";
        case OutputStyle::verbose:
            return "[search] Scanning directory: src/\n"
                   "[search] Pattern: *.cpp\n"
                   "[search] Found 3 matches in 12ms\n"
                   "  src/main.cpp  (2.4KB, modified 2025-06-01)\n"
                   "  src/utils.cpp (1.1KB, modified 2025-05-30)\n"
                   "  src/app.cpp   (3.8KB, modified 2025-06-07)\n"
                   "[done] Search complete.";
        case OutputStyle::compact:
            return "3 matches: main.cpp, utils.cpp, app.cpp";
        case OutputStyle::json:
            return "{\"matches\":3,\"files\":[\"src/main.cpp\","
                   "\"src/utils.cpp\",\"src/app.cpp\"]}";
        case OutputStyle::quiet:
            return "3";
    }
    return "";
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single radio-style option row
[[nodiscard]] inline Element RenderStyleRadio(
    const StyleOption& opt, bool selected) {

    // Radio indicator: filled circle for selected, empty for others
    auto radio = selected
        ? text(" ● ") | color(Color::Cyan)
        : text(" ○ ");

    auto label_el = text(" " + opt.label) | (selected ? bold : nothing);

    // Current indicator badge
    Elements row_elements;
    row_elements.push_back(radio);
    row_elements.push_back(label_el);

    if (opt.is_current) {
        row_elements.push_back(text(" (current)") | color(Color::Green) | dim);
    }

    row_elements.push_back(filler());
    row_elements.push_back(text(opt.description) | dim);

    auto row = hbox(row_elements);
    if (selected) {
        row = row | bgcolor(Color::RGB(30, 40, 55));
    }
    return row;
}

/// Render the radio selection list
[[nodiscard]] inline Element RenderStyleList(
    const std::vector<StyleOption>& options,
    int focused) {

    Elements items;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        items.push_back(RenderStyleRadio(options[i], i == focused));
    }
    return vbox(items);
}

/// Render the live preview panel for the focused style
[[nodiscard]] inline Element RenderPreview(OutputStyle style) {
    auto preview_text = style_preview_text(style);

    // Split preview into lines for rendering
    Elements lines;
    lines.push_back(text(" Preview:") | bold | dim);
    lines.push_back(separator());

    // Simple line splitting
    std::string::size_type pos = 0;
    std::string::size_type prev = 0;
    while ((pos = preview_text.find('\n', prev)) != std::string::npos) {
        lines.push_back(text("  " + preview_text.substr(prev, pos - prev)) | dim);
        prev = pos + 1;
    }
    lines.push_back(text("  " + preview_text.substr(prev)) | dim);

    return vbox(lines)
        | border
        | size(HEIGHT, LESS_THAN, 10)
        | color(Color::GrayLight);
}

/// Render the action buttons bar (Apply / Cancel)
[[nodiscard]] inline Element RenderButtonBar(int focused) {
    auto apply_label  = text(" Apply ");
    auto cancel_label = text(" Cancel ");

    auto apply_btn = (focused == 0)
        ? apply_label | bold | inverted | color(Color::Green)
        : apply_label | color(Color::Green);

    auto cancel_btn = (focused == 1)
        ? cancel_label | bold | inverted
        : cancel_label | dim;

    return hbox({
        text("  "),
        apply_btn | size(WIDTH, GREATER_THAN, 10),
        text("  "),
        cancel_btn | size(WIDTH, GREATER_THAN, 10),
        filler(),
        text("Enter") | bold | dim,
        text(" confirm · ") | dim,
        text("Tab") | bold | dim,
        text(" switch · ") | dim,
        text("Esc") | bold | dim,
        text(" cancel") | dim,
    });
}

/// Render the full output style picker dialog
[[nodiscard]] inline Element RenderOutputStylePickerDialog(
    const std::vector<StyleOption>& options,
    int focused_style,
    int focused_button,
    int width) {

    auto selected_style = (focused_style >= 0
        && focused_style < static_cast<int>(options.size()))
        ? options[focused_style].style
        : OutputStyle::normal;

    auto body = vbox({
        text(" Select an output verbosity style:") | dim,
        text(""),
        RenderStyleList(options, focused_style),
        text(""),
        RenderPreview(selected_style),
        text(""),
        separator(),
        RenderButtonBar(focused_button),
    });

    return window(
        text(" Select Output Style ") | bold | color(Color::Cyan),
        body | size(WIDTH, LESS_THAN, width)
    ) | color(Color::Cyan);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the output style picker dialog component
[[nodiscard]] inline Component OutputStylePickerDialog(
    OutputStylePickerProps props) {

    /// UI focus area: either the style list or the button bar
    enum class FocusArea { list, buttons };

    struct State {
        OutputStylePickerProps props;
        std::vector<StyleOption> options;
        int focused_style = 0;
        int focused_button = 0;
        FocusArea focus = FocusArea::list;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);
    state->options = all_style_options(state->props.current_style);

    // Default focus to the current style
    for (int i = 0; i < static_cast<int>(state->options.size()); ++i) {
        if (state->options[i].is_current) {
            state->focused_style = i;
            break;
        }
    }

    return Renderer([state] {
        return RenderOutputStylePickerDialog(
            state->options,
            state->focused_style,
            state->focused_button,
            state->props.dialog_width);
    }) | CatchEvent([state](Event event) -> bool {
        auto style_count = static_cast<int>(state->options.size());

        if (state->focus == FocusArea::list) {
            // Up / 'k' to navigate style list
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->focused_style =
                    std::max(0, state->focused_style - 1);
                return true;
            }
            // Down / 'j' to navigate style list
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->focused_style =
                    std::min(style_count - 1, state->focused_style + 1);
                return true;
            }
            // Tab to switch focus to buttons
            if (event == Event::Tab) {
                state->focus = FocusArea::buttons;
                return true;
            }
            // Enter to apply the focused style directly
            if (event == Event::Return) {
                if (state->props.on_apply) {
                    state->props.on_apply(
                        state->options[state->focused_style].style);
                }
                return true;
            }
        }

        if (state->focus == FocusArea::buttons) {
            // Left/Right to toggle between Apply and Cancel
            if (event == Event::ArrowLeft || event == Event::ArrowRight
                || event == Event::Tab) {
                state->focused_button =
                    (state->focused_button + 1) % 2;
                return true;
            }
            // Up to return to list
            if (event == Event::ArrowUp) {
                state->focus = FocusArea::list;
                return true;
            }
            // Enter to activate focused button
            if (event == Event::Return) {
                if (state->focused_button == 0) {
                    // Apply
                    if (state->props.on_apply) {
                        state->props.on_apply(
                            state->options[state->focused_style].style);
                    }
                } else {
                    // Cancel
                    if (state->props.on_cancel) {
                        state->props.on_cancel();
                    }
                }
                return true;
            }
        }

        // Escape always cancels
        if (event == Event::Escape) {
            if (state->props.on_cancel) {
                state->props.on_cancel();
            }
            return true;
        }

        return false;
    });
}

/// Create output style picker with simple apply callback (overload)
[[nodiscard]] inline Component OutputStylePickerDialog(
    OutputStyle current_style,
    std::function<void(OutputStyle)> on_apply) {

    OutputStylePickerProps props;
    props.current_style = current_style;
    props.on_apply = std::move(on_apply);
    return OutputStylePickerDialog(std::move(props));
}

} // namespace cc::ui::dialogs
