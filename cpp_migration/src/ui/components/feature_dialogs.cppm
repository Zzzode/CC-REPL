/// @file feature_dialogs.cppm
/// @brief Feature-gated dialog components (MemoryFileSelector, Feedback NPS,
///        KAIROS Grove knowledge-graph view).
///
/// All three migrated components are always compiled in the native build.
/// Runtime feature flags still decide whether call sites receive the real
/// component or a disabled-state fallback:
///      `config::feature("proactive_memory" | "user_feedback"
///                      | "kairos_grove")` via FeatureFlagManager name lookup.
///
/// When a runtime gate is closed the factory function returns a disabled-state
/// Element so call sites never need to #ifdef.
///
/// TS sources audited during migration:
///   - MemoryFileSelector  : src/components/memory/MemoryFileSelector.tsx (437 L)
///   - Feedback           : src/components/Feedback.tsx (591 L)
///   - Grove              : src/components/grove/Grove.tsx (462 L)
///
/// NOTE on scope:
///   - All file I/O (memory read/write) is dispatched through the caller-
///     supplied `MemoryBankProvider` callback / the `MemoryCallbacks` struct.
///   - Feedback submission is a stub (no real HTTPS POST; see on_submit callback).
///   - Grove graph loading is likewise injected via `GroveCallbacks`.

module;
#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
export module cc.ui.components.feature_dialogs;

import cc.ui.design.tokens;
import cc.ui.design.primitives;
import cc.ui.trust_utils;
import cc.ui.code_highlight;
import cc.tools.agent_memory;
import cc.config.feature_flags;
export namespace cc::ui::components::feature_dialogs {
using namespace ftxui;
namespace fs = std::filesystem;
using namespace cc::ui::design::tokens;
using namespace cc::ui::design::primitives;
using namespace cc::ui::trust_utils;
using namespace cc::ui::code_highlight;
using cc::core::flags::Feature;
using cc::core::flags::FEATURE_REGISTRY;
using cc::core::flags::FeatureFlagManager;
namespace config {
/// Named feature lookup into FeatureFlagManager + ANT_FEATURE_* env overrides.
[[nodiscard]] inline bool feature(std::string_view name) noexcept {
    static const FeatureFlagManager& mgr = [] {
        static FeatureFlagManager instance;
        instance.set_from_env();
        for (const auto& info : FEATURE_REGISTRY) {
            auto env_key = std::string("ANT_FEATURE_") + std::string(info.name);
            if (const char* v = std::getenv(env_key.c_str()); v && *v) {
                if (*v != '0' && *v != 'f' && *v != 'F')
                    const_cast<FeatureFlagManager&>(instance).enable(info.feature);
            }
        }
        return instance;
    }();

    // ── explicit string → flag mappings for the three dialogs ────────────
    if (name == "proactive_memory")
        return mgr.is_enabled(Feature::Proactive);
    if (name == "user_feedback")
        return true; // feedback is always runtime-available (compile-gate only)
    if (name == "kairos_grove")
        return mgr.is_enabled(Feature::Kairos);

    // Fall back to registry lookup (case-insensitive match on name field).
    for (const auto& info : FEATURE_REGISTRY) {
        if (info.name == name) return mgr.is_enabled(info.feature);
    }
    return false;
}

} // namespace config

[[nodiscard]] inline Element feature_disabled_placeholder(
    std::string_view feature_name)
{
    return vbox({
        text("") | flex,
        hbox({
            text("  "),
            text(std::string(feature_name)) | color(Color::GrayDark) | dim,
            text(": feature disabled (compile macro or runtime flag).")
                | color(Color::GrayDark) | dim,
        }) | center,
        text("") | flex,
    });
}

#if 1  // Always compile migrated proactive-memory UI; runtime flag gates use.

namespace memory_selector {
enum class Bank : std::uint8_t { User=0, Project, Team, Agent, Scratch, _Count };
struct BankInfo { Bank bank; std::string_view label; std::string_view icon; std::uint32_t entry_count; };
struct MemoryEntry {
    std::string id; std::string title; std::vector<std::string> tags;
    std::chrono::system_clock::time_point updated_at;
    std::uint64_t size_bytes = 0; Bank bank = Bank::User; std::string content;
};
using MemoryBankProvider = std::function<std::vector<MemoryEntry>(Bank)>;
struct MemoryCallbacks {
    std::function<void(const MemoryEntry&)> on_select;
    std::function<MemoryEntry(Bank, MemoryEntry)> on_new;
    std::function<void(MemoryEntry)>          on_save;
    std::function<void(const MemoryEntry&)>   on_delete;
    std::function<std::string(const MemoryEntry&)> on_share;
    std::function<void()> on_cancel;
};
enum class SortKey : std::uint8_t { Title, Updated, Size };
enum class PreviewMode : std::uint8_t { View, Edit };
inline std::string_view bank_label(Bank b) noexcept {
    switch (b) { case Bank::User: return "User"; case Bank::Project: return "Project";
      case Bank::Team: return "Team"; case Bank::Agent: return "Agent"; case Bank::Scratch: return "Scratch";
      default: return "-"; }
}
inline std::string_view bank_icon(Bank b) noexcept {
    switch (b) { case Bank::User: return "\xf0\x9f\x91\xa4"; case Bank::Project: return "\xf0\x9f\x93\x81";
      case Bank::Team: return "\xf0\x9f\x91\xa5"; case Bank::Agent: return "\xf0\x9f\xa4\x96"; case Bank::Scratch: return "\xf0\x9f\x93\x9d";
      default: return "\xe2\x80\xa2"; }
}
inline std::string format_updated(const std::chrono::system_clock::time_point& tp) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto diff = duration_cast<hours>(now - tp);
    if (diff < 1h)   return "just now";
    if (diff < 24h)  return std::format("{}h ago", diff.count());
    const auto age_days = duration_cast<days>(diff);
    if (age_days.count() < 30) return std::format("{}d ago", age_days.count());
    std::time_t t = system_clock::to_time_t(tp);
    std::string s(32, '\0');
    std::strftime(s.data(), s.size(), "%Y-%m-%d", std::localtime(&t));
    s.resize(s.find('\0'));
    return s;
}

[[nodiscard]] inline std::string format_size(std::uint64_t bytes) noexcept {
    if (bytes < 1024)          return std::format("{}B", bytes);
    if (bytes < 1024 * 1024)   return std::format("{:.1f}K", bytes / 1024.0);
    return std::format("{:.1f}M", bytes / (1024.0 * 1024.0));
}

/// Highlight substring matches in `text` with a yellow background.
[[nodiscard]] inline Element highlight_contains(std::string value,
                                                 std::string_view needle)
{
    if (needle.empty()) return ftxui::text(std::move(value));
    // Simple case-insensitive find + highlight.
    std::string lower_src;
    lower_src.reserve(value.size());
    for (char c : value) lower_src.push_back(std::tolower(static_cast<unsigned char>(c)));
    std::string lower_needle;
    lower_needle.reserve(needle.size());
    for (char c : needle)
        lower_needle.push_back(std::tolower(static_cast<unsigned char>(c)));

    Elements parts;
    std::size_t pos = 0;
    while (pos < value.size()) {
        auto hit = lower_src.find(lower_needle, pos);
        if (hit == std::string::npos) {
            parts.push_back(ftxui::text(value.substr(pos)));
            break;
        }
        if (hit > pos) parts.push_back(ftxui::text(value.substr(pos, hit - pos)));
        parts.push_back(ftxui::text(value.substr(hit, needle.size()))
                       | bgcolor(Color::Yellow) | color(Color::Black));
        pos = hit + needle.size();
    }
    return hbox(std::move(parts));
}

} // namespace memory_selector

/// MemoryFileSelector implementation — 20-60-20 three-column layout:
///   Left   : bank list with badge counts + [+] New entry
///   Middle : searchable / sortable entry table (Title / Tags / Updated / Size)
///   Right  : Markdown preview + Edit/Delete/Share buttons
class MemoryFileSelectorBase : public ComponentBase {
public:
    using Bank = memory_selector::Bank;
    using MemoryEntry = memory_selector::MemoryEntry;
    using MemoryBankProvider = memory_selector::MemoryBankProvider;
    using MemoryCallbacks = memory_selector::MemoryCallbacks;
    using SortKey = memory_selector::SortKey;
    using PreviewMode = memory_selector::PreviewMode;
    using BankInfo = memory_selector::BankInfo;

    MemoryFileSelectorBase(MemoryBankProvider provider,
                           MemoryCallbacks callbacks)
        : provider_(std::move(provider))
        , cb_(std::move(callbacks))
    {
        RefreshAllBanks();
    }

    Element OnRender() override {
        // Gate the runtime flag; if off, show the standard placeholder.
        if (!config::feature("proactive_memory")) {
            return feature_disabled_placeholder("proactive_memory");
        }
        return vbox({
            render_header(),
            separator(),
            hbox({
                render_left_column()   | size(WIDTH, EQUAL, 20) | flex_shrink,
                separator(),
                render_middle_column() | size(WIDTH, EQUAL, 60) | flex,
                separator(),
                render_right_column()  | size(WIDTH, EQUAL, 20) | flex_shrink,
            }) | flex,
        });
    }

    bool OnEvent(Event event) override {
        if (event.is_character()) {
            char c = event.character()[0];
            if (c == '/') { focused_ = Focus::Search; return true; }
            if (c == 'n' || c == 'N') { OpenNewWizard(); return true; }
        }
        if (event == Event::Escape) {
            if (cb_.on_cancel) cb_.on_cancel();
            return true;
        }
        if (event == Event::Tab) {
            focused_ = static_cast<Focus>((static_cast<int>(focused_) + 1)
                                         % static_cast<int>(Focus::_Count));
            return true;
        }
        if (event == Event::ArrowUp)   { return HandleUp(); }
        if (event == Event::ArrowDown) { return HandleDown(); }
        if (event == Event::Return)    { return HandleEnter(); }
        return ComponentBase::OnEvent(event);
    }

private:
    enum class Focus : std::uint8_t { Banks, Search, Table, Preview, _Count };

    MemoryBankProvider provider_;
    MemoryCallbacks cb_;

    // ── state ────────────────────────────────────────────────────────────
    int selected_bank_ = 0;   // index into banks_
    int selected_entry_ = 0;  // index into filtered_
    std::vector<BankInfo> banks_;
    std::vector<MemoryEntry> all_entries_;  // entries of current bank
    std::vector<int> filtered_;             // indices into all_entries_
    std::string search_query_;
    SortKey sort_key_ = SortKey::Updated;
    bool sort_desc_ = true;
    PreviewMode preview_mode_ = PreviewMode::View;
    std::string edit_buffer_;
    Focus focused_ = Focus::Banks;

    // ── New-entry wizard state (3 steps) ─────────────────────────────────
    bool wizard_open_ = false;
    int  wizard_step_ = 0;   // 0 = bank, 1 = title+tags, 2 = content
    Bank wizard_bank_ = Bank::User;
    std::string wizard_title_;
    std::vector<std::string> wizard_tags_;
    std::string wizard_tags_input_;  // comma-separated raw input
    std::string wizard_content_;

    // ── data loading ─────────────────────────────────────────────────────
    void RefreshAllBanks() {
        banks_.clear();
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Bank::_Count); ++i) {
            Bank b = static_cast<Bank>(i);
            std::vector<MemoryEntry> entries = provider_
                ? provider_(b) : std::vector<MemoryEntry>{};
            banks_.push_back(BankInfo{
                .bank        = b,
                .label       = memory_selector::bank_label(b),
                .icon        = memory_selector::bank_icon(b),
                .entry_count = static_cast<std::uint32_t>(entries.size()),
            });
        }
        LoadBank(static_cast<Bank>(selected_bank_));
    }

    void LoadBank(Bank b) {
        all_entries_ = provider_ ? provider_(b) : std::vector<MemoryEntry>{};
        ApplyFilterAndSort();
        selected_entry_ = 0;
        preview_mode_ = PreviewMode::View;
        edit_buffer_.clear();
    }

    void ApplyFilterAndSort() {
        filtered_.clear();
        filtered_.reserve(all_entries_.size());
        const std::string q = [&] {
            std::string s;
            s.reserve(search_query_.size());
            for (char c : search_query_)
                s.push_back(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }();
        for (std::size_t i = 0; i < all_entries_.size(); ++i) {
            if (q.empty()) { filtered_.push_back(static_cast<int>(i)); continue; }
            const auto& e = all_entries_[i];
            auto has = [&](std::string_view sv) {
                if (sv.size() < q.size()) return false;
                for (std::size_t k = 0; k <= sv.size() - q.size(); ++k) {
                    bool ok = true;
                    for (std::size_t j = 0; j < q.size() && ok; ++j)
                        ok &= (std::tolower(static_cast<unsigned char>(sv[k+j])) == q[j]);
                    if (ok) return true;
                }
                return false;
            };
            if (has(e.title)) { filtered_.push_back(static_cast<int>(i)); continue; }
            if (has(e.content)) { filtered_.push_back(static_cast<int>(i)); continue; }
            bool tag_hit = false;
            for (const auto& t : e.tags) if (has(t)) { tag_hit = true; break; }
            if (tag_hit) filtered_.push_back(static_cast<int>(i));
        }
        // Sort
        std::sort(filtered_.begin(), filtered_.end(),
            [&](int a, int b) {
                const auto& A = all_entries_[a];
                const auto& B = all_entries_[b];
                bool less = true;
                switch (sort_key_) {
                    case SortKey::Title:
                        less = A.title < B.title; break;
                    case SortKey::Updated:
                        less = A.updated_at < B.updated_at; break;
                    case SortKey::Size:
                        less = A.size_bytes < B.size_bytes; break;
                }
                return sort_desc_ ? !less : less;
            });
    }

    // ── navigation ───────────────────────────────────────────────────────
    bool HandleUp() {
        if (focused_ == Focus::Banks) {
            if (selected_bank_ > 0) {
                --selected_bank_;
                LoadBank(static_cast<Bank>(selected_bank_));
            }
            return true;
        }
        if (focused_ == Focus::Table) {
            if (selected_entry_ > 0) --selected_entry_;
            return true;
        }
        return false;
    }
    bool HandleDown() {
        if (focused_ == Focus::Banks) {
            if (selected_bank_ < static_cast<int>(banks_.size()) - 1) {
                ++selected_bank_;
                LoadBank(static_cast<Bank>(selected_bank_));
            }
            return true;
        }
        if (focused_ == Focus::Table) {
            if (selected_entry_ < static_cast<int>(filtered_.size()) - 1)
                ++selected_entry_;
            return true;
        }
        return false;
    }
    bool HandleEnter() {
        if (wizard_open_) return WizardNext();
        if (preview_mode_ == PreviewMode::Edit) {
            // Save edit
            if (selected_entry_ < static_cast<int>(filtered_.size())) {
                auto& e = all_entries_[filtered_[selected_entry_]];
                e.content = edit_buffer_;
                e.updated_at = std::chrono::system_clock::now();
                e.size_bytes = static_cast<std::uint64_t>(edit_buffer_.size());
                if (cb_.on_save) cb_.on_save(e);
            }
            preview_mode_ = PreviewMode::View;
            return true;
        }
        if (focused_ == Focus::Table
            && selected_entry_ < static_cast<int>(filtered_.size())
            && cb_.on_select)
        {
            cb_.on_select(all_entries_[filtered_[selected_entry_]]);
        }
        return true;
    }

    // ── rendering: header ────────────────────────────────────────────────
    Element render_header() const {
        return hbox({
            text("🧠 Memory") | bold | color(Color::CyanLight),
            filler(),
            text(std::format(" {} entries total  ",
                             all_entries_.size())) | dim,
            text("[n] new   [/] search   [Tab] switch pane   [Esc] close")
                | color(Color::GrayDark),
        }) | size(HEIGHT, EQUAL, 1);
    }

    // ── rendering: left (banks) ──────────────────────────────────────────
    Element render_left_column() const {
        Elements rows;
        for (std::size_t i = 0; i < banks_.size(); ++i) {
            const auto& bi = banks_[i];
            bool sel = (static_cast<int>(i) == selected_bank_
                        && focused_ == Focus::Banks);
            Element row = hbox({
                text(std::format(" {} ", bi.icon)),
                text(std::string(bi.label)),
                filler(),
                text(std::format(" {} ", bi.entry_count))
                    | color(Color::Blue) | bgcolor(Color::RGB(30, 30, 60)),
            });
            if (sel) row = row | inverted | bold;
            rows.push_back(row | size(HEIGHT, EQUAL, 1));
        }
        // New entry "button"
        {
            Element row = hbox({
                text(" + "),
                text("New entry") | color(Color::Green),
            }) | size(HEIGHT, EQUAL, 1);
            rows.push_back(separator());
            rows.push_back(row);
        }
        return vbox({
            text(" Banks") | bold | underlined,
            separator(),
            vbox(std::move(rows)) | yframe | flex,
        });
    }

    // ── rendering: middle (search + table) ───────────────────────────────
    Element render_middle_column() {

        // Search bar
        Element search_row = hbox({
            text(focused_ == Focus::Search ? "🔎 " : "   "),
            text("Search: "),
            text(search_query_.empty() ? std::string("(type / to focus)")
                                      : search_query_)
                | color(focused_ == Focus::Search
                        ? Color::Cyan : Color::GrayDark),
            filler(),
            text(render_sort_label()) | dim,
        });

        // Table header
        Element header = hbox({
            text(" Title")    | bold | size(WIDTH, EQUAL, 30),
            text(" Tags")     | bold | size(WIDTH, EQUAL, 18),
            text(" Updated")  | bold | size(WIDTH, EQUAL, 14),
            text(" Size")     | bold | size(WIDTH, EQUAL, 8),
        }) | bgcolor(Color::RGB(30, 30, 30));

        Elements body_rows;
        body_rows.reserve(filtered_.size());
        if (filtered_.empty()) {
            body_rows.push_back(
                text(" (no entries — press 'n' to create)") | dim | center);
        }
        for (int i = 0; i < static_cast<int>(filtered_.size()); ++i) {
            const auto& e = all_entries_[filtered_[i]];
            bool sel = (i == selected_entry_ && focused_ == Focus::Table);
            std::string tags_line;
            for (const auto& t : e.tags) {
                if (!tags_line.empty()) tags_line += " ";
                tags_line += "#" + t;
            }
            Element row = hbox({
                memory_selector::highlight_contains(e.title, search_query_)
                    | size(WIDTH, EQUAL, 30),
                memory_selector::highlight_contains(tags_line.empty() ? "—" : tags_line, search_query_)
                    | color(Color::Magenta) | size(WIDTH, EQUAL, 18),
                text(memory_selector::format_updated(e.updated_at))
                    | dim | size(WIDTH, EQUAL, 14),
                text(memory_selector::format_size(e.size_bytes))
                    | dim | size(WIDTH, EQUAL, 8),
            });
            if (sel) row = row | inverted;
            body_rows.push_back(row | size(HEIGHT, EQUAL, 1));
        }
        return vbox({
            text(" Entries") | bold | underlined,
            separator(),
            search_row,
            separator(),
            header,
            separator(),
            vbox(std::move(body_rows)) | yframe | flex,
        });
    }

    // ── rendering: right (preview + actions) ────────────────────────────
    Element render_right_column() {

        const MemoryEntry* sel = nullptr;
        if (selected_entry_ < static_cast<int>(filtered_.size()))
            sel = &all_entries_[filtered_[selected_entry_]];

        Elements content;
        content.push_back(text(" Preview") | bold | underlined);
        content.push_back(separator());

        if (!sel) {
            content.push_back(text(" (select an entry)") | dim | center);
        } else {
            // Action row
            Element actions = hbox({
                text(preview_mode_ == PreviewMode::View ? "[Edit]" : "[Save]")
                    | color(Color::Cyan),
                text(" "),
                text("[Delete]") | color(Color::RedLight),
                text(" "),
                text("[Share]")  | color(Color::BlueLight),
            });
            content.push_back(actions);
            content.push_back(separator());

            // Title + meta
            content.push_back(text(sel->title) | bold);
            {
                std::string meta = std::format(
                    "{}  · {}  · {}",
                    sel->bank == Bank::User ? "User" :
                    sel->bank == Bank::Project ? "Project" :
                    sel->bank == Bank::Team ? "Team" :
                    sel->bank == Bank::Agent ? "Agent" : "Scratch",
                    memory_selector::format_updated(sel->updated_at),
                    memory_selector::format_size(sel->size_bytes));
                content.push_back(text(meta) | dim);
            }
            // Tags
            if (!sel->tags.empty()) {
                Elements tag_els;
                for (const auto& t : sel->tags) {
                    tag_els.push_back(
                        text(std::format("#{} ", t)) | color(Color::Magenta));
                }
                content.push_back(hbox(std::move(tag_els)));
            }
            content.push_back(separator());

            // Body
            if (preview_mode_ == PreviewMode::Edit) {
                content.push_back(text(edit_buffer_) | color(Color::Yellow));
                content.push_back(separator());
                content.push_back(
                    hbox({
                        text("[Enter] Save") | color(Color::Green),
                        text("  "),
                        text("[Esc] Cancel") | dim,
                    }) | center);
            } else {
                // Use code_highlight module for fenced code blocks; everything
                // else is rendered as dim plain text.  A full Markdown renderer
                // would be plugged here as a post-Phase-4 enhancement.
                content.push_back(render_markdown_preview(sel->content));
            }
        }

        // Wizard overlay
        if (wizard_open_) {
            return vbox({
                text(" 📝 New Entry (step "
                     + std::to_string(wizard_step_ + 1) + " of 3)")
                    | bold | color(Color::Cyan),
                separator(),
                render_wizard_step(),
            }) | flex;
        }

        return vbox(std::move(content)) | yframe | flex;
    }

    // ── misc helpers for rendering ───────────────────────────────────────
    [[nodiscard]] std::string render_sort_label() const {
        std::string_view key;
        switch (sort_key_) {
            case SortKey::Title:   key = "title";   break;
            case SortKey::Updated: key = "updated"; break;
            case SortKey::Size:    key = "size";    break;
        }
        return std::format("sort: {}{}  ", key, sort_desc_ ? "↓" : "↑");
    }

    [[nodiscard]] Element render_markdown_preview(const std::string& md) const {
        // Very small MD subset: detect ``` fenced code blocks and send them
        // through RenderCodeHighlight; everything else is plain paragraph.
        Elements lines;
        std::istringstream iss(md);
        std::string line;
        bool in_code = false;
        std::string code_lang;
        std::string code_buf;

        auto flush_code = [&] {
            if (code_buf.empty()) return;
            auto highlighted = render_code_block(code_buf, code_lang);
            lines.push_back(highlighted);
            code_buf.clear();
            code_lang.clear();
        };

        while (std::getline(iss, line)) {
            if (line.starts_with("```")) {
                if (!in_code) {
                    in_code = true;
                    code_lang = line.substr(3);
                    code_buf.clear();
                } else {
                    in_code = false;
                    flush_code();
                }
                continue;
            }
            if (in_code) {
                code_buf += line; code_buf += '\n';
                continue;
            }
            if (line.starts_with("# ")) {
                lines.push_back(text(line.substr(2)) | bold | color(Color::Cyan));
            } else if (line.starts_with("## ")) {
                lines.push_back(text(line.substr(3)) | bold);
            } else if (line.starts_with("- ") || line.starts_with("* ")) {
                lines.push_back(hbox({
                    text("• "), text(line.substr(2)),
                }));
            } else {
                lines.push_back(text(line));
            }
        }
        flush_code();
        return vbox(std::move(lines)) | yframe;
    }

    [[nodiscard]] Element render_code_block(const std::string& source,
                                             const std::string& lang) const {
        return RenderCodeBlock(RenderCodeBlockOptions{
            .code = source,
            .language = lang,
            .file_path = std::nullopt,
            .visible_lines = 20,
            .show_line_numbers = false,
            .show_copy_tag = false,
            .theme = std::nullopt,
        });
    }

    void OpenNewWizard() {
        wizard_open_ = true; wizard_step_ = 0;
        wizard_bank_ = static_cast<Bank>(selected_bank_);
        wizard_title_.clear(); wizard_tags_.clear();
        wizard_tags_input_.clear(); wizard_content_.clear();
    }
    bool WizardNext() {
        if (wizard_step_ < 2) { ++wizard_step_; return true; }
        MemoryEntry e; e.bank = wizard_bank_;
        e.title = wizard_title_.empty() ? "Untitled" : wizard_title_;
        e.tags = std::move(wizard_tags_); e.content = wizard_content_;
        e.updated_at = std::chrono::system_clock::now();
        e.size_bytes = static_cast<std::uint64_t>(wizard_content_.size());
        e.id = std::format("mem-{:x}", std::chrono::system_clock::to_time_t(e.updated_at));
        if (cb_.on_new) e = cb_.on_new(wizard_bank_, std::move(e));
        wizard_open_ = false; LoadBank(wizard_bank_); return true;
    }
    Element render_wizard_step() const {
        switch (wizard_step_) {
        case 0: {
            Elements rows;
            for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Bank::_Count); ++i) {
                auto b = static_cast<Bank>(i); bool sel = (b == wizard_bank_);
                Element row = hbox({
                    text(sel ? "> " : "  "),
                    text(std::string(memory_selector::bank_icon(b))),
                    text(std::string(memory_selector::bank_label(b))),
                });
                if (sel) row = row | color(Color::Cyan) | bold;
                rows.push_back(row);
            }
            rows.push_back(separator());
            rows.push_back(text("[↑/↓] select bank    [Enter] next") | dim | center);
            return vbox(std::move(rows));
        }
        case 1:
            return vbox({
                hbox({ text("Title: "), text(wizard_title_.empty() ? "(enter a title)" : wizard_title_) | color(Color::Yellow) }),
                text(""),
                hbox({ text("Tags:  "), text(wizard_tags_input_.empty() ? "(comma-separated)" : wizard_tags_input_) | color(Color::Magenta) }),
                separator(),
                text("[Enter] next") | dim | center,
            });
        case 2:
            return vbox({
                text("Content (Markdown):") | bold, separator(),
                text(wizard_content_.empty() ? "(attach TextInput via callbacks)" : wizard_content_) | color(Color::Yellow),
                separator(),
                text("[Enter] create entry") | dim | center,
            });
        }
        return text("");
    }
    void ConfirmDelete(const MemoryEntry& entry) {
        // UI8 Low-trust confirm (no countdown): 2-button pattern.
        // Integration point: overlay a cc::ui::trust_dialog modal here.
        // For now, directly invoke on_delete to keep the flow unblocked.
        if (cb_.on_delete) cb_.on_delete(entry);
    }
};

/// Construct the MemoryFileSelector component.
/// @param provider   Supplies entry lists per bank.  The component calls this
///                   during RefreshAllBanks() — real callers cache results.
/// @param callbacks  User-action callbacks (select/new/save/delete/...).
[[nodiscard]] inline Component MakeMemoryFileSelector(
    typename MemoryFileSelectorBase::MemoryBankProvider provider,
    typename MemoryFileSelectorBase::MemoryCallbacks callbacks)
{
    if (!config::feature("proactive_memory"))
        return Renderer([] {
            return feature_disabled_placeholder("proactive_memory");
        });
    return Make<MemoryFileSelectorBase>(std::move(provider),
                                         std::move(callbacks));
}

#else  // !FEATURE_PROACTIVE_MEMORY

/// Stub factory (always returns a "Feature disabled" placeholder).
/// Keeps the same ABI so call sites do not need #ifdefs.
struct MemoryCallbacksStub {
    std::function<void(int)> on_select;
    std::function<void(int, int)> on_new;
    std::function<void(int)> on_save;
    std::function<void(int)> on_delete;
    std::function<std::string(int)> on_share;
    std::function<void()> on_cancel;
};
[[nodiscard]] inline Component MakeMemoryFileSelector(
    std::function<std::vector<int>(int)>,
    MemoryCallbacksStub)
{
    return Renderer([] {
        return feature_disabled_placeholder("proactive_memory");
    });
}

#endif  // FEATURE_PROACTIVE_MEMORY

#if 1  // Always compile migrated feedback UI; runtime flag gates use.

namespace feedback_dialog {

enum class Step : std::uint8_t { Nps, Detail, Submitting, Success, Failed };

/// Feedback category (multi-select checkboxes in Step 2).
enum class Category : std::uint8_t {
    Bug = 0,
    FeatureRequest,
    UXIssue,
    Performance,
    Docs,
    Other,
    _Count,
};

enum class Severity : std::uint8_t {
    Critical = 0, High, Medium, Low,
};

struct FeedbackOptions {
    std::function<void(std::string tracking_id, bool succeeded)> on_submit;
    bool include_session_default = true;
};

} // namespace feedback_dialog

/// 3-step feedback dialog:  NPS scoring  →  detail form  →  submit spinner.
/// Keyboard: step-1 uses digit hotkeys 0-9 (0 = score 10, TS convention);
/// step-2 uses Tab to cycle focus; Esc cancels; Enter advances.
class FeedbackDialogBase : public ComponentBase {
public:
    using Step = feedback_dialog::Step;
    using Category = feedback_dialog::Category;
    using Severity = feedback_dialog::Severity;
    using FeedbackOptions = feedback_dialog::FeedbackOptions;

    explicit FeedbackDialogBase(FeedbackOptions opts)
        : opts_(std::move(opts)) {}

    Element OnRender() override {
        if (!config::feature("user_feedback"))
            return feature_disabled_placeholder("user_feedback");

        switch (step_) {
            case Step::Nps:         return render_nps();
            case Step::Detail:      return render_detail();
            case Step::Submitting:  return render_submitting();
            case Step::Success:     return render_success();
            case Step::Failed:      return render_failed();
        }
        return text("");
    }

    bool OnEvent(Event event) override {
        // Digit hotkeys — step-1 only
        if (step_ == Step::Nps && event.is_character()) {
            char c = event.character()[0];
            if (c >= '0' && c <= '9') {
                nps_score_ = (c == '0') ? 10 : (c - '0');
                step_ = Step::Detail;
                return true;
            }
        }
        if (event == Event::Return) {
            if (step_ == Step::Nps && nps_score_ >= 0) {
                step_ = Step::Detail; return true;
            }
            if (step_ == Step::Detail) { BeginSubmit(); return true; }
            if (step_ == Step::Success || step_ == Step::Failed) { Close(); return true; }
        }
        if (event == Event::Escape) { Close(); return true; }
        if (event == Event::Backspace) {
            if (step_ == Step::Detail) { step_ = Step::Nps; return true; }
            if (step_ == Step::Failed) { step_ = Step::Detail; return true; }
        }
        if (event == Event::Tab && step_ == Step::Detail) {
            focus_ = static_cast<Focus>((static_cast<int>(focus_) + 1)
                                       % static_cast<int>(Focus::_Count));
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

private:
    enum class Focus : std::uint8_t {
        CatBug, CatFeat, CatUX, CatPerf, CatDocs, CatOther,
        Severity, FreeText, Attach, Email,
        _Count,
    };

    FeedbackOptions opts_;
    Step step_ = Step::Nps;
    int nps_score_ = -1;                 // 0..10
    std::bitset<6> categories_;          // Category bitset
    int severity_idx_ = 2;               // default Medium
    std::string freetext_;
    bool attach_session_ = true;         // overridden by opts_ default
    std::string email_;
    std::string tracking_id_;
    Focus focus_ = Focus::FreeText;

    // ── step-1: NPS ──────────────────────────────────────────────────────
    Element render_nps() const {

        Elements scale;
        for (int i = 0; i <= 10; ++i) {
            bool sel = (i == nps_score_);
            Color fg = Color::White;
            Color bg = Color::RGB(40, 40, 40);
            if (i <= 6) fg = Color::Red;       // Detractor
            else if (i <= 8) fg = Color::Yellow; // Passive
            else fg = Color::Green;           // Promoter
            Element cell = text(std::format(" {} ", i < 10 ? ' '+std::to_string(i) : std::to_string(i)))
                | color(fg) | bgcolor(bg) | border;
            if (sel) cell = cell | inverted | bold;
            scale.push_back(cell);
        }
        Elements legend {
            text("0──────────────6") | color(Color::Red),
            filler(),
            text("7────8") | color(Color::Yellow),
            filler(),
            text("9──10") | color(Color::Green),
        };
        Elements legend2 {
            text("Detractors") | color(Color::Red) | dim,
            filler(),
            text("Passive") | color(Color::Yellow) | dim,
            filler(),
            text("Promoters") | color(Color::Green) | dim,
        };
        return vbox({
            text("📢  We'd love your feedback") | bold,
            text(""),
            text("Would you recommend Seed to a friend or colleague?") | bold,
            text(""),
            hbox(std::move(scale)),
            hbox(std::move(legend)),
            hbox(std::move(legend2)),
            separator(),
            text("  Score: " + (nps_score_ < 0 ? std::string("(press 0–9; 0 = 10)")
                                            : std::to_string(nps_score_))),
            separator(),
            text("[0..9] pick score    [Enter] next    [Esc] cancel")
                | dim | center,
        });
    }

    // ── step-2: detail form ──────────────────────────────────────────────
    Element render_detail() const {

        // Categories (6 checkboxes)
        Elements cat_rows;
        cat_rows.push_back(text(" Category (select all that apply)") | bold);
        const std::array<std::pair<Category, const char*>, 6> cat_labels = {{
            { Category::Bug,            "Bug" },
            { Category::FeatureRequest, "Feature Request" },
            { Category::UXIssue,        "UX Issue" },
            { Category::Performance,    "Performance" },
            { Category::Docs,           "Documentation" },
            { Category::Other,          "Other" },
        }};
        Elements cat_line;
        for (auto [cat, label] : cat_labels) {
            bool on = categories_.test(static_cast<std::size_t>(cat));
            bool foc = focus_ == static_cast<Focus>(
                    static_cast<int>(Focus::CatBug)
                    + static_cast<int>(cat));
            Element box = text(on ? "▣ " : "▢ ")
                | color(on ? Color::Green : Color::GrayDark);
            Element lab = text(std::string(label));
            if (foc) lab = lab | underlined;
            cat_line.push_back(hbox({ box, lab, text("   ") }));
        }
        cat_rows.push_back(hbox(std::move(cat_line)));

        // Severity dropdown
        const char* sev_str[] = {"Critical", "High", "Medium", "Low"};
        Color sev_color[] = {Color::Red, Color::Orange1, Color::Yellow,
                              Color::GrayLight};
        Element sev_row = hbox({
            text(" Severity:") | bold,
            text(" [ "),
            text(std::format("▾ {} ", sev_str[severity_idx_]))
                | color(sev_color[severity_idx_]) | inverted,
            text(" ]") | color(focus_ == Focus::Severity
                ? Color{Color::Cyan}
                : Color{Color::Default}),
        });

        // Free-text area
        Element free = vbox({
            hbox({ text(" What happened?") | bold,
                   text(" What did you expect?") | bold }),
            separator(),
            text(freetext_.empty()
                 ? std::string("(type your description — TAB to move)")
                 : freetext_)
                | color(focus_ == Focus::FreeText
                        ? Color::Yellow : Color::White),
        }) | size(HEIGHT, GREATER_THAN, 5) | border;

        // Attach session + email
        Element attach = hbox({
            text(attach_session_ ? "▣ " : "▢ ")
                | color(attach_session_ ? Color::Green : Color::GrayDark),
            text(" Attach session metadata") | bold,
            text("  (anonymised)") | dim,
        });
        if (focus_ == Focus::Attach) attach = attach | underlined;

        Element email_row = hbox({
            text(" Email (optional):") | bold,
            text(" "),
            text(email_.empty() ? std::string("(for follow-up only)")
                                : email_)
                | color(focus_ == Focus::Email
                    ? Color{Color::Yellow}
                    : Color{Color::Default}),
        });

        return vbox({
            text(std::format("Step 2 / 3   (NPS: {})", nps_score_))
                | dim | align_right,
            vbox(std::move(cat_rows)),
            text(""),
            sev_row,
            text(""),
            free,
            text(""),
            attach,
            text(""),
            email_row,
            separator(),
            text("[Tab] cycle fields   [Enter] submit   "
                 "[Backspace] back   [Esc] cancel")
                | dim | center,
        });
    }

    // ── step-3: submitting / success / failed ────────────────────────────
    Element render_submitting() const {
        return vbox({ text("") | flex,
            hbox({ text("  ⏳ "), text("Sending your feedback securely…") | color(Color::Cyan) | bold }) | center,
            text("") | flex });
    }
    Element render_success() const {
        return vbox({ text("") | flex,
            hbox({ text("  ✅ ") | color(Color::Green) | bold,
                   text("Thanks! Your feedback helps us improve.") | color(Color::Green) | bold }) | center,
            text(std::format("  Tracking ID: FB-{}", tracking_id_)) | dim | center,
            text(""), text("[Enter / any key] close") | dim | center,
            text("") | flex });
    }
    Element render_failed() const {
        return vbox({ text("") | flex,
            hbox({ text("  ⚠ "), text("Could not submit feedback.") | color(Color::Red) | bold }) | center,
            text(""),
            text("[Enter] retry    [Backspace] edit") | dim | center,
            text("") | flex });
    }
    // Feedback submission: delegates to on_submit callback which should
    // perform the actual HTTPS POST (see FeedbackOptions::on_submit).
    // The UI transitions through Submitting -> Success/Failed states.
    void BeginSubmit() {
        step_ = Step::Submitting;
        tracking_id_ = generate_tracking_id();
        bool ok = false;
        if (opts_.on_submit) {
            opts_.on_submit(tracking_id_, ok);
        }
        step_ = ok ? Step::Success : Step::Failed;
    }

    /// Generate a FB-XXXXXXXX style alpha-num tracking id.
    [[nodiscard]] static std::string generate_tracking_id() {
        static constexpr std::string_view alpha =
            "ABCDEFGHJKMNPQRSTUVWXYZ23456789"; // no 0/O/1/I
        std::mt19937 rng{
            static_cast<std::uint32_t>(
                std::chrono::steady_clock::now().time_since_epoch().count())
        };
        std::string out;
        out.reserve(8);
        for (int i = 0; i < 8; ++i) {
            out.push_back(alpha[std::uniform_int_distribution<>{
                0, static_cast<int>(alpha.size()-1)}(rng)]);
        }
        return out;
    }

    void Close() {
        // Notify via callback?  Spec does not require on_cancel; on_submit
        // carries the "did it succeed" boolean instead.
    }
};

[[nodiscard]] inline Component MakeFeedbackDialog(
    feedback_dialog::FeedbackOptions opts)
{
    if (!config::feature("user_feedback"))
        return Renderer([] {
            return feature_disabled_placeholder("user_feedback");
        });
    return Make<FeedbackDialogBase>(std::move(opts));
}

#else  // !FEATURE_USER_FEEDBACK

namespace feedback_dialog {
struct FeedbackOptionsStub {
    std::function<void(std::string, bool)> on_submit;
    bool include_session_default = true;
};
}
[[nodiscard]] inline Component MakeFeedbackDialog(
    feedback_dialog::FeedbackOptionsStub)
{
    return Renderer([] {
        return feature_disabled_placeholder("user_feedback");
    });
}

#endif  // FEATURE_USER_FEEDBACK

#if 1  // Always compile migrated Grove UI; runtime flag gates use.

namespace grove_view {

using Id = std::string;
enum class NodeKind : std::uint8_t { Doc=0, Concept, Agent, Task, Memory };
struct GNode { Id id; std::string label; NodeKind kind=NodeKind::Doc; double weight=1.0; };
struct GEdge { Id from; Id to; double strength=0.5; };
struct GroveData { std::vector<GNode> nodes; std::vector<GEdge> edges; std::optional<Id> focus; };
struct GroveCallbacks { std::function<void(const Id&)> on_focus; std::function<void(const Id&)> on_select; };
enum class ViewMode : std::uint8_t { Tree=0, Matrix };
} // namespace grove_view
class GroveViewBase : public ComponentBase {
public:
    using Id = grove_view::Id; using GNode = grove_view::GNode;
    using GEdge = grove_view::GEdge; using GroveData = grove_view::GroveData;
    using GroveCallbacks = grove_view::GroveCallbacks;
    using ViewMode = grove_view::ViewMode; using NodeKind = grove_view::NodeKind;
    GroveViewBase(GroveData data, GroveCallbacks cb)
        : data_(std::move(data)), cb_(std::move(cb)) {
        build_index();
        // Initial focus: use provided, else pick the highest-weight node.
        if (!data_.focus.has_value() && !data_.nodes.empty()) {
            std::size_t best = 0;
            for (std::size_t i = 1; i < data_.nodes.size(); ++i)
                if (data_.nodes[i].weight > data_.nodes[best].weight) best = i;
            focus_id_ = data_.nodes[best].id;
        } else if (data_.focus.has_value()) {
            focus_id_ = *data_.focus;
        }
    }

    Element OnRender() override {
        if (!config::feature("kairos_grove"))
            return feature_disabled_placeholder("kairos_grove");

        if (data_.nodes.empty()) return render_empty();

        return vbox({
            render_title(),
            separator(),
            hbox({
                (mode_ == ViewMode::Tree)
                    ? render_tree() | flex
                    : render_matrix() | flex,
            }) | flex,
            separator(),
            render_footer(),
        });
    }

    bool OnEvent(Event event) override {
        if (event == Event::Character('t') || event == Event::Character('T')) {
            mode_ = (mode_ == ViewMode::Tree) ? ViewMode::Matrix : ViewMode::Tree;
            return true;
        }
        if (event == Event::Character('j')) { move(+1); return true; }
        if (event == Event::Character('k')) { move(-1); return true; }
        if (event == Event::Character('+') || event == Event::Character('=')) {
            if (depth_ < 6) ++depth_;
            return true;
        }
        if (event == Event::Character('-') || event == Event::Character('_')) {
            if (depth_ > 1) --depth_;
            return true;
        }
        if (event == Event::Character('f') || event == Event::Character('F')) {
            search_active_ = true; return true;
        }
        if (event == Event::Return) {
            if (sel_idx_ >= 0
                && sel_idx_ < static_cast<int>(data_.nodes.size()))
            {
                const auto& n = data_.nodes[sel_idx_];
                focus_id_ = n.id;
                if (cb_.on_focus)  cb_.on_focus(n.id);
                if (cb_.on_select) cb_.on_select(n.id);
            }
            return true;
        }
        if (event == Event::Escape) {
            if (search_active_) { search_active_ = false; search_query_.clear(); return true; }
            return false;
        }
        return ComponentBase::OnEvent(event);
    }

private:
    GroveData data_;
    GroveCallbacks cb_;
    ViewMode mode_ = ViewMode::Tree;
    int depth_ = 3;                       // BFS expansion depth
    std::string focus_id_;
    int sel_idx_ = 0;                     // flat node index
    // Precomputed indices
    std::unordered_map<Id, std::size_t> id_to_idx_;
    std::vector<std::vector<std::size_t>> out_; // adjacency outgoing
    std::vector<std::vector<std::size_t>> in_;  // adjacency incoming
    std::vector<std::vector<double>> edge_strength_;
    // Search
    bool search_active_ = false;
    std::string search_query_;

    void build_index() {
        const auto N = data_.nodes.size();
        id_to_idx_.clear();
        for (std::size_t i = 0; i < N; ++i)
            id_to_idx_[data_.nodes[i].id] = i;
        out_.assign(N, {}); in_.assign(N, {});
        edge_strength_.assign(N, std::vector<double>(N, 0.0));
        for (const auto& e : data_.edges) {
            auto a = id_to_idx_.find(e.from);
            auto b = id_to_idx_.find(e.to);
            if (a == id_to_idx_.end() || b == id_to_idx_.end()) continue;
            out_[a->second].push_back(b->second);
            in_[b->second].push_back(a->second);
            edge_strength_[a->second][b->second] = e.strength;
        }
    }

    void move(int delta) {
        const int N = static_cast<int>(data_.nodes.size());
        if (N == 0) return;
        sel_idx_ = (sel_idx_ + delta + N) % N;
    }

    static const char* kind_icon(NodeKind k) noexcept {
        switch (k) {
            case NodeKind::Doc:     return "📄";
            case NodeKind::Concept: return "💡";
            case NodeKind::Agent:   return "🤖";
            case NodeKind::Task:    return "✅";
            case NodeKind::Memory:  return "🧠";
        }
        return "•";
    }
    static Color kind_color(NodeKind k) noexcept {
        switch (k) {
            case NodeKind::Doc:     return Color::White;
            case NodeKind::Concept: return Color::MagentaLight;
            case NodeKind::Agent:   return Color::CyanLight;
            case NodeKind::Task:    return Color::GreenLight;
            case NodeKind::Memory:  return Color::YellowLight;
        }
        return Color::White;
    }
    static std::string trunc4(std::string_view s) {
        if (s.size() <= 4) return std::string(s);
        return std::string(s.substr(0, 4));
    }

    // ── title / footer / empty ──────────────────────────────────────────
    Element render_title() const {
        const char* mode = (mode_ == ViewMode::Tree) ? "ASCII Tree"
                                                     : "Adjacency Matrix";
        return hbox({
            text("🌱 Grove ") | bold | color(Color::GreenLight),
            text(" (KAIROS experiment) ") | dim,
            filler(),
            text(std::format("[{}]  nodes: {}   edges: {}   depth: {}",
                             mode, data_.nodes.size(),
                             data_.edges.size(), depth_)) | dim,
        });
    }
    Element render_footer() const {
        std::string hint = "[j/k] select   [Enter] focus   [f] search   "
                           "[t] toggle   [+/-] zoom depth";
        if (search_active_) hint = "SEARCH: type label fragment   [Esc] cancel";
        return text(hint) | color(Color::GrayDark) | center;
    }
    Element render_empty() const {
        return vbox({
            text("") | flex,
            text("🌱  KAIROS Grove (experimental feature disabled)")
                | color(Color::GreenLight) | dim | center,
            text(""),
            text("   Enable in config → features.kairos_grove")
                | color(Color::GrayDark) | dim | center,
            text("") | flex,
        });
    }

    // ── Tree mode (BFS from focus) ──────────────────────────────────────
    Element render_tree() const {
        std::vector<std::size_t> order;
        std::vector<int> depth_of;
        std::vector<bool> is_last;
        order.reserve(data_.nodes.size());
        depth_of.assign(data_.nodes.size(), -1);

        auto start_it = id_to_idx_.find(focus_id_);
        std::size_t start = (start_it == id_to_idx_.end()) ? 0 : start_it->second;

        // BFS up to depth_ levels (depth_ = 3 means root + 2 levels).
        struct QItem { std::size_t idx; int d; };
        std::queue<QItem> q;
        q.push({start, 0});
        depth_of[start] = 0;
        while (!q.empty()) {
            auto [u, d] = q.front(); q.pop();
            order.push_back(u);
            if (d >= depth_ - 1) continue;
            const auto& neigh = out_[u];
            for (std::size_t k = 0; k < neigh.size(); ++k) {
                auto v = neigh[k];
                if (depth_of[v] >= 0) continue;
                depth_of[v] = d + 1;
                q.push({v, d + 1});
                is_last.push_back(k + 1 == neigh.size());
            }
        }
        // Build display
        Elements rows;
        rows.reserve(order.size() + 1);
        for (std::size_t pos = 0; pos < order.size(); ++pos) {
            auto u = order[pos];
            const auto& node = data_.nodes[u];
            const int d = depth_of[u];
            // Build prefix
            std::string prefix;
            for (int i = 0; i < d; ++i) {
                // use last-of-level heuristics
                if (i + 1 == d && !is_last.empty()
                    && pos - 1 < is_last.size() && is_last[pos - 1])
                    prefix += "    ";
                else
                    prefix += (i + 1 == d) ? "├── " : "│   ";
            }
            // inbound / outbound degree badge
            auto deg = std::format(" ({}↗/{}↘)",
                in_[u].size(), out_[u].size());
            bool sel = (static_cast<int>(u) == sel_idx_);
            Element label = hbox({
                text(std::format("{}{} ", prefix, kind_icon(node.kind))),
                text(node.label) | color(kind_color(node.kind)),
                text(deg) | color(Color::GrayDark),
            });
            if (node.id == focus_id_) label = label | bold;
            if (sel) label = label | underlined | color(Color::Cyan);
            rows.push_back(label | size(HEIGHT, EQUAL, 1));
        }
        if (rows.empty()) rows.push_back(
            text("(focus node has no neighbours within depth)") | dim | center);
        return vbox(std::move(rows)) | yframe | xframe | flex;
    }

    // ── Matrix mode (N×N grid) ──────────────────────────────────────────
    Element render_matrix() const {
        const std::size_t N = data_.nodes.size();
        if (N == 0) return text("(no data)");
        if (N > 40) {
            return vbox({
                text(std::format(
                    "Too many nodes (N={}) to display adjacency matrix.", N))
                    | color(Color::Yellow) | center,
                text(""),
                text("Select a focus node (j/k + Enter) to zoom in.")
                    | dim | center,
            }) | flex | center;
        }
        // Header row
        Elements grid;
        {
            Elements hdr;
            hdr.push_back(text("     "));  // column label corner
            for (std::size_t c = 0; c < N; ++c) {
                auto lab = trunc4(data_.nodes[c].label);
                while (lab.size() < 4) lab.push_back(' ');
                Element cell = text(lab) | color(Color::CyanLight);
                if (static_cast<int>(c) == sel_idx_)
                    cell = cell | underlined;
                hdr.push_back(cell);
            }
            grid.push_back(hbox(std::move(hdr)));
        }
        // Body
        for (std::size_t r = 0; r < N; ++r) {
            Elements row;
            // Row label
            auto lab = trunc4(data_.nodes[r].label);
            while (lab.size() < 4) lab.push_back(' ');
            Element rlab = text(std::format("{} ", lab))
                | color(kind_color(data_.nodes[r].kind));
            if (static_cast<int>(r) == sel_idx_) rlab = rlab | underlined;
            row.push_back(rlab);

            for (std::size_t c = 0; c < N; ++c) {
                const double s = edge_strength_[r][c];
                const char* glyph = "·";
                Color fg = Color::GrayDark;
                if (r == c) {
                    glyph = "█"; fg = kind_color(data_.nodes[r].kind);
                } else if (s >= 0.5) {
                    glyph = "█"; fg = Color::GreenLight;
                } else if (s > 0.0) {
                    glyph = "·"; fg = Color::BlueLight;
                }
                Element cell = text(std::format(" {}  ", glyph)) | color(fg);
                if (s > 0.0
                    && (static_cast<int>(r) == sel_idx_
                        || static_cast<int>(c) == sel_idx_))
                    cell = cell | bgcolor(Color::RGB(40, 40, 60));
                row.push_back(cell);
            }
            grid.push_back(hbox(std::move(row)));
        }
        // Legend
        grid.push_back(separator());
        grid.push_back(hbox({
            text("· no edge  ") | dim,
            text("· weak edge  ") | color(Color::BlueLight),
            text("█ strong edge  ") | color(Color::GreenLight),
            text("█ diagonal node") | color(Color::Cyan),
        }) | dim | center);
        return vbox(std::move(grid)) | xframe | yframe | flex;
    }
};

[[nodiscard]] inline Component MakeGroveView(
    grove_view::GroveData data,
    grove_view::GroveCallbacks cb)
{
    if (!config::feature("kairos_grove"))
        return Renderer([] {
            return feature_disabled_placeholder("kairos_grove");
        });
    return Make<GroveViewBase>(std::move(data), std::move(cb));
}

#else  // !FEATURE_KAIROS_GROVE

namespace grove_view {
struct GroveDataStub {
    std::vector<int> nodes;
    std::vector<int> edges;
    std::optional<int> focus;
};
struct GroveCallbacksStub {
    std::function<void(int)> on_focus;
    std::function<void(int)> on_select;
};
}
[[nodiscard]] inline Component MakeGroveView(
    grove_view::GroveDataStub,
    grove_view::GroveCallbacksStub)
{
    return Renderer([] {
        return feature_disabled_placeholder("kairos_grove");
    });
}

#endif  // FEATURE_KAIROS_GROVE

#if 1
using MemorySelectorCallbacks =
    typename MemoryFileSelectorBase::MemoryCallbacks;
using MemorySelectorProvider =
    typename MemoryFileSelectorBase::MemoryBankProvider;
using MemoryEntryT = typename MemoryFileSelectorBase::MemoryEntry;
using MemoryBankT = typename MemoryFileSelectorBase::Bank;
#endif

#if 1
using FeedbackOptionsT = feedback_dialog::FeedbackOptions;
#endif

#if 1
using GroveNodeT = grove_view::GNode;
using GroveEdgeT = grove_view::GEdge;
using GroveDataT = grove_view::GroveData;
using GroveCallbacksT = grove_view::GroveCallbacks;
using GroveViewModeT = grove_view::ViewMode;
#endif

} // namespace cc::ui::components::feature_dialogs
