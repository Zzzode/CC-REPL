/// @file suggestion_dropdown.cppm
/// @brief FTXUI Component dropdown for slash-command / file / agent /
/// history suggestions that appear above the Prompt text input.
///
/// Design choices:
///   - Reuses the Suggestion + SuggestionCategory types declared in
///     ui.components.text_input (avoids double definition / ODR risk).
///   - Renders in the style of custom_select.cppm:
///       inverted row for the selected item, 1-row category headers,
///       column-aligned icon + display + description.
///   - Keyboard navigation: Tab / ArrowDown = next, Shift+Tab / ArrowUp = prev,
///     Enter = accept, Escape = dismiss.
///   - The dropdown is a pure child; the *parent* (PromptInput composer)
///     wires TextInputImpl::insert_suggestion() into on_accept.
///
/// Migrated from src/components/PromptInput/PromptInputFooterSuggestions.tsx
///               + SuggestionItem type in PromptInput.tsx.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.prompt.suggestion_dropdown;

import ui.components.text_input;

export namespace cc::ui::prompt::suggestions {
using namespace ftxui;
using ui::components::Suggestion;
using ui::components::SuggestionCategory;

// ============================================================
// Types
// ============================================================

/// Render layout style (matches TS PromptInputFooterSuggestions.tsx).
enum class Layout : std::uint8_t {
    Unified,    // Single flat list, one header per category change
    Command,    // Compact, command-only panel (no category headers)
};

/// Options for the dropdown component.
struct SuggestionDropdownOptions {
    /// Provider callback — returns suggestions for the current input prefix.
    /// Called by the parent on every on_change of TextInputImpl.
    std::function<std::vector<Suggestion>(const std::string& input,
                                          int cursor_pos)>
        get_suggestions;

    /// Fired when the user presses Enter / Tab on a selected item.
    /// The parent is expected to call TextInputImpl::insert_suggestion(s).
    std::function<void(const Suggestion& s)> on_accept;

    /// Fired when the user presses Escape or deletes all query chars.
    std::function<void()> on_dismiss;

    /// Max visible rows in the dropdown (virtual scroll).
    int max_visible = 5;

    /// Min characters before we show anything (for non-trigger queries).
    int min_chars = 1;

    /// Trigger chars that open the panel even at length 1.
    std::string trigger_chars = "/@!*&";

    /// Layout variant.
    Layout layout = Layout::Unified;

    /// If true, insert custom "No results" row instead of hiding when empty.
    bool show_no_results = false;
};

/// Runtime state held inside the component's shared state.
struct SuggestionDropdownState {
    bool visible = false;
    std::string query;
    int cursor_pos = 0;
    std::vector<Suggestion> items;
    int selected_index = 0;
    int scroll_offset = 0;
};

// ============================================================
// Rendering helpers
// ============================================================

/// Per-category icon. Matches the TS file's `iconForCategory()` logic.
[[nodiscard]] inline std::string icon_for(SuggestionCategory cat) {
    switch (cat) {
        case SuggestionCategory::Command:      return "⌘";
        case SuggestionCategory::File:         return "+";  // TS uses "+" for files
        case SuggestionCategory::Directory:    return "📁";
        case SuggestionCategory::History:      return "⏪";
        case SuggestionCategory::Agent:        return "*";  // TS uses "*" for agents
        case SuggestionCategory::MCPResource:  return "◇";  // MCP diamond
        case SuggestionCategory::Shell:        return "$";
        case SuggestionCategory::SlackChannel: return "#";
        case SuggestionCategory::CustomTitle:  return "›";
        case SuggestionCategory::None:         return " ";
    }
    return " ";
}

/// Display name for a suggestion category (used as group header).
[[nodiscard]] inline std::string name_for(SuggestionCategory cat) {
    switch (cat) {
        case SuggestionCategory::Command:      return "Commands";
        case SuggestionCategory::File:         return "Files";
        case SuggestionCategory::Directory:    return "Directories";
        case SuggestionCategory::History:      return "Recent";
        case SuggestionCategory::Agent:        return "Agents";
        case SuggestionCategory::MCPResource:  return "MCP";
        case SuggestionCategory::Shell:        return "Shell";
        case SuggestionCategory::SlackChannel: return "Slack";
        case SuggestionCategory::CustomTitle:  return "";
        case SuggestionCategory::None:         return "";
    }
    return "";
}

[[nodiscard]] inline Color color_for(SuggestionCategory cat) {
    switch (cat) {
        case SuggestionCategory::Command:      return Color::Cyan;
        case SuggestionCategory::File:         return Color::Green;
        case SuggestionCategory::Directory:    return Color::GreenLight;
        case SuggestionCategory::History:      return Color::Yellow;
        case SuggestionCategory::Agent:        return Color::Magenta;
        case SuggestionCategory::MCPResource:  return Color::BlueLight;
        case SuggestionCategory::Shell:        return Color::RedLight;
        case SuggestionCategory::SlackChannel: return Color::CyanLight;
        case SuggestionCategory::CustomTitle:  return Color::GrayLight;
        case SuggestionCategory::None:         return Color::White;
    }
    return Color::White;
}

/// Returns true if we should show the dropdown for this query.
[[nodiscard]] inline bool should_show(std::string_view query,
                                      const SuggestionDropdownOptions& opts) {
    if (query.empty()) return false;
    // Trigger chars open immediately at length 1.
    char first = query[0];
    bool is_trigger = opts.trigger_chars.find(first) != std::string::npos;
    int threshold = is_trigger ? 1 : opts.min_chars;
    return static_cast<int>(query.size()) >= threshold;
}

// ============================================================
// Rendering: dropdown element
// ============================================================

/// Compute the visible row window from state + opts.
struct VisibleWindow {
    int start;
    int end;            // exclusive
    int total;
};
[[nodiscard]] inline VisibleWindow visible_window(
    const SuggestionDropdownState& s,
    const SuggestionDropdownOptions& opts) {
    int n = static_cast<int>(s.items.size());
    int start = s.scroll_offset;
    int end = std::min(n, start + opts.max_visible);
    // Ensure the selected index is within view
    if (s.selected_index >= 0 && s.selected_index < n) {
        if (s.selected_index < start) {
            start = s.selected_index;
            end = std::min(n, start + opts.max_visible);
        } else if (s.selected_index >= end) {
            end = s.selected_index + 1;
            start = std::max(0, end - opts.max_visible);
        }
    }
    return {start, end, n};
}

/// Render a single suggestion row.
[[nodiscard]] inline Element render_row(
    const Suggestion& s, bool selected, Layout layout) {
    std::string icon = s.icon.value_or(icon_for(s.category));
    Color cat_clr = color_for(s.category);

    Element display;
    {
        std::string d = s.display_text.empty() ? s.text : s.display_text;
        if (s.tag && !s.tag->empty()) {
            // Tag appended after display text (e.g. "  [skill]")
            if (layout == Layout::Unified) {
                display = hbox({
                    text(d) | color(Color::White),
                    text(" " + *s.tag) | dim | color(cat_clr),
                });
            } else {
                display = text(d) | color(Color::White);
            }
        } else {
            display = text(d) | color(Color::White);
        }
    }

    Element row = hbox({
        text(" " + icon + " ") | color(cat_clr) | dim,
        display,
        filler(),
        s.description.empty() ? text("")
                              : text(s.description) | dim | color(Color::GrayLight),
        text(" "),
    });

    if (selected) {
        row = row | inverted | bgcolor(Color::RGB(30, 50, 70));
    }
    return row;
}

[[nodiscard]] inline Element RenderDropdown(
    const SuggestionDropdownState& state,
    const SuggestionDropdownOptions& opts) {

    if (!state.visible) return text("");

    if (state.items.empty()) {
        if (!opts.show_no_results) return text("");
        return vbox({
            text(" No results") | dim | color(Color::GrayLight),
        }) | borderLight | bgcolor(Color::RGB(20, 20, 28));
    }

    auto win = visible_window(state, opts);
    Elements rows;

    SuggestionCategory last_cat = static_cast<SuggestionCategory>(255);

    for (int i = win.start; i < win.end; ++i) {
        const auto& item = state.items[i];
        bool selected = (i == state.selected_index);

        // Optional category header (Unified layout only)
        if (opts.layout == Layout::Unified && item.category != last_cat
            && item.category != SuggestionCategory::CustomTitle
            && item.category != SuggestionCategory::None) {
            last_cat = item.category;
            std::string header = name_for(item.category);
            if (!header.empty()) {
                rows.push_back(hbox({
                    text(" " + header) | dim | bold
                        | color(color_for(item.category)),
                    filler(),
                    text(std::format(" {}/{} ", i + 1, win.total)) | dim,
                }));
                rows.push_back(separatorLight() | dim);
            }
        }

        rows.push_back(render_row(item, selected, opts.layout));
    }

    // Scroll footer
    if (win.total > opts.max_visible) {
        rows.push_back(hbox({
            filler(),
            text(std::format(" showing {}-{} of {} ",
                             win.start + 1, win.end, win.total)) | dim,
        }));
    }

    // Detail / tag line for selected (expanded for MCP + Agent)
    if (state.selected_index >= 0 && state.selected_index < win.total) {
        const auto& sel = state.items[state.selected_index];
        bool show_detail =
            sel.category == SuggestionCategory::MCPResource ||
            sel.category == SuggestionCategory::Agent;
        if (show_detail && !sel.description.empty()) {
            rows.push_back(separatorLight() | dim);
            rows.push_back(hbox({
                text(" ") | dim,
                text(sel.description) | dim | color(Color::Cyan),
            }));
        }
    }

    return vbox(rows)
        | borderLight
        | bgcolor(Color::RGB(20, 20, 28))
        | size(WIDTH, GREATER_THAN, 36);
}

// ============================================================
// Component factory
// ============================================================

/// Adjust scroll after a selection change so the cursor stays in view.
inline void keep_selected_in_view(SuggestionDropdownState& s,
                                  const SuggestionDropdownOptions& opts) {
    int n = static_cast<int>(s.items.size());
    if (n == 0) return;
    if (s.selected_index < 0) s.selected_index = 0;
    if (s.selected_index >= n) s.selected_index = n - 1;
    if (s.scroll_offset > s.selected_index) {
        s.scroll_offset = s.selected_index;
    } else if (s.scroll_offset + opts.max_visible <= s.selected_index) {
        s.scroll_offset = s.selected_index - opts.max_visible + 1;
    }
}

/// Advance selection by `delta` with wrap-around.
inline void advance(SuggestionDropdownState& s,
                    const SuggestionDropdownOptions& opts, int delta) {
    int n = static_cast<int>(s.items.size());
    if (n == 0) return;
    s.selected_index = (s.selected_index + delta % n + n) % n;
    keep_selected_in_view(s, opts);
}

/// Create a SuggestionDropdown component.
///
/// The parent is expected to call `UpdateSuggestions()` (exposed free
/// function below) whenever the text input changes.
[[nodiscard]] inline Component SuggestionDropdown(
    SuggestionDropdownOptions options) {

    auto state = std::make_shared<SuggestionDropdownState>();
    auto opts  = std::make_shared<SuggestionDropdownOptions>(std::move(options));

    return Renderer([state, opts] {
        return RenderDropdown(*state, *opts);
    }) | CatchEvent([state, opts](Event event) -> bool {
        if (!state->visible) return false;
        int n = static_cast<int>(state->items.size());

        // Tab / ArrowDown → next (Tab also accepts on first row in TS)
        if (event == Event::ArrowDown) {
            advance(*state, *opts, +1);
            return true;
        }
        if (event == Event::Tab) {
            if (n <= 1) {
                // Single item → accept immediately
                if (n == 1 && opts->on_accept) {
                    opts->on_accept(state->items.front());
                }
                state->visible = false;
                return true;
            }
            advance(*state, *opts, +1);
            return true;
        }
        // ArrowUp / TabReverse → previous
        if (event == Event::ArrowUp) {
            advance(*state, *opts, -1);
            return true;
        }
        if (event == Event::TabReverse) {
            advance(*state, *opts, -1);
            return true;
        }
        // Enter → accept
        if (event == Event::Return) {
            if (n > 0 && opts->on_accept) {
                opts->on_accept(state->items[state->selected_index]);
            }
            state->visible = false;
            return true;
        }
        // Escape → dismiss without accepting
        if (event == Event::Escape) {
            state->visible = false;
            if (opts->on_dismiss) opts->on_dismiss();
            return true;
        }
        // Ctrl+J / Ctrl+K (common terminal fallback)
        if (event.is_character() && event.character().size() == 1) {
            char c = event.character()[0];
            if (c == '\n') {  // Ctrl+J (alternative Enter)
                if (n > 0 && opts->on_accept) {
                    opts->on_accept(state->items[state->selected_index]);
                }
                state->visible = false;
                return true;
            }
        }
        return false;
    });
}

// ============================================================
// Free update function — called by the PromptInput composer
// whenever the text buffer changes.
// ============================================================

/// Populate `state` with new suggestions for `(input, cursor_pos)`,
/// show / hide the dropdown, and reset the selection.
///
/// This is the *single* entry point the parent should call from
/// TextInputImpl's on_change callback. Keeping this free-standing
/// makes it easy to unit-test the visibility logic.
inline void UpdateSuggestions(
    SuggestionDropdownState& state,
    const SuggestionDropdownOptions& opts,
    const std::string& input,
    int cursor_pos) {

    state.query = input;
    state.cursor_pos = cursor_pos;

    if (!should_show(input, opts)) {
        state.visible = false;
        state.items.clear();
        state.selected_index = 0;
        state.scroll_offset = 0;
        return;
    }

    if (opts.get_suggestions) {
        state.items = opts.get_suggestions(input, cursor_pos);
    } else {
        state.items.clear();
    }

    if (state.items.empty()) {
        state.visible = opts.show_no_results;
        state.selected_index = 0;
        state.scroll_offset = 0;
        return;
    }

    state.visible = true;
    state.selected_index = 0;
    state.scroll_offset = 0;
}

/// Same as above, but returns the visible-state bool for convenience.
[[nodiscard]] inline bool IsSuggestionsVisible(
    const SuggestionDropdownState& state) noexcept {
    return state.visible;
}

/// Accessor for tests / external wiring.
[[nodiscard]] inline const Suggestion* SelectedSuggestion(
    const SuggestionDropdownState& state) {
    if (state.selected_index < 0 ||
        state.selected_index >= static_cast<int>(state.items.size())) {
        return nullptr;
    }
    return &state.items[state.selected_index];
}

} // namespace cc::ui::prompt::suggestions
