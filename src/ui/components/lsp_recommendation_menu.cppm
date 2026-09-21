/// @file lsp_recommendation_menu.cppm
/// @brief LSP plugin recommendation menu - browse, filter and install
/// recommended LSP servers from the marketplace.  Mirrors the TS
/// `/plugin browse` LSP-recommendation sub-panel and the hooks that
/// surface "Recommended LSP" entries when an unhandled language is
/// detected in the workspace.
module;

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

export module cc.ui.components.lsp_rec_menu;

import cc.ui.design.primitives;
import cc.ui.design.tokens;
import cc.ui.design.theme;
import cc.types.types;

export namespace cc::ui::components::lsp_rec_menu {

using namespace ftxui;
using namespace cc::ui::design::primitives;
using namespace cc::ui::design::tokens;
using namespace cc::ui::design::theme;

// ─── Types ───────────────────────────────────────────────────────────────────

/// Why this plugin is being recommended to the user.
enum class RecommendReason {
    DetectedLangInWorkspace,
    UserAsked,
    PopularInCategory,
    SimilarToInstalled,
};

/// A single LSP plugin recommendation entry.  Mirrors TS
/// LspPluginRecommendation.
struct LspPluginRecommendation {
    std::string plugin_id;
    std::string display_name;
    std::string description;
    std::vector<std::string> language_ids;
    uint64_t install_count = 0;
    double rating = 0.0;            // 0.0 - 5.0
    std::string version;
    std::string publisher;
    bool already_installed = false;
    bool trusted = false;
    RecommendReason reason = RecommendReason::DetectedLangInWorkspace;
};

/// View state for the recommendation menu.
struct LspRecMenuState {
    std::vector<LspPluginRecommendation> items;
    int selected = 0;
    std::string filter_language;
    std::string search_term;
    bool show_installed_only = false;
};

// ─── Rendering Helpers ───────────────────────────────────────────────────────

/// Format a rating (0.0..5.0) as a 5-glyph star string: ★ for each full
/// unit, ☆ for the remainder (rounded to the nearest integer).
[[nodiscard]] inline std::string rating_stars(double rating) {
    int full = static_cast<int>(std::round(std::clamp(rating, 0.0, 5.0)));
    std::string out;
    out.reserve(15);
    for (int i = 0; i < full; ++i) out.append("★");
    for (int i = full; i < 5; ++i) out.append("☆");
    return out;
}

/// Format an install count with k / M suffixes.
[[nodiscard]] inline std::string format_installs(uint64_t n) {
    if (n < 1000)        return std::to_string(n);
    if (n < 1'000'000)   return std::format("{:.1f}k", n / 1000.0);
    return std::format("{:.1f}M", n / 1'000'000.0);
}

/// Human-readable "Recommended because ..." sentence for each reason.
[[nodiscard]] inline std::string reason_hint(RecommendReason r,
                                             std::string_view lang_id) {
    switch (r) {
        case RecommendReason::DetectedLangInWorkspace:
            return lang_id.empty()
                ? std::string("Detected an unhandled language in the workspace.")
                : std::format("Detected {} files in the workspace.", lang_id);
        case RecommendReason::UserAsked:
            return "You asked for an LSP server for this language.";
        case RecommendReason::PopularInCategory:
            return "This is one of the most popular plugins in its category.";
        case RecommendReason::SimilarToInstalled:
            return "Behaves like another plugin you already have installed.";
    }
    return "Recommended.";
}

/// Extract the unique, sorted list of language ids from a list of items.
[[nodiscard]] inline std::vector<std::string> unique_languages(
    const std::vector<LspPluginRecommendation>& items) {
    std::set<std::string> seen;
    for (const auto& it : items) {
        for (const auto& l : it.language_ids) seen.insert(l);
    }
    return std::vector<std::string>(seen.begin(), seen.end());
}

/// Apply filter (search term + language + installed-only) and return the
/// matching indices into the original list.
[[nodiscard]] inline std::vector<int> apply_filter(
    const LspRecMenuState& s) {
    std::vector<int> out;
    auto term = s.search_term;
    std::transform(term.begin(), term.end(), term.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
        const auto& item = s.items[i];
        if (s.show_installed_only && !item.already_installed) continue;
        if (!s.filter_language.empty()) {
            auto it = std::find(item.language_ids.begin(),
                                item.language_ids.end(),
                                s.filter_language);
            if (it == item.language_ids.end()) continue;
        }
        if (!term.empty()) {
            auto name = item.display_name;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            auto desc = item.description;
            std::transform(desc.begin(), desc.end(), desc.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            auto id = item.plugin_id;
            std::transform(id.begin(), id.end(), id.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            bool hit = name.find(term) != std::string::npos ||
                       desc.find(term) != std::string::npos ||
                       id.find(term) != std::string::npos;
            if (!hit) continue;
        }
        out.push_back(i);
    }
    return out;
}

// ─── Row Component (per recommendation) ──────────────────────────────────────

/// One menu row: rating + name + description + badges + action buttons.
class LspRecRow : public ftxui::ComponentBase {
public:
    LspRecRow(const LspPluginRecommendation& item,
              int index,
              const Theme& theme,
              std::function<void(int)> on_install,
              std::function<void(int)> on_skip,
              bool is_selected)
        : item_(item),
          index_(index),
          theme_(theme),
          is_selected_(is_selected) {
        using namespace ftxui;
        if (!item_.already_installed) {
            install_btn_ = Button("Install", [this, cb = std::move(on_install)]() {
                if (cb) cb(index_);
            }, ButtonOption::Animated(theme_.color_for(Role::Primary)));
            Add(install_btn_);
            skip_btn_ = Button("Skip", [this, cb = std::move(on_skip)]() {
                if (cb) cb(index_);
            }, ButtonOption::Simple());
            Add(skip_btn_);
        }
    }

    Element Render() override {
        using namespace ftxui;

        // Rating stars + average.
        auto stars = text(rating_stars(item_.rating))
                     | color(Color::Yellow);
        auto rating_num = text(std::format(" {:.1f} ", item_.rating))
                          | dim | color(theme_.palette->muted);

        // Display name.
        auto name = text(item_.display_name) | bold
                    | color(theme_.palette->text);

        // Description (dim, truncated).
        auto desc = paragraphAlignLeft(
                        item_.description.empty()
                            ? std::string("(no description)")
                            : item_.description)
                    | dim | color(theme_.palette->muted);

        // Publisher.
        auto pub = text(std::format("  by {}",
                                    item_.publisher.empty()
                                        ? std::string("(unknown)")
                                        : item_.publisher))
                   | color(theme_.palette->subtle) | dim;

        // Install count.
        auto installs = hbox({
            text("⬇ ") | color(theme_.palette->subtle),
            text(format_installs(item_.install_count)) | bold
                | color(theme_.palette->subtle),
        });

        // Trust badge.
        Element trust = text("");
        if (item_.trusted) {
            trust = hbox({
                text(" "),
                text("✓TRUSTED") | bold | color(Color::Green) | dim,
                text(" "),
            });
        }

        // Version badge.
        Element version_badge = text("");
        if (!item_.version.empty()) {
            version_badge = hbox({
                text(" "),
                text("v" + item_.version) | color(theme_.palette->muted) | dim,
                text(" "),
            });
        }

        // Language badges.
        Elements lang_badges;
        for (const auto& lang : item_.language_ids) {
            lang_badges.push_back(hbox({
                text(" "),
                text(lang) | bold | color(theme_.palette->text) |
                    bgcolor(theme_.palette->background),
                text(" "),
            }) | color(theme_.palette->chrome));
            lang_badges.push_back(text(" "));
        }

        // Reason hint.
        std::string primary_lang = item_.language_ids.empty()
            ? std::string{} : item_.language_ids.front();
        auto reason = text(std::format("  ⓘ {}",
                                       reason_hint(item_.reason, primary_lang)))
                      | dim | color(theme_.palette->muted);

        // Right side: INSTALLED badge or Install / Skip buttons.
        Element right;
        if (item_.already_installed) {
            right = hbox({
                text(" "),
                text("INSTALLED") | bold | inverted
                    | color(Color::White)
                    | bgcolor(Color::Green),
                text(" "),
            });
        } else {
            right = hbox({
                install_btn_ ? install_btn_->Render() : text(""),
                text(" "),
                skip_btn_    ? skip_btn_->Render() | dim : text(""),
            });
        }

        // Assemble the row.
        auto main_row = hbox({
            stars, rating_num,
            text(" "), name, pub,
            filler(),
            installs, trust, version_badge,
            text(" "),
            right,
        });

        auto badges_row = hbox({
            text("    "),
            hbox(std::move(lang_badges)) | xflex,
        });

        auto desc_row = hbox({
            text("    "),
            desc | xflex,
        });

        auto reason_row = hbox({
            text("    "),
            reason | xflex,
        });

        auto body = vbox({
            main_row,
            badges_row,
            desc_row,
            reason_row,
        });

        if (is_selected_) {
            body = vbox({
                text(""),
                body | borderStyled(BorderStyle::HEAVY,
                                    theme_.color_for(Role::Primary)),
                text(""),
            });
        } else {
            body = vbox({
                text(""),
                body | borderStyled(BorderStyle::LIGHT,
                                    theme_.palette->chrome),
                text(""),
            });
        }
        return body;
    }

private:
    const LspPluginRecommendation& item_;
    int index_;
    Theme theme_;
    bool is_selected_;
    ftxui::Component install_btn_;
    ftxui::Component skip_btn_;
};

// ─── Top-level Menu Component ────────────────────────────────────────────────

class LspRecMenuBase : public ftxui::ComponentBase {
public:
    LspRecMenuBase(LspRecMenuState state,
                   std::function<void(int)> on_install,
                   std::function<void(int)> on_skip,
                   Theme theme)
        : state_(std::move(state)),
          on_install_(std::move(on_install)),
          on_skip_(std::move(on_skip)),
          theme_(std::move(theme)) {
        using namespace ftxui;

        // Build filter controls.
        search_input_ = Input(&state_.search_term, "Search plugins…");
        Add(search_input_);

        installed_checkbox_ = Checkbox("Show installed only",
                                       &state_.show_installed_only);
        Add(installed_checkbox_);

        // Radiobox-style list of languages.
        languages_ = unique_languages(state_.items);
        language_labels_.reserve(languages_.size() + 1);
        language_labels_.push_back("All");
        for (const auto& l : languages_) language_labels_.push_back(l);
        lang_selected_ = 0;
        lang_radiobox_ = Radiobox(&language_labels_, &lang_selected_);
        Add(lang_radiobox_);

        // Build the vertical list of row components.
        rebuild_rows();
    }

    bool OnEvent(Event e) override {
        // Let FTXUI first dispatch to focusable children (buttons, inputs).
        if (ComponentBase::OnEvent(e)) return true;

        // Apply language filter state on any event that could change it.
        if (lang_selected_ == 0) {
            state_.filter_language.clear();
        } else if (lang_selected_ > 0 &&
                   lang_selected_ <= static_cast<int>(languages_.size())) {
            state_.filter_language = languages_[lang_selected_ - 1];
        }

        // Row navigation on the filtered list.
        auto filtered = apply_filter(state_);
        if (e == Event::ArrowDown && !filtered.empty()) {
            selected_ = std::min(selected_ + 1,
                                 static_cast<int>(filtered.size()) - 1);
            rebuild_rows();
            return true;
        }
        if (e == Event::ArrowUp && !filtered.empty()) {
            selected_ = std::max(selected_ - 1, 0);
            rebuild_rows();
            return true;
        }

        return false;
    }

    Element Render() override {
        using namespace ftxui;

        // Filter row: search input + installed-only checkbox.
        auto top_row = hbox({
            text("🔍 ") | color(theme_.palette->muted),
            search_input_->Render() | xflex | borderStyled(theme_.palette->chrome),
            text("  "),
            installed_checkbox_->Render(),
        });

        // Language filter row.
        auto filter_row = hbox({
            text(" Languages: ") | bold | color(theme_.palette->subtle),
            lang_radiobox_->Render(),
            filler(),
        });

        // Menu rows.
        auto filtered = apply_filter(state_);
        if (selected_ >= static_cast<int>(filtered.size())) {
            selected_ = std::max(0, static_cast<int>(filtered.size()) - 1);
        }
        Elements rows;
        rows.reserve(filtered.size());
        // Each row component owns Button sub-components whose `Render()`
        // returns an Element that, via FTXUI's `reflect(box_)`, holds a
        // reference to the Button's own Box member.  The row (and therefore
        // its buttons) must outlive the returned Element tree, since the
        // caller lays the tree out *after* Render() returns.  We therefore
        // retain the row components in `row_cache_` for the lifetime of this
        // menu (cleared at the top of the next Render()).
        row_cache_.clear();
        row_cache_.reserve(filtered.size());
        for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
            int original_idx = filtered[i];
            bool sel = (i == selected_);
            auto row = Make<LspRecRow>(
                state_.items[original_idx],
                original_idx,
                theme_,
                on_install_, on_skip_, sel);
            row_cache_.push_back(row);
            rows.push_back(row->Render());
        }
        if (rows.empty()) {
            rows.push_back(
                hbox({ text("  (no matching plugins)") | dim
                                             | color(theme_.palette->muted) })
            );
        }

        auto results_count = hbox({
            text(std::format(" Showing {} of {} recommendations ",
                             filtered.size(), state_.items.size()))
                | dim | color(theme_.palette->muted),
        });

        auto list = vbox(std::move(rows)) | yflex;

        return vbox({
            top_row,
            separatorEmpty(),
            filter_row,
            separator() | color(theme_.palette->subtle),
            results_count,
            separatorEmpty(),
            list,
        }) | borderRounded | color(theme_.palette->chrome);
    }

    void update_state(LspRecMenuState s) {
        state_ = std::move(s);
        languages_ = unique_languages(state_.items);
        language_labels_.clear();
        language_labels_.push_back("All");
        for (const auto& l : languages_) language_labels_.push_back(l);
        lang_selected_ = 0;
        rebuild_rows();
    }

private:
    void rebuild_rows() {
        selected_ = 0;
    }

    LspRecMenuState state_;
    std::function<void(int)> on_install_;
    std::function<void(int)> on_skip_;
    Theme theme_;

    ftxui::Component search_input_;
    ftxui::Component installed_checkbox_;
    ftxui::Component lang_radiobox_;

    // Row components built during the most recent Render(); retained so their
    // Button sub-components (and the Box references those buttons' Render()
    // embed in the returned Element tree) stay alive until the caller has
    // finished laying out the tree.  See the comment in Render().
    std::vector<ftxui::Component> row_cache_;

    std::vector<std::string> languages_;
    std::vector<std::string> language_labels_;
    int lang_selected_ = 0;
    int selected_ = 0;
};

// ─── Public Constructors ─────────────────────────────────────────────────────

/// Build the LSP recommendation menu using the current (thread-local) theme.
[[nodiscard]] inline auto BuildLspRecommendationMenu(
    const LspRecMenuState& s,
    std::function<void(int)> on_install,
    std::function<void(int)> on_skip) -> ftxui::Component {
    Theme theme = current_theme();
    return ftxui::Make<LspRecMenuBase>(s, std::move(on_install),
                                       std::move(on_skip), std::move(theme));
}

/// Theme-explicit overload for tests and design previews.
[[nodiscard]] inline auto BuildLspRecommendationMenu(
    const LspRecMenuState& s,
    const Theme& theme,
    std::function<void(int)> on_install,
    std::function<void(int)> on_skip) -> ftxui::Component {
    return ftxui::Make<LspRecMenuBase>(s, std::move(on_install),
                                       std::move(on_skip), theme);
}

} // namespace cc::ui::components::lsp_rec_menu
