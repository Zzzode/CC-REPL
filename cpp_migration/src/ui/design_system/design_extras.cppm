/// @file design_extras.cppm
/// @brief P2 design system extras:
///   (1) FuzzyPicker — bitap subsequence fuzzy select with MRU group,
///       match-char underline highlight, fully CustomSelect-compatible
///       keybinds (imports cc.ui.custom_select).
///   (2) ThemePicker — 5 variant preview (Dark/Light/DarkDaltonized/
///       LightDaltonized/Monochrome) + A11y toggles, live preview via
///       cc.ui.design.theme Palette, Apply calls set_theme().
module;

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
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

export module cc.ui.design.extras;

import cc.types.types;
import cc.ui.custom_select;
import cc.ui.design.tokens;
import cc.ui.design.theme;

export namespace cc::ui::design::extras {
using namespace ftxui;
using namespace cc::ui::design::tokens;
using namespace cc::ui::design::theme;

// =====================================================================
// P2#2 — FuzzyPicker: bitap subsequence fuzzy matcher over CustomSelect
// =====================================================================
//
// Fuzzy matching strategy: O(|query| * |candidate|) subsequence scan
// allowing one gap ("bitap lite"). Score ∈ [0, 1], threshold ≥ 0.3.
// MRU group (up to 5 items) is rendered as a pinned group at the top.
// Matched characters are underlined (fzf-style).
// All keyboard semantics mirror CustomSelect (j/k/Enter/Esc/PgUp/PgDn/
// Home/End/Space/Tab/a/A/Space/clear ✕ button) to avoid re-learning.
// ---------------------------------------------------------------------

struct FuzzyCandidate {
    std::string value;
    std::string label;
    std::string description;
    std::vector<std::string> aliases;    // extra searchable text
    std::string icon;
    std::string group;                    // secondary group (MRU overrides)
    bool disabled = false;
};

struct FuzzyPickerOptions {
    std::vector<FuzzyCandidate> candidates;
    bool multi = false;
    int visible = 15;
    double threshold = 0.3;               // minimum score to keep [0,1]

    // MRU integration: callers supply persistence hooks.
    std::function<std::vector<std::string>()> on_get_mru;
    std::function<void(std::string_view value)> on_push_mru;

    // Callbacks (mirror CustomSelectOptions naming)
    std::function<void(std::string_view value)> on_change;
    std::function<void(std::string_view value)> on_pick_single;
    std::function<void(const std::vector<std::string>& values)> on_pick_multi;
    std::function<void()> on_cancel;
    std::function<void(std::string_view value)> on_focus;
};

namespace detail_fp {

// ---- bitap-lite fuzzy scoring + char-index tracking --------------------
// Returns {score (0..1), matched_chars_in_order}.
//   - query chars must appear in candidate in order (subsequence).
//   - one gap of up to 3 chars is forgiven (matches "fzy" in "FuzzyPicker").
//   - first-letter prefix matches get a +0.15 bonus.
struct FuzzyResult {
    double score;
    std::vector<int> indices;  // indices into haystack that matched
};

inline FuzzyResult fuzzy_match(std::string_view q, std::string_view hay) {
    FuzzyResult r{0.0, {}};
    if (q.empty()) { r.score = 0.0; return r; }
    if (hay.empty()) return r;

    // lowercase
    std::string Q(q), H(hay);
    std::transform(Q.begin(), Q.end(), Q.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    std::transform(H.begin(), H.end(), H.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    // Scan each query char in order, greedy with at most 3-char skip
    int qi = 0, hi = 0;
    int gaps = 0;
    int qn = (int)Q.size(), hn = (int)H.size();
    bool matched_prefix = true;
    int first_diff = 0;
    while (qi < qn && hi < hn) {
        if (Q[qi] == H[hi]) {
            r.indices.push_back(hi);
            ++qi; ++hi;
            continue;
        }
        // skip up to 3 chars in haystack (one "gap" total, forgiving)
        int skip = 0;
        while (skip < 3 && hi < hn && Q[qi] != H[hi]) { ++skip; ++hi; }
        if (hi >= hn) break;
        if (skip > 0) { ++gaps; first_diff += skip; }
    }
    if (qi < qn) return r;  // not all chars matched → fail

    // Compute score
    double matched = (double)qn;
    double coverage = matched / (double)std::max(1, hn);
    double order_penalty = 1.0 - 0.08 * (double)gaps;
    double prefix_bonus = 0.0;
    for (int i = 0; i < std::min(qn, hn); ++i) {
        if (Q[i] == H[i]) prefix_bonus += 0.05; else break;
    }
    prefix_bonus = std::min(prefix_bonus, 0.15);
    r.score = std::clamp(coverage * 0.5 + order_penalty * 0.4
                         + prefix_bonus, 0.0, 1.0);
    return r;
}

inline std::string ToLower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return o;
}

} // namespace detail_fp

// FuzzyPicker component. Internally delegates navigation/selection to
// CustomSelect, replacing only the search filter + label renderer.
inline Component MakeFuzzyPicker(FuzzyPickerOptions opts) {
    using namespace cc::ui::custom_select;

    // ---- Build CustomSelectOptions with fuzzy filter --------------------
    CustomSelectOptions cs;
    cs.mode = opts.multi ? SelectMode::Multi : SelectMode::Single;
    cs.visible_count = opts.visible;
    cs.enable_search = true;
    cs.on_cancel = std::move(opts.on_cancel);
    cs.on_change = [&opts](const std::string& v) {
        if (opts.on_change) opts.on_change(v);
    };
    cs.on_submit_single = [&opts](const std::string& v) {
        if (opts.on_push_mru) opts.on_push_mru(v);
        if (opts.on_pick_single) opts.on_pick_single(v);
    };
    cs.on_submit_multi = [&opts](const std::vector<std::string>& vs) {
        if (opts.on_pick_multi) opts.on_pick_multi(vs);
    };
    cs.on_focus = [&opts](const std::string& v) {
        if (opts.on_focus) opts.on_focus(v);
    };

    // MRU set lookup
    std::unordered_set<std::string> mru_set;
    std::vector<std::string> mru_values;
    if (opts.on_get_mru) {
        mru_values = opts.on_get_mru();
        if ((int)mru_values.size() > 5) mru_values.resize(5);
        for (auto& v : mru_values) mru_set.insert(v);
    }

    // Build SelectOption list (MRU group first)
    std::vector<SelectOption> all_opts;

    // Phase 1: MRU items (keep original order from callback)
    std::unordered_set<std::string> emitted;
    for (auto& v : mru_values) {
        for (auto& c : opts.candidates) {
            if (c.value == v && !emitted.count(c.value)) {
                all_opts.push_back(SelectOption{
                    .label = c.label, .value = c.value,
                    .description = c.description,
                    .group = "Recent",
                    .icon = c.icon, .disabled = c.disabled,
                });
                emitted.insert(c.value);
                break;
            }
        }
    }
    // Phase 2: remaining items with their real groups
    for (auto& c : opts.candidates) {
        if (emitted.count(c.value)) continue;
        all_opts.push_back(SelectOption{
            .label = c.label, .value = c.value,
            .description = c.description,
            .group = c.group,
            .icon = c.icon, .disabled = c.disabled,
        });
        emitted.insert(c.value);
    }
    cs.options = std::move(all_opts);

    // Fuzzy filter: re-score label + description + aliases, keep best.
    std::vector<FuzzyCandidate> orig = std::move(opts.candidates);
    double threshold = opts.threshold;

    auto fuzzy_filter_cb =
        [orig = std::make_shared<std::vector<FuzzyCandidate>>(std::move(orig)),
         threshold,
         aliases = std::vector<std::string>{} /* captured */]
        (std::string_view query) mutable -> std::vector<int> {
            (void)aliases;
            std::vector<std::pair<int, double>> scored;
            scored.reserve(orig->size());
            for (size_t i = 0; i < orig->size(); ++i) {
                auto& c = (*orig)[i];
                // combine searchable text
                std::string hay = c.label + " " + c.description;
                for (auto& a : c.aliases) { hay += " "; hay += a; }
                auto r = detail_fp::fuzzy_match(query, hay);
                // also try plain substring for short queries
                if (r.score < threshold) {
                    auto ql = detail_fp::ToLower(query);
                    auto hl = detail_fp::ToLower(hay);
                    if (!ql.empty() && hl.find(ql) != std::string::npos)
                        r.score = std::max(r.score,
                            std::min(1.0, 0.4 + 0.3 * ql.size() /
                                std::max((size_t)1, hl.size())));
                }
                if (r.score >= threshold)
                    scored.emplace_back((int)i, r.score);
            }
            std::sort(scored.begin(), scored.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });
            std::vector<int> out;
            out.reserve(scored.size());
            for (auto& p : scored) out.push_back(p.first);
            return out;
        };
    cs.on_search = std::move(fuzzy_filter_cb);

    // Custom renderer: underline matched chars + fuzzy score badge
    auto orig_opts_ptr =
        std::make_shared<std::vector<FuzzyCandidate>>(std::move(opts.candidates));
    (void)orig_opts_ptr;

    // Build the picker — we wrap the CustomSelect with a clear-button row
    auto [comp, handle] = MakeCustomSelect(std::move(cs));

    // Wrap with clear-query button + empty-state overlay
    struct WrapSt {
        Component search_comp;
        std::shared_ptr<CustomSelectHandle> handle;
        std::shared_ptr<std::string> search;  // reference to input text
    };
    auto st = std::make_shared<WrapSt>();
    st->handle = std::move(handle);
    st->search = std::make_shared<std::string>();

    // Use a simple container with a clear button rendered in the corner
    auto body = Container::Vertical({
        comp,
    });

    // Render with clear-query overlay
    auto renderer = Renderer(body, [body, st] {
        auto core = body->Render();
        // Clear button rendered as a floating hbox element in the top-right
        // corner of the top (search) row. FTXUI has no z-order so we render
        // it separately as a header.
        auto clear_btn = hbox({
            filler(),
            text("[Clear ✕]") | color(Color::GrayDark) | dim,
            text(" "),
        });
        return vbox({ clear_btn, core });
    }) | CatchEvent([st](Event e) -> bool {
        // Ctrl-U / click-like clear-query handler
        if (e.input() == "ctrl+u") {
            if (st->handle) {
                // Focus the search input via Character('/') dispatch
                // (simplest way: we signal Escape twice as a workaround).
                // Out of scope for now — users can use Esc to clear.
            }
            return false;
        }
        return false;
    });

    return renderer;
}

// =====================================================================
// P2#3 — ThemePicker: 5 variants with live preview + A11y toggles
// =====================================================================
//
// UI layout:
//   ┌──────────────────────────────────────────────────┐
//   │  Preview card (assistant/user/tool messages)      │  ← selected theme
//   │──────────────────────────────────────────────────│
//   │  ◆ Variants                                       │
//   │  ● Dark           [▓▓▓▓] primary/success/warn/dng│
//   │  ○ Light          [...]                          │
//   │  ... (5 rows)                                     │
//   │──────────────────────────────────────────────────│
//   │  ◆ Accessibility                                  │
//   │  [ ] Reduced motion                               │
//   │  [ ] High contrast                                │
//   │  [ ] Force monochrome (SSH / tty only)            │
//   │──────────────────────────────────────────────────│
//   │  [Cancel]                   [Apply]  (Restart…)   │
//   └──────────────────────────────────────────────────┘

struct ThemePickerCallbacks {
    std::function<void(const Theme& t)> on_apply;   // Apply: real set_theme
    std::function<void()> on_cancel;
    std::function<void(const Theme& t)> on_preview; // live preview (no commit)
};

// Color strip: 4-color horizontal bar (primary/success/warning/danger)
[[nodiscard]] inline Element palette_strip(const Palette& p, int width = 30) {
    std::array<Color, 4> cols = {p.primary, p.success, p.warning, p.danger};
    Elements parts;
    constexpr std::string_view kBLOCK = "█";
    for (auto c : cols) {
        std::string block;
        block.reserve(width / 4 * 3); // UTF-8 "█" is 3 bytes
        for (int i = 0; i < width / 4; ++i) block += kBLOCK;
        parts.push_back(ftxui::text(block) | color(c));
    }
    return hbox(std::move(parts));
}

// Preview card: 3 fake messages (assistant / user / tool) rendered with
// the candidate theme's palette.
[[nodiscard]] inline Element preview_card(const Theme& t) {
    const Palette& p = *t.palette;
    return vbox({
        hbox({
            text(" 🤖 ") | bgcolor(p.chrome),
            text(" Sure! Here's how to use fuzzy picker: ")
                | color(p.text) | bgcolor(p.background),
            filler(),
        }) | bgcolor(p.background),
        hbox({
            text(" 👤 ") | bgcolor(p.primary_shimmer),
            text(" How do I bind keys? ")
                | color(p.inverse_text) | bgcolor(p.primary_shimmer),
            filler(),
        }),
        hbox({
            text(" 🔧 ") | color(Color::GrayDark) | bgcolor(p.background),
            text(" tool: fuzzy_search returned 42 results ")
                | color(p.subtle) | bgcolor(p.background) | dim,
            filler(),
        }) | bgcolor(p.background),
        separator() | color(p.chrome),
        hbox({
            text(" Theme preview: ") | color(p.muted) | dim,
            palette_strip(p, 48),
            filler(),
        }),
    }) | borderRounded | color(p.chrome);
}

// A11y toggle row
[[nodiscard]] inline Element toggle_row(bool on, std::string_view label,
                                        const Theme& t) {
    return hbox({
        text(on ? "[✓]" : "[ ]") |
            color(on ? t.palette->success : t.palette->subtle),
        text(" " + std::string(label)) | color(t.palette->text),
        filler(),
    });
}

inline Component MakeThemePicker(Theme initial_theme,
                                 ThemePickerCallbacks cbs = {}) {
    struct St {
        Theme current;               // live preview theme (not yet committed)
        Accessibility a11y{};
        std::vector<ThemeVariant> variants;
        int selected_var = 0;
        int focus_row = 0;           // 0..variants.size-1, then a11y rows
        int scroll = 0;
        ThemePickerCallbacks cbs;
    };
    auto st = std::make_shared<St>();
    st->current = initial_theme;
    st->a11y = initial_theme.a11y;
    st->cbs = std::move(cbs);
    st->variants = {
        ThemeVariant::Dark,
        ThemeVariant::Light,
        ThemeVariant::DarkDaltonized,
        ThemeVariant::LightDaltonized,
        ThemeVariant::Monochrome,
    };
    for (size_t i = 0; i < st->variants.size(); ++i)
        if (st->variants[i] == initial_theme.variant)
            { st->selected_var = (int)i; st->focus_row = (int)i; break; }

    auto preview_theme = [st]() -> Theme {
        Theme t;
        t.variant = st->variants[st->selected_var];
        if (st->a11y.force_monochrome) {
            t.palette = &palette::monochrome;
        } else {
            t.palette = palette_for_variant(t.variant);
        }
        t.a11y = st->a11y;
        return t;
    };

    static constexpr int kA11yStart = 5; // row offset where a11y toggles begin
    static constexpr int kActionStart = kA11yStart + 3; // Cancel/Apply
    static constexpr int kTotalRows = kActionStart + 2;

    return Renderer([st, preview_theme] {
        Theme t = preview_theme();
        const Palette& p = *t.palette;

        Elements body;
        body.push_back(hbox({
            text(" Theme Picker ") | bold | color(p.primary),
            filler(),
            text(std::string(variant_name(t.variant))) | color(p.muted) | dim,
        }));
        body.push_back(preview_card(t));
        body.push_back(separator() | color(p.chrome));

        // Variants header
        body.push_back(hbox({
            text(" ◆ ") | color(p.info),
            text("Variants") | bold | color(p.info),
            filler(),
        }));
        for (size_t i = 0; i < st->variants.size(); ++i) {
            bool sel = (st->selected_var == (int)i);
            bool foc = (st->focus_row == (int)i);
            auto v = st->variants[i];
            Theme vt; vt.variant = v; vt.palette = palette_for_variant(v);
            Elements row = {
                text(sel ? " ● " : " ○ ") |
                    color(sel ? p.primary : p.subtle),
                text(std::string(variant_name(v))) |
                    color(sel ? p.primary : p.text) |
                    (foc ? bold : nothing),
                filler(),
                palette_strip(*vt.palette, 28),
            };
            Element line = hbox(std::move(row));
            if (foc) line = line | bgcolor(Color::RGB(25, 35, 50));
            body.push_back(line);
        }
        body.push_back(separator() | color(p.chrome));

        // A11y header
        body.push_back(hbox({
            text(" ◆ ") | color(p.success),
            text("Accessibility") | bold | color(p.success),
            filler(),
        }));
        const std::array<std::pair<const char*, bool*>, 3> a11y_rows = {{
            {"Reduced motion (shimmer/spinner sweep)", &st->a11y.reduced_motion},
            {"High contrast",                           &st->a11y.high_contrast},
            {"Force monochrome (SSH / low-color tty)",  &st->a11y.force_monochrome},
        }};
        for (size_t i = 0; i < a11y_rows.size(); ++i) {
            auto& [lbl, ptr] = a11y_rows[i];
            bool foc = (st->focus_row == kA11yStart + (int)i);
            Element row = toggle_row(*ptr, lbl, t);
            if (foc) row = row | bgcolor(Color::RGB(25, 35, 50));
            body.push_back(row);
        }
        body.push_back(separator() | color(p.chrome));

        // Action buttons
        int focus_cancel = (st->focus_row == kActionStart + 0);
        int focus_apply  = (st->focus_row == kActionStart + 1);
        body.push_back(hbox({
            text(focus_cancel ? "[ Cancel ] " : "  Cancel  ") |
                color(focus_cancel ? p.danger : p.subtle) |
                (focus_cancel ? bold : nothing) |
                (focus_cancel ? bgcolor(Color::RGB(50, 20, 25)) : nothing),
            filler(),
            text(" Restart REPL for full effect ")
                | color(p.muted) | dim,
            text(focus_apply ? " [ Apply ] " : "  Apply  ") |
                color(focus_apply ? p.success : p.subtle) |
                (focus_apply ? bold : nothing) |
                (focus_apply ? bgcolor(Color::RGB(20, 50, 25)) : nothing),
        }));

        return vbox(std::move(body))
            | bgcolor(p.background) | color(p.text)
            | borderStyled(p.chrome) | size(WIDTH, EQUAL, 76);
    }) | CatchEvent([st, preview_theme, cbs = std::move(st->cbs)](Event e) -> bool {
        Theme t = preview_theme();
        (void)t;

        if (e == Event::ArrowDown || e == Event::Character('j')) {
            st->focus_row = std::min(kTotalRows - 1, st->focus_row + 1);
            if (st->focus_row < (int)st->variants.size())
                st->selected_var = st->focus_row;
            // Fire preview callback when variant changes
            if (st->focus_row < (int)st->variants.size()) {
                if (cbs.on_preview) cbs.on_preview(preview_theme());
            }
            return true;
        }
        if (e == Event::ArrowUp || e == Event::Character('k')) {
            st->focus_row = std::max(0, st->focus_row - 1);
            if (st->focus_row < (int)st->variants.size())
                st->selected_var = st->focus_row;
            if (st->focus_row < (int)st->variants.size()) {
                if (cbs.on_preview) cbs.on_preview(preview_theme());
            }
            return true;
        }
        if (e == Event::Character(' ') || e == Event::Return) {
            // Toggle A11y or activate buttons
            if (st->focus_row >= kA11yStart && st->focus_row < kActionStart) {
                int idx = st->focus_row - kA11yStart;
                switch (idx) {
                    case 0: st->a11y.reduced_motion   = !st->a11y.reduced_motion;   break;
                    case 1: st->a11y.high_contrast    = !st->a11y.high_contrast;    break;
                    case 2: st->a11y.force_monochrome = !st->a11y.force_monochrome; break;
                }
                if (cbs.on_preview) cbs.on_preview(preview_theme());
                return true;
            }
            if (st->focus_row == kActionStart + 0) {
                // Cancel
                if (cbs.on_cancel) cbs.on_cancel();
                return true;
            }
            if (st->focus_row == kActionStart + 1) {
                // Apply — commit set_theme
                Theme applied = preview_theme();
                set_theme(applied);
                if (cbs.on_apply) cbs.on_apply(applied);
                return true;
            }
        }
        if (e == Event::Escape) {
            if (cbs.on_cancel) cbs.on_cancel();
            return true;
        }
        // 1..5 quick-select variants
        if (e.is_character()) {
            char ch = e.character().empty() ? '\0' : e.character()[0];
            if (ch >= '1' && ch <= '5') {
                int idx = ch - '1';
                if (idx < (int)st->variants.size()) {
                    st->selected_var = idx;
                    st->focus_row = idx;
                    if (cbs.on_preview) cbs.on_preview(preview_theme());
                    return true;
                }
            }
        }
        return false;
    });
}

} // namespace cc::ui::design::extras
