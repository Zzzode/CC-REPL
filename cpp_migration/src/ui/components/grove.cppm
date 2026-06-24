/// @file grove.cppm
/// @brief Grove semantic search tree view - displays hierarchical retrieval
/// results (files, symbols, concepts, references, chunks) from the Grove
/// backend with relevance scoring, kind badges and keyboard navigation.
module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.grove;

import cc.ui.design.primitives;
import cc.ui.design.tokens;
import cc.ui.design.theme;
import cc.types.types;

export namespace cc::ui::components::grove {

using namespace ftxui;
using namespace cc::ui::design::primitives;
using namespace cc::ui::design::tokens;
using namespace cc::ui::design::theme;

// ─── Types ───────────────────────────────────────────────────────────────────

/// Semantic kind of a Grove node.  Drives the colored kind badge.
enum class GroveKind { File, Symbol, Concept, Reference, Chunk };

/// A single node in the results hierarchy.  Mirrors TS GroveNode.
struct GroveNode {
    std::string id;
    std::string title;
    std::string snippet;
    std::string source_path;
    int line_start = 0;
    int line_end = 0;
    double relevance = 0.0;
    GroveKind kind = GroveKind::Chunk;
    std::vector<GroveNode> children;
};

/// View state for the entire Grove panel.  Mirrors TS GroveViewState.
struct GroveViewState {
    std::vector<GroveNode> roots;
    std::vector<GroveNode> query;
    int selected_index = 0;
    int total_results = 0;
    int query_time_ms = 0;
    bool expanded_all = false;
};

// ─── Kind / Badge Helpers ────────────────────────────────────────────────────

/// Human-readable short badge string for a node kind.
[[nodiscard]] inline std::string_view kind_label(GroveKind k) noexcept {
    switch (k) {
        case GroveKind::File:      return "FILE";
        case GroveKind::Symbol:    return "SYM";
        case GroveKind::Concept:   return "CON";
        case GroveKind::Reference: return "REF";
        case GroveKind::Chunk:     return "CHK";
    }
    return "???";
}

/// FTXUI color for a node kind badge.
[[nodiscard]] inline Color kind_color(GroveKind k) noexcept {
    switch (k) {
        case GroveKind::File:      return Color::Blue;
        case GroveKind::Symbol:    return Color::Green;
        case GroveKind::Concept:   return Color::Magenta;
        case GroveKind::Reference: return Color::GrayLight;
        case GroveKind::Chunk:     return Color::Yellow;
    }
    return Color::White;
}

/// Build the colored kind badge pill (5-character wide).
[[nodiscard]] inline Element kind_badge(GroveKind k) {
    auto label = std::string(kind_label(k));
    auto fg = kind_color(k);
    return hbox({
        text(" "),
        text(label) | color(fg) | bold,
        text(" "),
    }) | borderStyled(BorderStyle::ROUNDED, fg)
       | size(WIDTH, EQUAL, 7);
}

/// Colored relevance dot: green >0.8, yellow >0.5, else red.
[[nodiscard]] inline Element relevance_dot(double r) {
    Color c = Color::Red;
    if (r > 0.8)      c = Color::Green;
    else if (r > 0.5) c = Color::Yellow;
    return hbox({
        text("■") | color(c),
    });
}

/// Truncate a string to `max` chars, appending "…" if necessary.
[[nodiscard]] inline std::string truncate(std::string_view s,
                                          std::size_t max = 60) {
    if (s.size() <= max) return std::string(s);
    std::string out(s.substr(0, max > 1 ? max - 1 : 0));
    out.append("…");
    return out;
}

/// Count the total number of (visible) nodes in a forest, respecting a
/// per-node expanded flag that defaults to `s.expanded_all`.
[[nodiscard]] inline int count_nodes(const std::vector<GroveNode>& roots,
                                     bool expanded_all) {
    int count = 0;
    for (const auto& n : roots) {
        ++count;
        if (expanded_all || !n.children.empty()) {
            count += count_nodes(n.children, expanded_all);
        }
    }
    return count;
}

/// Render a "source:line" reference string for the right-hand gutter.
[[nodiscard]] inline std::string source_ref(const GroveNode& n) {
    if (n.source_path.empty()) return {};
    if (n.line_start <= 0) return n.source_path;
    return std::format("{}:{}", n.source_path, n.line_start);
}

// ─── Flat index for keyboard navigation ──────────────────────────────────────

/// A flattened, ordered view of the visible tree.  Each entry records the
/// node pointer plus depth so the render code can apply indentation.
struct FlatEntry {
    const GroveNode* node = nullptr;
    int depth = 0;
    bool is_expanded = false;
};

/// Flatten a forest into a pre-order traversal.  `expanded_all` overrides
/// per-node collapsed state; nodes with children are expanded by default.
[[nodiscard]] inline std::vector<FlatEntry> flatten(
    const std::vector<GroveNode>& roots, bool expanded_all, int depth = 0) {
    std::vector<FlatEntry> out;
    for (const auto& n : roots) {
        bool has_children = !n.children.empty();
        bool expanded = expanded_all || has_children;
        out.push_back({&n, depth, expanded});
        if (expanded) {
            auto sub = flatten(n.children, expanded_all, depth + 1);
            out.insert(out.end(),
                       std::make_move_iterator(sub.begin()),
                       std::make_move_iterator(sub.end()));
        }
    }
    return out;
}

// ─── Component Implementation ────────────────────────────────────────────────

class GroveTreeBase : public ftxui::ComponentBase {
public:
    GroveTreeBase(GroveViewState state,
                  std::function<void(const GroveNode&)> on_select,
                  Theme theme)
        : state_(std::move(state)),
          on_select_(std::move(on_select)),
          theme_(std::move(theme)) {
        rebuild_flat();
    }

    /// Key handling: up/down navigate the flat list; enter fires on_select.
    bool OnEvent(Event e) override {
        if (flat_.empty()) return ComponentBase::OnEvent(e);
        if (e == Event::ArrowDown) {
            selected_ = std::min(selected_ + 1,
                                 static_cast<int>(flat_.size()) - 1);
            return true;
        }
        if (e == Event::ArrowUp) {
            selected_ = std::max(selected_ - 1, 0);
            return true;
        }
        if (e == Event::Return) {
            if (on_select_ && selected_ >= 0 &&
                selected_ < static_cast<int>(flat_.size())) {
                on_select_(*flat_[selected_].node);
            }
            return true;
        }
        if (e == Event::Home) { selected_ = 0; return true; }
        if (e == Event::End) {
            selected_ = static_cast<int>(flat_.size()) - 1; return true;
        }
        return ComponentBase::OnEvent(e);
    }

    Element OnRender() override {
        using namespace ftxui;

        // ── Header ───────────────────────────────────────────────
        auto grove_badge = hbox({
            text(" "),
            text("GROVE") | bold | inverted,
            text(" "),
        }) | color(Color::Black) | bgcolor(theme_.palette->primary);

        auto count_badge = hbox({
            text(" "),
            text(std::format("{}", state_.total_results)) | bold,
            text(" "),
        }) | color(theme_.palette->text)
           | bgcolor(theme_.palette->background);

        auto time_badge = hbox({
            text(" "),
            text(std::format("{}ms", state_.query_time_ms)) | dim,
            text(" "),
        }) | color(theme_.palette->muted)
           | bgcolor(theme_.palette->background);

        auto header = hbox({
            grove_badge,
            text(" "),
            count_badge,
            text(" "),
            time_badge,
            filler(),
        });

        // ── Query bar (if the caller populated query results) ────
        Elements query_rows;
        for (const auto& q : state_.query) {
            query_rows.push_back(hbox({
                text("  › ") | color(theme_.palette->primary),
                text(truncate(q.title, 40)) | color(theme_.palette->text) | dim,
            }));
        }
        Element query_block = query_rows.empty()
            ? text("") | size(HEIGHT, EQUAL, 0)
            : vbox({
                text(""),
                vbox(std::move(query_rows)),
                text(""),
            });

        // ── Tree rows ────────────────────────────────────────────
        Elements rows;
        rows.reserve(flat_.size());
        for (int i = 0; i < static_cast<int>(flat_.size()); ++i) {
            const auto& entry = flat_[i];
            const GroveNode& n = *entry.node;
            bool is_selected = (i == selected_);

            // Indentation prefix (per depth level, 2 cols each).
            Elements indent;
            indent.reserve(entry.depth);
            for (int d = 0; d < entry.depth; ++d) {
                indent.push_back(text("  "));
            }

            // Collapse/expand caret for nodes with children.
            Element caret = n.children.empty()
                ? text("  ")
                : (entry.is_expanded
                       ? text("▼ ") | color(theme_.palette->subtle)
                       : text("▶ ") | color(theme_.palette->subtle));

            // Build the row.
            auto row = hbox({
                hbox(std::move(indent)),
                caret,
                kind_badge(n.kind),
                text(" "),
                text(truncate(n.title, 32)) | bold
                    | color(theme_.palette->text),
                text(" "),
                relevance_dot(n.relevance),
                text(" "),
                text(truncate(n.snippet, 60)) | dim
                    | color(theme_.palette->muted) | xflex,
                text("  "),
                text(source_ref(n)) | dim | color(theme_.palette->muted),
            }) | xflex;

            // Highlight selected row with an accent background.
            if (is_selected) {
                row = row | bgcolor(theme_.palette->subtle)
                          | focusPositionRelative(0, 0);
                row = row | inverted;
            }

            // Wrap in a MenuEntry-like focusable block.
            rows.push_back(std::move(row));
        }

        if (rows.empty()) {
            rows.push_back(
                hbox({ text("  (no results)") | dim
                                             | color(theme_.palette->muted) })
            );
        }

        auto tree_block = vbox(std::move(rows)) | yflex;

        // ── Footer ───────────────────────────────────────────────
        int safe_total = state_.total_results > 0
            ? state_.total_results
            : static_cast<int>(flat_.size());
        int one_based = std::max(0, std::min(selected_ + 1, safe_total));
        auto footer_right = hbox({
            text(std::format("Selected: {}/{}", one_based, safe_total))
                | dim | color(theme_.palette->muted),
        });
        auto footer = hbox({ filler(), footer_right });

        // ── Assemble ─────────────────────────────────────────────
        return vbox({
            header,
            separator() | color(theme_.palette->subtle),
            query_block,
            tree_block,
            separator(),
            footer,
        }) | borderRounded | color(theme_.palette->chrome);
    }

    /// Refresh the state snapshot and rebuild the flat index.
    void update_state(GroveViewState s) {
        state_ = std::move(s);
        rebuild_flat();
        selected_ = std::clamp(selected_, 0,
                               static_cast<int>(flat_.size()) - 1);
    }

private:
    void rebuild_flat() {
        flat_ = flatten(state_.roots, state_.expanded_all, 0);
        if (selected_ >= static_cast<int>(flat_.size())) {
            selected_ = static_cast<int>(flat_.size()) - 1;
        }
        if (selected_ < 0 && !flat_.empty()) selected_ = 0;
    }

    GroveViewState state_;
    std::function<void(const GroveNode&)> on_select_;
    Theme theme_;
    std::vector<FlatEntry> flat_;
    int selected_ = 0;
};

// ─── Public Constructors ─────────────────────────────────────────────────────

/// Build the Grove tree panel with the current (thread-local) theme.
[[nodiscard]] inline auto BuildGroveTree(
    const GroveViewState& s,
    std::function<void(const GroveNode&)> on_select = {})
    -> ftxui::Component {
    Theme theme = current_theme();
    return ftxui::Make<GroveTreeBase>(s, std::move(on_select), std::move(theme));
}

/// Theme-explicit overload for tests / design previews.
[[nodiscard]] inline auto BuildGroveTree(
    const GroveViewState& s,
    const Theme& theme,
    std::function<void(const GroveNode&)> on_select = {})
    -> ftxui::Component {
    return ftxui::Make<GroveTreeBase>(s, std::move(on_select), theme);
}

} // namespace cc::ui::components::grove
