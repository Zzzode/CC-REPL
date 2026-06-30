/// @file quick_open.cppm
/// @brief Quick Open / Command Palette dialog — faithful port of TS
///        QuickOpen component with fuzzy search, categories, and keyboard nav.
///
/// MODULE:   cc.ui.dialogs.quick_open
/// LICENCE:  Exported.  Imported by default_renderers to register, and
///           by dialog_triggers / app code to build item lists.
///
/// TS REFERENCE: src/components/QuickOpen/QuickOpen.tsx
///
/// FEATURES:
///   - Fuzzy substring matching on label + description
///   - Categorized item list with section headers
///   - Keyboard navigation (↑/↓, j/k, Enter, Esc)
///   - Selected item shows description
///   - Scroll indicators for long lists
module;

#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <span>
#include <cctype>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.quick_open;

import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
import cc.ui.design.theme;
import cc.ui.permissions.components;

export namespace cc::ui::dialogs::quick_open {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
using Theme = cc::ui::design::theme::Theme;
using QuickOpenItem = dsys::QuickOpenItem;

// ============================================================
// Fuzzy filtering
// ============================================================

/// Filter items based on a fuzzy query string (case-insensitive).
/// Matches query characters in order within label or description.
[[nodiscard]] inline auto filter_items(std::span<const QuickOpenItem> items,
                                       std::string_view query)
    -> std::vector<QuickOpenItem>
{
    if (query.empty()) {
        return std::vector<QuickOpenItem>(items.begin(), items.end());
    }

    std::vector<QuickOpenItem> results;

    auto to_lower = [](std::string_view s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };

    std::string lower_query = to_lower(query);

    for (const auto& item : items) {
        std::string lower_label = to_lower(item.label);
        std::string lower_desc = to_lower(item.description);

        // Fuzzy match: query chars appear in order in label or description
        auto fuzzy_match = [&](const std::string& haystack) {
            size_t qi = 0;
            for (char c : haystack) {
                if (qi < lower_query.size() && c == lower_query[qi]) {
                    ++qi;
                }
            }
            return qi == lower_query.size();
        };

        if (fuzzy_match(lower_label) || fuzzy_match(lower_desc)) {
            results.push_back(item);
        }
    }

    return results;
}

// ============================================================
// Rendering
// ============================================================

/// Render the QuickOpen dialog using FTXUI + DialogFrame.
/// Faithful to the TS QuickOpen visual style.
[[nodiscard]] inline Element RenderQuickOpen(
    const dsys::QuickOpenPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    auto filtered = filter_items(p.items, p.query);
    int count = static_cast<int>(filtered.size());
    int selected = p.selected_index;
    if (selected >= count) selected = std::max(0, count - 1);
    if (selected < 0) selected = 0;

    // Max visible items (leave room for search input + divider + padding)
    int max_visible = std::max(3, ctx.term_rows - 10);

    // Calculate scroll window
    int start = 0;
    if (selected >= max_visible) {
        start = selected - max_visible + 1;
    }
    int end = std::min(count, start + max_visible);

    // ---- Build item list ----
    Elements list_els;
    std::string current_category;
    namespace pc = cc::ui::permissions::components;

    if (filtered.empty()) {
        list_els.push_back(
            hbox({
                text("  "),
                text("No results") | dim,
            })
        );
    } else {
        for (int i = start; i < end; ++i) {
            const auto& item = filtered[i];
            bool is_selected = (i == selected);

            // Category header
            if (item.category != current_category) {
                current_category = item.category;
                list_els.push_back(
                    hbox({
                        text("── "),
                        text(current_category) | dim,
                        text(" ──") | dim,
                        filler(),
                    })
                );
            }

            // Item row
            auto prefix = is_selected
                ? text("❯ ") | color(Color::Cyan)
                : text("  ");

            auto label_el = is_selected
                ? text(item.label) | bold
                : text(item.label);

            Elements row_els = { prefix, label_el };

            // Shortcut on right
            if (item.shortcut.has_value()) {
                row_els.push_back(filler());
                row_els.push_back(text(*item.shortcut) | dim);
            } else {
                row_els.push_back(filler());
            }

            auto row = hbox(std::move(row_els));
            if (is_selected) {
                row = row | bgcolor(Color::BlueLight) | color(Color::White);
            }
            list_els.push_back(row);

            // Description (only for selected item)
            if (is_selected && !item.description.empty()) {
                list_els.push_back(
                    hbox({
                        text("    "),
                        text(item.description) | dim,
                    })
                );
            }
        }

        // Scroll indicators
        if (start > 0) {
            list_els.insert(list_els.begin(),
                hbox({
                    text("  ↑ "),
                    text(std::to_string(start) + " more") | dim,
                    filler(),
                })
            );
        }
        int remaining = count - end;
        if (remaining > 0) {
            list_els.push_back(
                hbox({
                    text("  ↓ "),
                    text(std::to_string(remaining) + " more") | dim,
                    filler(),
                })
            );
        }
    }

    auto list_el = vbox(std::move(list_els));

    // ---- Search input row ----
    auto query_display = p.query.empty()
        ? text("Type to search...") | dim
        : text(p.query) | bold;

    auto search_row = hbox({
        text("❯ ") | color(Color::Cyan),
        query_display,
        filler(),
        text(std::to_string(count) + " results") | dim,
    });

    // ---- Assemble full content (search + divider + list) ----
    auto full_content = vbox({
        search_row,
        text(""),
        pc::ThinDivider(),
        text(""),
        list_el | yflex,
    });

    // Wrap in DialogFrame
    dframe::DialogFrameProps props;
    props.title = "Quick Open";
    props.subtitle = "Search commands, settings, and more";
    props.style = dframe::FrameStyle::Permission;
    props.content = full_content;
    props.inner_padding_x = 1;
    props.inner_padding_y = 1;
    props.full_border = true;
    props.rounded = true;
    props.pane_variant = dframe::PaneVariant::ModalMinimal;

    return dframe::DialogFrame(props, ctx.theme)
        | size(WIDTH, EQUAL, std::min(60, ctx.term_cols - 4))
        | size(HEIGHT, EQUAL, std::min(20, ctx.term_rows - 4));
}

// ============================================================
// Event handling
// ============================================================

/// Handle keyboard events for the QuickOpen dialog.
/// Returns true if the event was handled.
inline bool HandleQuickOpenEvent(
    dsys::QuickOpenPayload& p,
    const Event& event)
{
    auto filtered = filter_items(p.items, p.query);
    int count = static_cast<int>(filtered.size());

    // Navigation
    if (event == Event::ArrowUp || event == Event::Character('k') ||
        event == Event::Character('K'))
    {
        if (p.selected_index > 0) {
            p.selected_index--;
        } else if (count > 0) {
            p.selected_index = count - 1;  // wrap to bottom
        }
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j') ||
        event == Event::Character('J'))
    {
        if (p.selected_index < count - 1) {
            p.selected_index++;
        } else {
            p.selected_index = 0;  // wrap to top
        }
        return true;
    }

    // Page up/down
    if (event == Event::PageUp) {
        p.selected_index = std::max(0, p.selected_index - 8);
        return true;
    }
    if (event == Event::PageDown) {
        p.selected_index = std::min(count - 1, p.selected_index + 8);
        return true;
    }

    // Home / End
    if (event == Event::Home) {
        p.selected_index = 0;
        return true;
    }
    if (event == Event::End) {
        p.selected_index = std::max(0, count - 1);
        return true;
    }

    // Selection
    if (event == Event::Return) {
        if (count > 0 && p.on_result) {
            int idx = std::min(p.selected_index, count - 1);
            if (idx >= 0) {
                p.on_result(idx, true);
            }
        }
        return true;
    }

    // Cancel
    if (event == Event::Escape) {
        if (p.on_result) {
            p.on_result(-1, false);
        }
        return true;
    }

    // Backspace — remove last char from query
    if (event == Event::Backspace || event == Event::Delete) {
        if (!p.query.empty()) {
            p.query.pop_back();
            p.selected_index = 0;  // reset selection on filter change
        }
        return true;
    }

    // Printable characters — add to query
    if (event.is_character()) {
        char c = event.character()[0];
        // Ignore control characters
        if (std::isprint(static_cast<unsigned char>(c))) {
            p.query += c;
            p.selected_index = 0;  // reset selection on filter change
            return true;
        }
    }

    return false;
}

// ============================================================
// Default item list builder
// ============================================================

/// Build a default set of quick open items (commands, settings, etc.).
/// Faithful to TS QuickOpen's default item list.
[[nodiscard]] inline auto BuildDefaultItems() -> std::vector<QuickOpenItem>
{
    std::vector<QuickOpenItem> items;

    // Commands category
    items.push_back({"Settings", "Open settings panel", "⌘,", "Commands"});
    items.push_back({"Help", "View help and keyboard shortcuts", "?", "Commands"});
    items.push_back({"MCP Servers", "Manage MCP servers", "", "Commands"});
    items.push_back({"Plugins", "Browse and manage plugins", "", "Commands"});
    items.push_back({"Export", "Export conversation", "", "Commands"});
    items.push_back({"Search", "Search conversation history", "⌘K", "Commands"});

    // Settings category
    items.push_back({"Model Settings", "Configure model and API", "", "Settings"});
    items.push_back({"Permissions", "Manage tool permissions", "", "Settings"});
    items.push_back({"API Configuration", "Set API keys and endpoints", "", "Settings"});
    items.push_back({"Privacy", "Privacy and data settings", "", "Settings"});
    items.push_back({"About", "About Claude Code", "", "Settings"});

    return items;
}

} // namespace cc::ui::dialogs::quick_open
