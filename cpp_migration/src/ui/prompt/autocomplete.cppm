/// @file autocomplete.cppm
/// @brief Command and file path autocomplete component with fuzzy matching,
/// categorized results, and keyboard navigation.
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

export module cc.ui.prompt.autocomplete;

import cc.types.types;

export namespace cc::ui::prompt::autocomplete {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Category of autocomplete suggestion
enum class SuggestionCategory : std::uint8_t {
    Command,        // Slash commands (/help, /clear, etc.)
    File,           // File paths
    Directory,      // Directory paths
    History,        // Previous inputs
    Snippet,        // Code snippets or templates
    Model,          // Model names
    Tool,           // Tool names
};

/// A single autocomplete suggestion
struct AutocompleteSuggestion {
    std::string text;               // Completion text to insert
    std::string display_text;       // What to show in the dropdown
    std::string description;        // Brief description
    SuggestionCategory category;
    int score = 0;                  // Match score for ranking
    std::optional<std::string> icon;
    std::optional<std::string> detail;  // Extra detail shown on highlight
};

/// Configuration for the autocomplete component
struct AutocompleteOptions {
    /// Callback to get suggestions for a query
    std::function<std::vector<AutocompleteSuggestion>(const std::string& input, int cursor_pos)>
        get_suggestions;

    /// Called when a suggestion is accepted
    std::function<void(const AutocompleteSuggestion& suggestion)> on_accept;

    /// Called when autocomplete is dismissed
    std::function<void()> on_dismiss;

    /// Maximum suggestions to display at once
    int max_visible = 10;

    /// Enable fuzzy matching
    bool fuzzy_match = true;

    /// Show categories as headers
    bool show_categories = true;

    /// Minimum characters before triggering
    int min_chars = 1;

    /// Trigger characters (beyond typing)
    std::string trigger_chars = "/.@";
};

/// State of the autocomplete dropdown
struct AutocompleteState {
    bool visible = false;
    std::string query;
    int cursor_pos = 0;
    std::vector<AutocompleteSuggestion> suggestions;
    int selected_index = 0;
    int scroll_offset = 0;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get icon for suggestion category
[[nodiscard]] inline std::string category_icon(SuggestionCategory cat) {
    switch (cat) {
        case SuggestionCategory::Command:   return "⌘";
        case SuggestionCategory::File:      return "📄";
        case SuggestionCategory::Directory: return "📁";
        case SuggestionCategory::History:   return "⏪";
        case SuggestionCategory::Snippet:   return "✂";
        case SuggestionCategory::Model:     return "🤖";
        case SuggestionCategory::Tool:      return "🔧";
    }
    return " ";
}

/// Get category display name
[[nodiscard]] inline std::string category_name(SuggestionCategory cat) {
    switch (cat) {
        case SuggestionCategory::Command:   return "Commands";
        case SuggestionCategory::File:      return "Files";
        case SuggestionCategory::Directory: return "Directories";
        case SuggestionCategory::History:   return "History";
        case SuggestionCategory::Snippet:   return "Snippets";
        case SuggestionCategory::Model:     return "Models";
        case SuggestionCategory::Tool:      return "Tools";
    }
    return "Other";
}

/// Simple fuzzy match score (higher = better match)
[[nodiscard]] inline int fuzzy_score(const std::string& text, const std::string& query) {
    if (query.empty()) return 0;
    if (text.empty()) return -1;

    int score = 0;
    size_t qi = 0;
    bool prev_matched = false;

    for (size_t ti = 0; ti < text.size() && qi < query.size(); ++ti) {
        char tc = static_cast<char>(std::tolower(static_cast<unsigned char>(text[ti])));
        char qc = static_cast<char>(std::tolower(static_cast<unsigned char>(query[qi])));
        if (tc == qc) {
            score += prev_matched ? 3 : 1;  // Consecutive match bonus
            if (ti == 0) score += 5;         // Start-of-string bonus
            prev_matched = true;
            ++qi;
        } else {
            prev_matched = false;
        }
    }

    // All query chars must be found
    if (qi < query.size()) return -1;

    // Bonus for exact prefix match
    if (text.size() >= query.size() &&
        text.substr(0, query.size()) == query) {
        score += 10;
    }

    return score;
}

// ============================================================
// Element Rendering
// ============================================================

/// Render the autocomplete dropdown
[[nodiscard]] inline Element RenderAutocomplete(
    const AutocompleteState& state,
    const AutocompleteOptions& opts) {

    if (!state.visible || state.suggestions.empty()) {
        return text("") | size(WIDTH, EQUAL, 0) | size(HEIGHT, EQUAL, 0);
    }

    Elements elements;

    // Optional category headers
    SuggestionCategory last_category = static_cast<SuggestionCategory>(255);
    int visible_start = state.scroll_offset;
    int visible_end = std::min(
        static_cast<int>(state.suggestions.size()),
        state.scroll_offset + opts.max_visible);

    for (int i = visible_start; i < visible_end; ++i) {
        const auto& s = state.suggestions[i];

        // Category header
        if (opts.show_categories && s.category != last_category) {
            last_category = s.category;
            elements.push_back(
                text(" " + category_name(s.category)) | dim | bold
                    | color(Color::GrayLight));
        }

        // Suggestion line
        std::string icon = s.icon.value_or(category_icon(s.category));
        auto line = hbox({
            text(" " + icon + " ") | dim,
            text(s.display_text.empty() ? s.text : s.display_text)
                | color(Color::White),
            filler(),
            text(s.description) | dim | color(Color::GrayLight),
            text(" "),
        });

        if (i == state.selected_index) {
            line = line | inverted | bgcolor(Color::RGB(40, 60, 80));
        }
        elements.push_back(line);
    }

    // Scroll indicator
    if (static_cast<int>(state.suggestions.size()) > opts.max_visible) {
        int total = static_cast<int>(state.suggestions.size());
        elements.push_back(hbox({
            filler(),
            text(std::format(" {}/{} ", visible_end, total)) | dim,
        }));
    }

    // Detail panel for selected
    if (state.selected_index >= 0 &&
        state.selected_index < static_cast<int>(state.suggestions.size())) {
        const auto& sel = state.suggestions[state.selected_index];
        if (sel.detail) {
            elements.push_back(separator() | dim);
            elements.push_back(text(" " + *sel.detail) | dim | color(Color::Cyan));
        }
    }

    return vbox(elements) | border | bgcolor(Color::RGB(25, 25, 35))
           | size(WIDTH, GREATER_THAN, 40);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an autocomplete component
[[nodiscard]] inline Component Autocomplete(AutocompleteOptions options) {
    auto state = std::make_shared<AutocompleteState>();
    auto opts = std::make_shared<AutocompleteOptions>(std::move(options));

    return Renderer([state, opts] {
        return RenderAutocomplete(*state, *opts);
    }) | CatchEvent([state, opts](Event event) -> bool {
        if (!state->visible) return false;

        int count = static_cast<int>(state->suggestions.size());
        if (count == 0) return false;

        // Navigate down
        if (event == Event::ArrowDown || event == Event::Tab) {
            state->selected_index = (state->selected_index + 1) % count;
            // Adjust scroll
            if (state->selected_index >= state->scroll_offset + opts->max_visible) {
                state->scroll_offset = state->selected_index - opts->max_visible + 1;
            }
            if (state->selected_index < state->scroll_offset) {
                state->scroll_offset = state->selected_index;
            }
            return true;
        }

        // Navigate up
        if (event == Event::ArrowUp || event == Event::TabReverse) {
            state->selected_index = (state->selected_index - 1 + count) % count;
            if (state->selected_index < state->scroll_offset) {
                state->scroll_offset = state->selected_index;
            }
            if (state->selected_index >= state->scroll_offset + opts->max_visible) {
                state->scroll_offset = state->selected_index - opts->max_visible + 1;
            }
            return true;
        }

        // Accept
        if (event == Event::Return) {
            if (opts->on_accept && state->selected_index < count) {
                opts->on_accept(state->suggestions[state->selected_index]);
            }
            state->visible = false;
            return true;
        }

        // Dismiss
        if (event == Event::Escape) {
            state->visible = false;
            if (opts->on_dismiss) {
                opts->on_dismiss();
            }
            return true;
        }

        return false;
    });
}

/// Utility: update autocomplete state from new input
inline void UpdateAutocomplete(
    AutocompleteState& state,
    const AutocompleteOptions& opts,
    const std::string& input,
    int cursor_pos) {

    state.query = input;
    state.cursor_pos = cursor_pos;

    if (static_cast<int>(input.size()) < opts.min_chars) {
        state.visible = false;
        state.suggestions.clear();
        return;
    }

    if (opts.get_suggestions) {
        state.suggestions = opts.get_suggestions(input, cursor_pos);
        state.visible = !state.suggestions.empty();
        state.selected_index = 0;
        state.scroll_offset = 0;
    }
}

} // namespace cc::ui::prompt::autocomplete
