/// @file partial_completions.cppm
/// @brief PARTIAL COMPLETIONS module — 3 components:
///   (1) TreeSelect: 3-state checkbox FSM cascade (j/k/h/l/Space/*/a + /search)
///   (2) HighlightedCode Fallback: heuristic regex colorizer (6 rules, gutter layout)
///   (3) GlimmerMessage: streaming skeleton (4 style variants, 1-char/frame shimmer)
///
/// PARTIAL COMPLETED by UI27.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.components.partial_completions;

import cc.types.types;
import cc.ui.design.tokens;

export namespace cc::ui::components::partial {
using namespace ftxui;

// =====================================================================
// PARTIAL#1 — TreeSelect: multi-state checkbox tree
// =====================================================================

enum class CheckState : std::uint8_t { Unchecked, Partial, Checked };

struct TreeSelectNode {
    std::string id;
    std::string label;
    enum class Kind : std::uint8_t { Root, Folder, File, Leaf } kind = Kind::Leaf;
    std::optional<std::string> icon;
    std::vector<TreeSelectNode> children;
    CheckState state = CheckState::Unchecked;
    bool expanded = false;
    bool selectable = true;
    std::optional<std::size_t> count;

    TreeSelectNode() = default;
    TreeSelectNode(std::string i, std::string l, Kind k = Kind::Leaf)
        : id(std::move(i)), label(std::move(l)), kind(k) {
        if (k == Kind::Folder || k == Kind::Root) expanded = true;
    }
};

struct TreeSelectCallbacks {
    std::function<void(const TreeSelectNode&)> on_toggle_expand;
    std::function<void(const std::vector<std::string>& checked_ids)> on_submit;
    std::function<void(std::string_view id, CheckState)> on_state_change;
};

namespace detail_ts {

inline void set_state_recursive(TreeSelectNode& n, CheckState s) {
    if (n.selectable) n.state = s;
    for (auto& c : n.children) set_state_recursive(c, s);
}

// Recompute parent state from children (post-order cascade-up).
inline CheckState recompute_parent(TreeSelectNode& n) {
    if (n.children.empty()) return n.state;
    int checked = 0, unchecked = 0, total = 0;
    for (auto& c : n.children) {
        auto cs = recompute_parent(c);
        if (!c.selectable) continue;
        ++total;
        if (cs == CheckState::Checked) ++checked;
        else if (cs == CheckState::Unchecked) ++unchecked;
    }
    if (total == 0) return n.state;
    if (checked == total)       n.state = CheckState::Checked;
    else if (unchecked == total) n.state = CheckState::Unchecked;
    else                         n.state = CheckState::Partial;
    return n.state;
}

struct TSFlat { const TreeSelectNode* node; int depth; };

inline void flatten_visible(const TreeSelectNode& n,
                            std::vector<TSFlat>& out,
                            int depth = 0) {
    out.push_back({&n, depth});
    if ((n.kind == TreeSelectNode::Kind::Folder ||
         n.kind == TreeSelectNode::Kind::Root) && n.expanded) {
        for (auto& c : n.children)
            flatten_visible(c, out, depth + 1);
    }
}

// Filter-aware flatten: a node is kept if it or any descendant matches;
// ancestors auto-expand along matching paths.
inline void flatten_filtered(const TreeSelectNode& n,
                             std::vector<TSFlat>& out,
                             const std::string& filter,
                             int depth = 0) {
    if (filter.empty()) { flatten_visible(n, out, depth); return; }
    auto lower = [](std::string_view s) {
        std::string o(s);
        std::transform(o.begin(), o.end(), o.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return o;
    };
    std::string needle = lower(filter);

    // Return whether subtree contains a match.
    std::function<bool(const TreeSelectNode&, int)> walk =
        [&](const TreeSelectNode& cur, int d) -> bool {
            bool self_match = lower(cur.label).find(needle) != std::string::npos;
            bool child_match = false;
            for (auto& c : cur.children)
                if (walk(c, d + 1)) child_match = true;
            if (self_match || child_match) {
                out.push_back({&cur, d});
                return true;
            }
            return false;
        };
    (void)depth;
    walk(n, 0);
    // walk produced pre-order; but it may include ancestor nodes at correct depth.
}

} // namespace detail_ts

[[nodiscard]] inline std::string_view check_glyph(CheckState s) {
    switch (s) {
        case CheckState::Checked:   return "[✓]";
        case CheckState::Partial:   return "[~]";
        case CheckState::Unchecked: return "[ ]";
    }
    return "[ ]";
}

// Render a tree element statically (non-interactive Element).
[[nodiscard]] inline Element RenderTreeElement(const TreeSelectNode& root) {
    std::vector<detail_ts::TSFlat> flat;
    detail_ts::flatten_visible(root, flat);
    Elements lines;
    lines.reserve(flat.size());
    for (auto& f : flat) {
        const auto* node = f.node;
        Elements row;
        row.push_back(text(std::string(f.depth * 3, ' ')) | dim);
        if (node->kind == TreeSelectNode::Kind::Folder ||
            node->kind == TreeSelectNode::Kind::Root)
            row.push_back(text(node->expanded ? "▾ " : "▸ ")
                          | color(Color::GrayLight));
        else
            row.push_back(text("  "));
        if (node->icon) row.push_back(text(*node->icon + " "));
        row.push_back(text(check_glyph(node->state)) |
            color(node->state == CheckState::Checked   ? Color::Green :
                  node->state == CheckState::Partial   ? Color::Yellow :
                                                         Color::GrayDark));
        row.push_back(text(" "));
        row.push_back(text(node->label) |
            color(node->kind == TreeSelectNode::Kind::Folder
                      ? Color::Blue : Color::White));
        row.push_back(filler());
        if (node->count)
            row.push_back(text(std::format(" ({})", *node->count)) | dim);
        lines.push_back(hbox(std::move(row)));
    }
    return vbox(std::move(lines));
}

// Interactive TreeSelect component factory.
inline Component MakeTreeSelect(TreeSelectNode root,
                                TreeSelectCallbacks cbs = {}) {
    struct St {
        TreeSelectNode root;
        TreeSelectCallbacks cbs;
        int selected = 0;
        int scroll = 0;
        std::string filter;
        bool in_search = false;
        Component search_input;
    };
    auto st = std::make_shared<St>();
    st->root = std::move(root);
    st->cbs = std::move(cbs);
    st->search_input = Input(&st->filter, "Filter nodes…");

    auto flat = [st]() -> std::vector<detail_ts::TSFlat> {
        std::vector<detail_ts::TSFlat> out;
        if (st->filter.empty() && !st->in_search)
            detail_ts::flatten_visible(st->root, out);
        else
            detail_ts::flatten_filtered(st->root, out, st->filter);
        return out;
    };

    auto toggle = [st](TreeSelectNode* node) {
        if (!node || !node->selectable) return;
        // Cycle: Unchecked/Partial → Checked (cascade down); Checked → Unchecked
        CheckState target = (node->state == CheckState::Checked)
            ? CheckState::Unchecked : CheckState::Checked;
        detail_ts::set_state_recursive(*node, target);
        detail_ts::recompute_parent(st->root);
        if (st->cbs.on_state_change)
            st->cbs.on_state_change(node->id, node->state);
    };

    auto rec_expand = [](TreeSelectNode& n, bool ex) {
        if (n.kind == TreeSelectNode::Kind::Folder ||
            n.kind == TreeSelectNode::Kind::Root)
            n.expanded = ex;
        for (auto& c : n.children) rec_expand(c, ex);
    };

    auto collect = [](const TreeSelectNode& n,
                      std::vector<std::string>& ids) {
        if (n.selectable && n.state == CheckState::Checked)
            ids.push_back(n.id);
        for (auto& c : n.children) collect(c, ids);
    };

    auto list = Renderer([st, flat] {
        auto v = flat();
        int n = (int)v.size();
        if (n == 0) {
            return vbox({
                text(" (tree empty)") | dim,
                (st->filter.empty()
                    ? text("") : text(" filter: " + st->filter) | dim),
            });
        }
        st->selected = std::min(st->selected, n - 1);
        int page = 20;
        int start = std::max(0, std::min(st->scroll, std::max(0, n - page)));
        int end = std::min(n, start + page);

        Elements lines;
        if (st->in_search) {
            lines.push_back(hbox({
                text("🔍 ") | dim,
                st->search_input->Render(),
                filler(),
                text(std::format(" {} nodes", n)) | dim,
            }));
            lines.push_back(separator() | dim);
        } else if (!st->filter.empty()) {
            lines.push_back(hbox({
                text(" 🔎 ") | dim,
                text(st->filter) | color(Color::Cyan),
                filler(),
                text(std::format(" {} nodes", n)) | dim,
            }));
            lines.push_back(separator() | dim);
        }
        for (int i = start; i < end; ++i) {
            auto* node = v[i].node;
            bool sel = (i == st->selected);
            Elements row;
            row.push_back(text(std::string(v[i].depth * 2, ' ')) | dim);
            if (node->kind == TreeSelectNode::Kind::Folder ||
                node->kind == TreeSelectNode::Kind::Root)
                row.push_back(text(node->expanded ? "▾ " : "▸ ")
                              | color(Color::GrayLight));
            else
                row.push_back(text("  "));
            if (node->icon) row.push_back(text(*node->icon + " "));
            row.push_back(text(check_glyph(node->state)) |
                color(node->state == CheckState::Checked ? Color::Green :
                      node->state == CheckState::Partial ? Color::Yellow :
                                                             Color::GrayDark));
            row.push_back(text(" "));
            row.push_back(text(node->label) |
                color(node->kind == TreeSelectNode::Kind::Folder
                          ? Color::Blue : Color::White) |
                (sel ? bold : nothing));
            row.push_back(filler());
            if (node->count)
                row.push_back(text(std::format(" ({})", *node->count)) | dim);
            Element line = hbox(std::move(row));
            if (sel) line = line | bgcolor(Color::RGB(25, 35, 50));
            lines.push_back(std::move(line));
        }
        return vbox(std::move(lines)) | yframe | flex;
    }) | CatchEvent([st, flat, toggle, rec_expand, collect](Event e) -> bool {
        auto v = flat();
        int n = (int)v.size();

        if (st->in_search) {
            // Delegate to search input; also handle Esc/Ctrl
            if (e == Event::Escape) {
                st->in_search = false;
                st->filter.clear();
                return true;
            }
            if (e == Event::Return) {
                st->in_search = false;
                return true;
            }
            // forward printable chars / backspace to input
            return st->search_input->OnEvent(e);
        }

        if (e == Event::ArrowDown || e == Event::Character('j')) {
            if (n > 0) st->selected = std::min(n - 1, st->selected + 1);
            int page = 20;
            if (st->selected >= st->scroll + page)
                st->scroll = std::max(0, st->selected - page + 1);
            return true;
        }
        if (e == Event::ArrowUp || e == Event::Character('k')) {
            st->selected = std::max(0, st->selected - 1);
            if (st->selected < st->scroll)
                st->scroll = st->selected;
            return true;
        }
        if (n == 0) return false;
        auto* cur = v[st->selected].node;
        auto* mcur = const_cast<TreeSelectNode*>(cur);

        if (e == Event::ArrowRight || e == Event::Character('l')) {
            if (cur->kind == TreeSelectNode::Kind::Folder && !cur->expanded) {
                mcur->expanded = true;
                if (st->cbs.on_toggle_expand) st->cbs.on_toggle_expand(*cur);
            } else if (cur->kind == TreeSelectNode::Kind::Folder && cur->expanded
                       && !cur->children.empty()) {
                // jump to first child in visible list
                auto* target = &cur->children.front();
                for (int i = st->selected + 1; i < n; ++i)
                    if (v[i].node == target) { st->selected = i; break; }
            }
            return true;
        }
        if (e == Event::ArrowLeft || e == Event::Character('h')) {
            if (cur->kind == TreeSelectNode::Kind::Folder && cur->expanded) {
                mcur->expanded = false;
                return true;
            }
            // jump to parent: scan backward in visible list
            int cur_depth = v[st->selected].depth;
            for (int i = st->selected - 1; i >= 0; --i) {
                if (v[i].depth < cur_depth) { st->selected = i; break; }
            }
            return true;
        }
        if (e == Event::Character(' ')) {
            toggle(mcur);
            return true;
        }
        if (e == Event::Return) {
            toggle(mcur);
            std::vector<std::string> ids;
            collect(st->root, ids);
            if (st->cbs.on_submit) st->cbs.on_submit(std::move(ids));
            return true;
        }
        if (e == Event::Character('*')) {
            if (cur->kind == TreeSelectNode::Kind::Folder)
                rec_expand(*mcur, true);
            return true;
        }
        if (e == Event::Character('a')) {
            // Toggle all currently-visible selectable nodes (this level only)
            bool all_checked = true;
            for (auto& x : v)
                if (x.node->selectable && x.node->state != CheckState::Checked)
                    { all_checked = false; break; }
            CheckState target = all_checked ? CheckState::Unchecked
                                            : CheckState::Checked;
            for (auto& x : v)
                if (x.node->selectable && v[std::distance(&v[0], &x)].depth
                    == v[st->selected].depth) {
                    // only items at same depth → treat "this level"
                    auto* nx = const_cast<TreeSelectNode*>(x.node);
                    detail_ts::set_state_recursive(*nx, target);
                }
            detail_ts::recompute_parent(st->root);
            return true;
        }
        if (e == Event::Character('/')) {
            st->in_search = true;
            return true;
        }
        return false;
    });

    return list;
}

// =====================================================================
// PARTIAL#2 — HighlightedCode Fallback (Fallback.tsx heuristic regex)
// =====================================================================
// TODO(UI27): Integrate into code_highlight.cppm RenderCodeBlock as the
//             shiki-unavailable branch (replace `if (!shiki_ok)` body).

struct FallbackOptions {
    bool show_line_numbers = true;
    int start_line = 1;
    int max_line_len = 400;
    // theme is SyntaxTheme-compatible — we keep a minimal local struct to
    // avoid a hard import of the entire code_highlight module.
    struct Theme {
        Color plain       = Color::GrayLight;
        Color keyword     = Color::Cyan;
        Color string_lit  = Color::Yellow;
        Color number      = Color::Magenta;
        Color comment     = Color::GrayDark;
        Color function    = Color::Blue;
        Color type_name   = Color::MagentaLight;
        Color line_number = Color::GrayDark;
        Color gutter_bg   = Color::RGB(20, 20, 20);
    };
    std::optional<Theme> theme;
};

namespace detail_fb {

constexpr std::array<std::string_view, 22> kCommonKeywords = {{
    "if","else","switch","case","for","while","do","return",
    "break","continue","true","false","null","function","class",
    "struct","import","export","const","let","var","nullptr",
}};

inline bool is_keyword(std::string_view w) {
    for (auto k : kCommonKeywords) if (k == w) return true;
    return false;
}

// CamelCase identifier → type (magenta). Alignment note: matches
// code_highlight.cppm type_name = magenta/cyan convention; we use
// MagentaLight to visually distinguish from keywords without conflict.
inline bool is_camel_type(std::string_view w) {
    if (w.size() < 2) return false;
    if (!(w[0] >= 'A' && w[0] <= 'Z')) return false;
    bool has_lower = false;
    for (char c : w) if (c >= 'a' && c <= 'z') { has_lower = true; break; }
    return has_lower;
}

inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c == '_' || c == '$';
}
inline bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

// Detect a line-comment prefix at raw[i] for a given language tag.
// Returns {true, len} on match.
inline std::pair<bool, size_t>
detect_comment(std::string_view raw, size_t i, std::string_view lang) {
    auto eq = [&](const char* s) {
        size_t L = std::strlen(s);
        if (i + L > raw.size()) return false;
        return raw.substr(i, L) == s;
    };
    // Always try // (C-family, JS, TS, Go, Rust, Swift, etc.)
    if (eq("//")) return {true, 2};
    // Shell-style #
    if (raw[i] == '#') {
        if (lang == "py" || lang == "python" || lang == "bash" ||
            lang == "sh" || lang == "zsh" || lang == "ruby" ||
            lang == "yaml" || lang == "yml" || lang == "shell" ||
            lang == "make" || lang == "makefile" || lang == "perl" ||
            lang == "ini" || lang == "conf" || lang == "toml" ||
            lang == "dockerfile" || lang == "docker")
            return {true, 1};
    }
    // -- (SQL, Lua, Ada)
    if (eq("--") &&
        (lang == "sql" || lang == "lua" || lang == "ada" ||
         lang == "haskell"))
        return {true, 2};
    // ; (assembly, lisp)
    if (raw[i] == ';' &&
        (lang == "asm" || lang == "lisp" || lang == "clj" ||
         lang == "scheme" || lang == "clojure"))
        return {true, 1};
    return {false, 0};
}

} // namespace detail_fb

[[nodiscard]] inline Element
RenderHighlightedCodeFallback(std::string_view code,
                              std::string_view language_tag,
                              FallbackOptions opts = {}) {
    FallbackOptions::Theme theme = opts.theme.value_or(FallbackOptions::Theme{});

    // Split into lines
    std::vector<std::string> raw_lines;
    {
        size_t pos = 0;
        while (pos <= code.size()) {
            auto nl = code.find('\n', pos);
            if (nl == std::string::npos) {
                raw_lines.emplace_back(code.substr(pos));
                break;
            }
            raw_lines.emplace_back(code.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }

    int total = (int)raw_lines.size();
    int gw = total > 0
        ? (int)std::format("{}", opts.start_line + total - 1).size()
        : 1;

    Elements out;
    out.reserve(raw_lines.size());

    for (int ln = 0; ln < total; ++ln) {
        std::string raw = raw_lines[ln];
        bool truncated = false;
        if ((int)raw.size() > opts.max_line_len) {
            raw = raw.substr(0, opts.max_line_len);
            truncated = true;
        }

        Elements tok;
        size_t i = 0, N = raw.size();

        while (i < N) {
            // Priority 1: line comment (rest of line)
            auto [cmt_ok, cmt_len] =
                detail_fb::detect_comment(raw, i, language_tag);
            if (cmt_ok) {
                tok.push_back(text(raw.substr(i))
                              | color(theme.comment) | dim);
                i = N;
                break;
            }

            char c = raw[i];

            // Priority 2: string literals (single-line only; multi-line
            // strings would need a state machine — deferred per spec)
            if (c == '"' || c == '\'' || c == '`') {
                char delim = c;
                size_t p = i + 1;
                while (p < N) {
                    if (raw[p] == '\\' && p + 1 < N) { p += 2; continue; }
                    if (raw[p] == delim) { ++p; break; }
                    ++p;
                }
                tok.push_back(text(raw.substr(i, p - i))
                              | color(theme.string_lit));
                i = p;
                continue;
            }

            // Priority 4: numbers (hex/bin/oct/float/int) before identifier
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && i + 1 < N &&
                 std::isdigit(static_cast<unsigned char>(raw[i + 1])))) {
                size_t p = i;
                if (c == '0' && i + 1 < N) {
                    char nxt = raw[i + 1];
                    if (nxt == 'x' || nxt == 'X') {
                        p += 2;
                        while (p < N && std::isxdigit(
                            static_cast<unsigned char>(raw[p]))) ++p;
                    } else if (nxt == 'b' || nxt == 'B') {
                        p += 2;
                        while (p < N && (raw[p] == '0' || raw[p] == '1')) ++p;
                    } else if (nxt == 'o' || nxt == 'O') {
                        p += 2;
                        while (p < N && raw[p] >= '0' && raw[p] <= '7') ++p;
                    } else goto decimal;
                } else {
                decimal:
                    while (p < N && std::isdigit(
                        static_cast<unsigned char>(raw[p]))) ++p;
                    if (p < N && raw[p] == '.') {
                        ++p;
                        while (p < N && std::isdigit(
                            static_cast<unsigned char>(raw[p]))) ++p;
                    }
                    if (p < N && (raw[p] == 'e' || raw[p] == 'E')) {
                        ++p;
                        if (p < N && (raw[p] == '+' || raw[p] == '-')) ++p;
                        while (p < N && std::isdigit(
                            static_cast<unsigned char>(raw[p]))) ++p;
                    }
                }
                // suffixes (f/u/l/ll/d)
                while (p < N && strchr("fFuUlLdDmM", raw[p])) ++p;
                tok.push_back(text(raw.substr(i, p - i))
                              | color(theme.number));
                i = p;
                continue;
            }

            // Identifier → keyword / CamelCase type / function call / plain
            if (detail_fb::is_ident_start(c)) {
                size_t p = i + 1;
                while (p < N && detail_fb::is_ident_char(raw[p])) ++p;
                std::string_view word(raw.data() + i, p - i);
                if (detail_fb::is_keyword(word)) {
                    tok.push_back(text(std::string(word))
                                  | color(theme.keyword) | bold);
                } else if (detail_fb::is_camel_type(word)) {
                    tok.push_back(text(std::string(word))
                                  | color(theme.type_name));
                } else {
                    // Function call: identifier followed (after optional ws)
                    // by '('
                    size_t pp = p;
                    while (pp < N && (raw[pp] == ' ' || raw[pp] == '\t')) ++pp;
                    if (pp < N && raw[pp] == '(') {
                        tok.push_back(text(std::string(word))
                                      | color(theme.function));
                    } else {
                        tok.push_back(text(std::string(word))
                                      | color(theme.plain));
                    }
                }
                i = p;
                continue;
            }

            // Fallback: single plain character
            tok.push_back(text(std::string(1, c)) | color(theme.plain));
            ++i;
        }

        if (truncated) {
            tok.push_back(text(" …[truncated]") | dim | color(Color::Yellow));
        }

        // Compose line with gutter
        Elements row;
        if (opts.show_line_numbers) {
            auto ns = std::format("{:>{}} ", opts.start_line + ln, gw);
            row.push_back(text(ns) | color(theme.line_number)
                          | bgcolor(theme.gutter_bg));
            row.push_back(text("│ ") | color(theme.line_number) | dim);
        }
        row.insert(row.end(), tok.begin(), tok.end());
        out.push_back(hbox(std::move(row)));
    }

    if (out.empty()) out.push_back(text(" (empty)") | dim);

    // Add a small footer tag indicating fallback mode
    out.push_back(hbox({
        filler(),
        text(" fallback syntax (heuristic) ") | dim | color(Color::GrayDark),
    }));
    return vbox(std::move(out));
}

// =====================================================================
// P2 Extra#1 — GlimmerMessage streaming skeleton
// =====================================================================

enum class GlimmerStyle : std::uint8_t {
    Default,
    ThinkingPlaceholder,
    ToolWaiting,
    VoiceTranscribing,
};

namespace detail_gl {

// Per-line width fraction — mimics real paragraph distribution.
inline double line_width_ratio(int idx, int total) {
    if (total <= 1) return 1.0;
    if (idx == 0)            return 1.00;
    if (idx == 1)            return 0.75;
    if (idx == 2)            return 0.90;
    if (idx == total - 1)    return 0.35;
    // mid-sentence variance (stable per-index)
    double r = 0.55 + 0.22 * std::sin((double)idx * 1.37);
    return std::clamp(r, 0.30, 1.0);
}

// Produce a shimmering line using only ░▒▓█ + space.
// frame_shift shifts brightness pattern each frame → movement illusion.
inline std::string shimmer_line(int width, int frame_shift, int seed) {
    static constexpr const char* kGlyphs[4] = {"░","▒","▓","█"};
    std::string out;
    out.reserve(width * 3 + 4);
    for (int x = 0; x < width; ++x) {
        // edge falloff (dimmer near ends)
        double edge = 1.0;
        if (width > 12) {
            double rel = (double)x / (double)width - 0.5;
            edge = 1.0 - std::abs(rel) * 0.7;
            edge = std::max(0.15, edge);
        }
        // brightness wave: 8-pixel period, shifted per frame, seeded per line
        int phase = (x * 3 / 4 + frame_shift / 2 + seed) % 8;
        double wave = 0.5 + 0.5 * std::sin((double)phase * 3.14159 / 4.0);
        int level = (int)std::round((wave * 0.7 + 0.15) * edge * 3.0);
        level = std::clamp(level, 0, 3);
        out += kGlyphs[level];
    }
    return out;
}

inline std::pair<std::string_view, std::string_view>
style_meta(GlimmerStyle s) {
    switch (s) {
        case GlimmerStyle::ThinkingPlaceholder:
            return {"🤖", "Thinking…"};
        case GlimmerStyle::ToolWaiting:
            return {"🔧", "Waiting for tool result…"};
        case GlimmerStyle::VoiceTranscribing:
            return {"🎙", "Transcribing audio…"};
        default:
            return {"✨", "Generating response…"};
    }
}

} // namespace detail_gl

// Interactive component: shimmer lines shift each Render() call via a
// frame counter (per-instance; no std::chrono required).
inline Component MakeGlimmerMessage(GlimmerStyle style,
                                    int min_lines = 3,
                                    int max_lines = 8) {
    struct St {
        int frame = 0;
        GlimmerStyle style;
        int mn, mx;
    };
    auto st = std::make_shared<St>();
    st->style = style;
    st->mn = std::max(1, min_lines);
    st->mx = std::max(st->mn, max_lines);

    return Renderer([st] {
        ++st->frame;
        auto [icon, title] = detail_gl::style_meta(st->style);
        Elements lines;
        lines.push_back(hbox({
            text(" " + std::string(icon) + " "),
            text(std::string(title)) | bold | color(Color::Cyan),
            filler(),
            text(" "),
        }));
        lines.push_back(separator() | dim);

        // Line count cycles slowly through [mn..mx] so it "fills in" over
        // time — avoids the visual being completely static.
        int range = std::max(1, st->mx - st->mn + 1);
        int total = st->mn + ((st->frame / 12) % range);
        total = std::min(st->mx, std::max(st->mn, total));

        static constexpr int kBaseW = 52;
        for (int i = 0; i < total; ++i) {
            double ratio = detail_gl::line_width_ratio(i, total);
            int w = std::max(8, (int)((double)kBaseW * ratio));
            auto shimmer = detail_gl::shimmer_line(w, st->frame, i * 5);
            lines.push_back(hbox({
                text("   "),
                text(shimmer) | color(Color::GrayLight),
                filler(),
            }));
        }
        return vbox(std::move(lines));
    });
}

} // namespace cc::ui::components::partial
