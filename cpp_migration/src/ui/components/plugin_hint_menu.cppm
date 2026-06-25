/// @file plugin_hint_menu.cppm
/// @brief "Recommended for you" plugin hint panel - surfaces proactive
/// suggestions for plugins based on workspace usage evidence, with
/// estimated step-savings, Install / Dismiss / Learn-more actions.
/// Mirrors TS `hooks/plugin_recommendation` and the marketplace banner
/// cards shown at the top of the `/plugin` browse view.
module;

#include <algorithm>
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

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.plugin_hint_menu;

import cc.ui.design.primitives;
import cc.ui.design.tokens;
import cc.ui.design.theme;
import cc.types.types;

export namespace cc::ui::components::plugin_hint_menu {

using namespace ftxui;
using namespace cc::ui::design::primitives;
using namespace cc::ui::design::tokens;
using namespace cc::ui::design::theme;

// ─── Types ───────────────────────────────────────────────────────────────────

/// A single proactive plugin recommendation.  Mirrors TS PluginHint.
struct PluginHint {
    std::string plugin_id;
    std::string display_name;
    std::string hint_reason;
    std::vector<std::string> usage_evidence;
    int usage_count = 0;
    double estimated_savings_pct = 0.0;
    std::string category;
    std::string plugin_icon_emoji;       // e.g. "🚀"
    bool dismissed = false;
};

/// View state for the plugin hint menu.
struct PluginHintMenuState {
    std::vector<PluginHint> hints;
    int selected = 0;
    bool show_dismissed = false;
};

// ─── Filtering Helpers ───────────────────────────────────────────────────────

/// Return indices into `state.hints` for entries that survive the
/// "show dismissed" toggle.
[[nodiscard]] inline std::vector<int> visible_indices(
    const PluginHintMenuState& state) {
    std::vector<int> out;
    out.reserve(state.hints.size());
    for (int i = 0; i < static_cast<int>(state.hints.size()); ++i) {
        if (!state.show_dismissed && state.hints[i].dismissed) continue;
        out.push_back(i);
    }
    return out;
}

/// Truncate a string to `max` codepoints, appending '…' if necessary.
[[nodiscard]] inline std::string truncate(std::string_view s,
                                          std::size_t max) {
    if (s.size() <= max) return std::string(s);
    std::string out(s.substr(0, max > 1 ? max - 1 : 0));
    out.append("…");
    return out;
}

/// Render the step-savings badge.  Color scales: green for >=50%,
/// yellow for >=25%, else red.
[[nodiscard]] inline Element savings_badge(double pct, const Theme&) {
    auto rounded = std::clamp(std::round(pct), 0.0, 999.0);
    Color c = Color::Red;
    if (rounded >= 50.0)      c = Color::Green;
    else if (rounded >= 25.0) c = Color::Yellow;

    std::string label = std::format(" Saves ~{:.0f}% steps ", rounded);
    return hbox({
        text(label) | bold | color(c),
    }) | borderStyled(BorderStyle::ROUNDED, c);
}

// ─── Single Hint Card Component ──────────────────────────────────────────────

class PluginHintCard : public ftxui::ComponentBase {
public:
    PluginHintCard(const PluginHint& hint,
                   int index,
                   const Theme& theme,
                   bool is_selected,
                   std::function<void(int)> on_install,
                   std::function<void(int)> on_dismiss,
                   std::function<void(int)> on_learn_more)
        : hint_(hint),
          index_(index),
          theme_(theme),
          is_selected_(is_selected) {
        using namespace ftxui;

        install_btn_ = Button("Install", [this, cb = std::move(on_install)]() {
            if (cb) cb(index_);
        }, ButtonOption::Animated(theme_.color_for(Role::Primary)));
        Add(install_btn_);

        dismiss_btn_ = Button("Dismiss", [this, cb = std::move(on_dismiss)]() {
            if (cb) cb(index_);
        }, ButtonOption::Simple());
        Add(dismiss_btn_);

        learn_btn_ = Button("Learn more", [this, cb = std::move(on_learn_more)]() {
            if (cb) cb(index_);
        }, ButtonOption::Ascii());
        Add(learn_btn_);
    }

    Element Render() override {
        using namespace ftxui;

        // ── Top line: emoji + name + category ────────────────────
        std::string emoji = hint_.plugin_icon_emoji.empty()
            ? std::string("🔌") : hint_.plugin_icon_emoji;
        auto icon = text(emoji + " ");
        auto name = text(hint_.display_name) | bold
                    | color(theme_.palette->text);
        auto cat = hint_.category.empty()
            ? text("")
            : text(std::format("  ({})", hint_.category))
                  | dim | color(theme_.palette->muted);
        auto top = hbox({ icon, name, cat, filler(),
                          savings_badge(hint_.estimated_savings_pct, theme_) });

        // ── Pitch paragraph ───────────────────────────────────────
        auto pitch = paragraphAlignLeft(
                        hint_.hint_reason.empty()
                            ? std::string("No pitch provided.")
                            : hint_.hint_reason)
                    | color(theme_.palette->text);

        // ── Usage evidence (bullet list, max 3 + overflow) ───────
        Elements bullets;
        const int max_shown = 3;
        int shown = 0;
        for (const auto& ev : hint_.usage_evidence) {
            if (shown >= max_shown) break;
            bullets.push_back(hbox({
                text("    • ") | color(theme_.palette->primary),
                text(truncate(ev, 80)) | dim | color(theme_.palette->subtle),
            }));
            ++shown;
        }
        int overflow = static_cast<int>(hint_.usage_evidence.size()) - shown;
        if (overflow > 0) {
            bullets.push_back(hbox({
                text("      ") | color(theme_.palette->muted),
                text(std::format(" +{} more", overflow))
                    | dim | color(theme_.palette->muted),
            }));
        }
        if (hint_.usage_count > 0 && shown == 0) {
            bullets.push_back(hbox({
                text("    • ") | color(theme_.palette->primary),
                text(std::format("Detected {} relevant operations in this session.",
                                 hint_.usage_count))
                    | dim | color(theme_.palette->subtle),
            }));
        }
        Element evidence = bullets.empty()
            ? (text("") | size(HEIGHT, EQUAL, 0))
            : vbox(std::move(bullets));

        // ── Button row ────────────────────────────────────────────
        auto buttons = hbox({
            install_btn_->Render(),
            text("  "),
            dismiss_btn_->Render() | dim | color(theme_.palette->muted),
            text("  "),
            learn_btn_->Render() | underlined,
        });

        // ── Dismissed overlay ─────────────────────────────────────
        Element overlay = text("");
        if (hint_.dismissed) {
            overlay = hbox({
                filler(),
                text(" [DISMISSED] ") | bold | dim
                    | color(theme_.palette->muted),
            });
        }

        // ── Assemble card ─────────────────────────────────────────
        auto body = vbox({
            top,
            separatorEmpty(),
            pitch,
            separatorEmpty(),
            evidence,
            separatorEmpty(),
            hbox({ buttons, filler(), overlay }),
        });

        auto border_color = is_selected_
            ? theme_.color_for(Role::Primary)
            : theme_.palette->chrome;
        auto style = is_selected_ ? BorderStyle::HEAVY : BorderStyle::ROUNDED;
        return vbox({
            text(""),
            body | borderStyled(style, border_color),
            text(""),
        });
    }

private:
    const PluginHint& hint_;
    int index_;
    Theme theme_;
    bool is_selected_;
    ftxui::Component install_btn_;
    ftxui::Component dismiss_btn_;
    ftxui::Component learn_btn_;
};

// ─── Top-level Menu Component ────────────────────────────────────────────────

class PluginHintMenuBase : public ftxui::ComponentBase {
public:
    PluginHintMenuBase(PluginHintMenuState state,
                       std::function<void(int)> on_install,
                       std::function<void(int)> on_dismiss,
                       std::function<void(int)> on_learn_more,
                       Theme theme)
        : state_(std::move(state)),
          on_install_(std::move(on_install)),
          on_dismiss_(std::move(on_dismiss)),
          on_learn_more_(std::move(on_learn_more)),
          theme_(std::move(theme)) {
        using namespace ftxui;
        show_dismissed_toggle_ = Checkbox("Show dismissed",
                                          &state_.show_dismissed);
        Add(show_dismissed_toggle_);
        rebuild_cards();
    }

    bool OnEvent(Event e) override {
        // Let buttons / checkbox grab input first.
        if (ComponentBase::OnEvent(e)) {
            // If the checkbox state changed via mouse, rebuild cards.
            rebuild_cards();
            return true;
        }

        auto visible = visible_indices(state_);
        if (e == Event::ArrowDown && !visible.empty()) {
            selected_ = std::min(selected_ + 1,
                                 static_cast<int>(visible.size()) - 1);
            rebuild_cards();
            return true;
        }
        if (e == Event::ArrowUp && !visible.empty()) {
            selected_ = std::max(selected_ - 1, 0);
            rebuild_cards();
            return true;
        }
        if (e == Event::Home) { selected_ = 0; return true; }
        if (e == Event::End) {
            auto v = visible_indices(state_);
            selected_ = v.empty() ? 0 : static_cast<int>(v.size()) - 1;
            return true;
        }
        return false;
    }

    Element Render() override {
        using namespace ftxui;

        // ── Header ───────────────────────────────────────────────
        auto title = hbox({
            text(" "),
            text("✨ RECOMMENDED FOR YOU") | bold | underlined | inverted,
            text(" "),
        }) | bgcolor(theme_.palette->primary)
           | color(Color::White);

        auto header = hbox({
            title,
            filler(),
            show_dismissed_toggle_->Render(),
        });

        // ── Cards ────────────────────────────────────────────────
        auto visible = visible_indices(state_);
        if (selected_ >= static_cast<int>(visible.size())) {
            selected_ = std::max(0, static_cast<int>(visible.size()) - 1);
        }
        Elements cards;
        cards.reserve(visible.size());
        // Each card owns Button sub-components whose Render() returns an
        // Element holding, via FTXUI `reflect(box_)`, a reference to the
        // button's own Box member.  The card (and its buttons) must outlive
        // the returned Element tree, since the caller lays the tree out
        // *after* Render() returns.  Retain cards in `card_cache_` for the
        // lifetime of this menu (cleared at the top of the next Render()).
        card_cache_.clear();
        card_cache_.reserve(visible.size());
        for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
            int idx = visible[i];
            bool sel = (i == selected_);
            auto card = Make<PluginHintCard>(
                state_.hints[idx], idx, theme_, sel,
                on_install_, on_dismiss_, on_learn_more_);
            card_cache_.push_back(card);
            cards.push_back(card->Render());
            if (i < static_cast<int>(visible.size()) - 1) {
                cards.push_back(separatorEmpty());
            }
        }
        if (cards.empty()) {
            cards.push_back(
                hbox({ text("  No plugin recommendations right now. 👍")
                              | dim | color(theme_.palette->muted) })
            );
        }

        auto card_list = vbox(std::move(cards)) | yflex;

        // ── Footer summary ───────────────────────────────────────
        int total = static_cast<int>(visible.size());
        int one_based = total == 0 ? 0 : std::min(selected_ + 1, total);
        int dismissed_count = 0;
        for (const auto& h : state_.hints) if (h.dismissed) ++dismissed_count;

        auto footer_left = hbox({
            text(std::format(" {} hint{} visible, {} dismissed",
                             total, total == 1 ? "" : "s",
                             dismissed_count))
                | dim | color(theme_.palette->muted),
        });
        auto footer_right = hbox({
            text(std::format("{}/{}", one_based, total))
                | dim | color(theme_.palette->muted),
        });

        auto footer = hbox({
            footer_left,
            filler(),
            footer_right,
        });

        // ── Assemble ─────────────────────────────────────────────
        return vbox({
            header,
            separator() | color(theme_.palette->subtle),
            separatorEmpty(),
            card_list,
            separator(),
            footer,
        }) | borderRounded | color(theme_.palette->chrome);
    }

    /// Replace the view state in-place.
    void update_state(PluginHintMenuState s) {
        state_ = std::move(s);
        selected_ = 0;
        rebuild_cards();
    }

private:
    void rebuild_cards() {
        // Rebuild logic is lazy — cards are re-created on each render based
        // on `visible_indices()`.  This hook is a safety net for
        // synchronous state updates via `update_state()`.
    }

    PluginHintMenuState state_;
    std::function<void(int)> on_install_;
    std::function<void(int)> on_dismiss_;
    std::function<void(int)> on_learn_more_;
    Theme theme_;
    ftxui::Component show_dismissed_toggle_;
    // Card components built during the most recent Render(); retained so their
    // Button sub-components (and the Box references those buttons' Render()
    // embed in the returned Element tree) stay alive until the caller has
    // finished laying out the tree.  See the comment in Render().
    std::vector<ftxui::Component> card_cache_;
    int selected_ = 0;
};

// ─── Public Constructors ─────────────────────────────────────────────────────

/// Build the plugin hint menu using the current (thread-local) theme.
[[nodiscard]] inline auto BuildPluginHintMenu(
    const PluginHintMenuState& s,
    std::function<void(int)> on_install,
    std::function<void(int)> on_dismiss,
    std::function<void(int)> on_learn_more) -> ftxui::Component {
    Theme theme = current_theme();
    return ftxui::Make<PluginHintMenuBase>(
        s, std::move(on_install), std::move(on_dismiss),
        std::move(on_learn_more), std::move(theme));
}

/// Theme-explicit overload for tests and design previews.
[[nodiscard]] inline auto BuildPluginHintMenu(
    const PluginHintMenuState& s,
    const Theme& theme,
    std::function<void(int)> on_install,
    std::function<void(int)> on_dismiss,
    std::function<void(int)> on_learn_more) -> ftxui::Component {
    return ftxui::Make<PluginHintMenuBase>(
        s, std::move(on_install), std::move(on_dismiss),
        std::move(on_learn_more), theme);
}

} // namespace cc::ui::components::plugin_hint_menu
