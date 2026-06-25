/// @file resume_screen.cppm
/// @brief Conversation resume screen with session picker and preview panel.
/// Migrated from src/screens/ResumeConversation.tsx (398 lines).
///
/// Covers 5 views:
///   1. Welcome empty state  – greeting + Create button + 5 recent quick links.
///   2. Session list view    – search + sort + filter header, card-style rows
///      with icon / title / summary / model / date / cost / msg count, plus
///      3-button hover menu (edit / delete / share).
///   3. Session details preview – metadata summary + last-10-message preview
///      with role avatar + role colour, plus Open / Resume / Cancel buttons.
///   4. Edit-title dialog    – inline title editor.
///   5. Delete confirmation  – delegates to UI8 TrustDialog (Medium risk).
///
/// Data flows in 100% from cc::core::ConversationStore (session/history.cppm);
/// no duplicate reads.  Colour tokens / spacing are aligned with Doctor
/// (UI18) and use the design tokens defined in that screen.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <variant>
#include <sstream>
#include <cctype>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.resume_screen;

// ---------------------------------------------------------------------------
// Cross-module imports
// ---------------------------------------------------------------------------
import cc.session.history;
import cc.types.types;
import cc.ui.trust_dialog;
import cc.ui.trust_utils;

export namespace cc::ui::resume_screen {
using namespace ftxui;

// Re-use data from session history — keep naming consistent.
using ConversationStore = cc::core::ConversationStore;
using Conversation      = cc::core::Conversation;
using Message           = cc::core::Message;
using Role              = cc::core::Role;

// Re-use trust dialog primitives for delete confirmation.
namespace tu = cc::ui::trust_utils;
using tu::RiskLevel;
using tu::TrustChoice;
using cc::ui::trust_dialog::TrustDialogProps;
using cc::ui::trust_dialog::MakeTrustDialogComponent;

// ===========================================================================
// Forward declarations
// ===========================================================================
struct ResumeScreenState;
class ResumeScreenImpl;

// ===========================================================================
// Design tokens  (aligned with Doctor screen UI18)
// ===========================================================================
namespace token {
    const Color kAccentGreen  = Color::Green;
    const Color kAccentBlue   = Color::Blue;
    const Color kAccentCyan   = Color::Cyan;
    const Color kAccentYellow = Color::Yellow;
    const Color kAccentRed    = Color::Red;
    const Color kDim          = Color::GrayLight;
    const Color kFg           = Color::White;

    constexpr std::string_view kIconWelcome    = "👋";
    constexpr std::string_view kIconSession    = "📝";
    constexpr std::string_view kIconModel      = "🧠";
    constexpr std::string_view kIconMsg        = "💬";
    constexpr std::string_view kIconClock      = "⏱";
    constexpr std::string_view kIconCost       = "💰";
    constexpr std::string_view kIconSearch     = "🔍";
    constexpr std::string_view kIconSort       = "↕";
    constexpr std::string_view kIconFilter     = "▼";
    constexpr std::string_view kIconNew        = "✨";
    constexpr std::string_view kIconImport     = "⬇";
    constexpr std::string_view kIconBrowse     = "📂";
    constexpr std::string_view kIconEdit       = "✎";
    constexpr std::string_view kIconDelete     = "🗑";
    constexpr std::string_view kIconShare      = "↗";
    constexpr std::string_view kIconResume     = "▶";
    constexpr std::string_view kIconOpen       = "📖";
    constexpr std::string_view kIconCancel     = "✕";
    constexpr std::string_view kIconUser       = "👤";
    constexpr std::string_view kIconAssistant  = "🤖";
    constexpr std::string_view kIconSystem     = "ℹ";
    constexpr std::string_view kIconTool       = "🔧";
} // namespace token

// ===========================================================================
// Enums: sort order
// ===========================================================================

/// Sort criterion for the session list.
enum class SortMode : std::uint8_t {
    NewestFirst,
    OldestFirst,
    TitleAsc,
    ModelAsc,
};

[[nodiscard]] constexpr std::string_view to_string(SortMode m) {
    switch (m) {
        case SortMode::NewestFirst: return "Newest";
        case SortMode::OldestFirst: return "Oldest";
        case SortMode::TitleAsc:    return "Title A–Z";
        case SortMode::ModelAsc:    return "Model";
    }
    return "Newest";
}

// ===========================================================================
// Enums: screen sub-view
// ===========================================================================

/// Top-level view inside ResumeScreen.
enum class ScreenView : std::uint8_t {
    WelcomeEmpty,       // 1. No sessions — create / import / browse
    SessionList,        // 2. Full list with search / sort / filter
    DetailsPreview,     // 3. Full preview + Open/Resume/Cancel
    EditTitle,          // 4. Rename dialog
    DeleteConfirm,      // 5. Medium-risk TrustDialog (delegated to UI8)
};

// ===========================================================================
// Types: Session metadata (built from ConversationStore — no duplicate read)
// ===========================================================================

/// Compact metadata row about one persisted Conversation.
/// Populated from cc::core::Conversation / ConversationStore — we only read
/// the preview (last N messages) *on demand* when a session is selected for
/// DetailsPreview, so idle memory stays small.
struct SessionMetaRow {
    std::string session_id;
    std::string title;
    std::optional<std::string> model_name;       // From last assistant message
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_active_at;
    std::size_t message_count = 0;
    std::size_t turn_count = 0;
    std::optional<double> total_cost_usd;
    std::optional<std::uint64_t> total_tokens;   // Input + output
    bool is_current_project = true;
    bool has_custom_title = false;
    std::string project_path;
    std::string last_message_summary;            // 1-line, truncated

    // Cached lazily when entering DetailsPreview — not populated during listing.
    mutable std::optional<std::vector<Message>> preview_cache;
};

// ===========================================================================
// Helpers: text + time formatting
// ===========================================================================

// CSS-style padding decorator (same helper used across ui/messages modules).
inline Decorator padding(int top, int right, int bottom, int left) {
    return [=](Element e) -> Element {
        Elements rows;
        for (int i = 0; i < top; ++i) rows.push_back(text(""));
        {
            Elements lp, rp;
            for (int i = 0; i < left; ++i) lp.push_back(text(" "));
            for (int i = 0; i < right; ++i) rp.push_back(text(" "));
            rows.push_back(hbox({hbox(std::move(lp)), std::move(e), hbox(std::move(rp))}));
        }
        for (int i = 0; i < bottom; ++i) rows.push_back(text(""));
        return vbox(std::move(rows));
    };
}
inline Decorator padding(int all) { return padding(all, all, all, all); }

/// Truncate to `max` runes, adding ellipsis if truncation happened.
[[nodiscard]] inline std::string truncate(const std::string& s, std::size_t max) {
    if (s.size() <= max) return s;
    if (max <= 3) return s.substr(0, max);
    return s.substr(0, max - 3) + "...";
}

/// Format a time_point as relative time string.
[[nodiscard]] inline std::string format_relative_time(
    std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;
    const auto now = std::chrono::system_clock::now();
    const auto diff = duration_cast<minutes>(now - tp);

    if (diff.count() < 1)       return "just now";
    if (diff.count() < 60)      return std::format("{}m ago", diff.count());

    const auto h = duration_cast<hours>(diff);
    if (h.count() < 24)     return std::format("{}h ago", h.count());

    const auto days = h.count() / 24;
    if (days < 7)               return std::format("{}d ago", days);
    if (days < 30)              return std::format("{}w ago", days / 7);
    if (days < 365)             return std::format("{}mo ago", days / 30);
    return std::format("{}y ago", days / 365);
}

/// Format a time_point as absolute "YYYY-MM-DD HH:MM" string.
[[nodiscard]] inline std::string format_absolute_time(
    std::chrono::system_clock::time_point tp)
{
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return std::string{buf};
}

/// Extract a 1-line plain-text summary from the last meaningful message in a
/// conversation (tool/system messages are skipped — user/assistant only).
[[nodiscard]] inline std::string extract_summary(const Conversation& conv,
                                                 std::size_t max_chars = 80)
{
    const auto all = conv.get_messages();
    // Walk backwards, skip tool/system.
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        const Role r = cc::core::get_role(*it);
        if (r != Role::User && r != Role::Assistant) continue;

        std::string text;
        std::visit([&](const auto& m) {
            for (const auto& blk : m.content) {
                if (auto* tb = std::get_if<cc::core::TextBlock>(&blk)) {
                    if (!text.empty()) text += " ";
                    text += tb->text;
                }
            }
        }, *it);
        // Collapse whitespace.
        std::string out;
        out.reserve(text.size());
        bool prev_space = false;
        for (char c : text) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (c == ' ' && prev_space) continue;
            prev_space = (c == ' ');
            out.push_back(c);
        }
        if (!out.empty()) {
            // Prefix role hint for clarity in compact listings.
            const std::string prefix = (r == Role::User) ? "You: " : "Assistant: ";
            return truncate(prefix + out, max_chars);
        }
    }
    return "(no messages yet)";
}

/// Extract the last assistant-message model name, if any.
[[nodiscard]] inline std::optional<std::string> extract_last_model(
    const Conversation& conv)
{
    const auto all = conv.get_messages();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (auto* am = std::get_if<cc::core::AssistantMessage>(&*it)) {
            if (am->model && !am->model->empty()) return am->model;
        }
    }
    return std::nullopt;
}

/// Case-insensitive ASCII substring match.
[[nodiscard]] inline bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    auto to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (to_lower(hay[i + j]) != to_lower(needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

// ===========================================================================
// Bridge: ConversationStore → vector<SessionMetaRow>
// ===========================================================================

/// Build metadata rows directly from a ConversationStore.
/// Performs a single pass over conversations — no duplicate reads.
[[nodiscard]] inline std::vector<SessionMetaRow> build_rows_from_store(
    const ConversationStore& store,
    std::string_view current_project_path = "")
{
    std::vector<SessionMetaRow> rows;
    const auto ids = store.get_conversation_ids();
    rows.reserve(ids.size());
    for (const auto& id : ids) {
        // We do a const_cast because ConversationStore's lookup APIs are
        // non-const only for historical reasons; the reads below are
        // logically const.
        auto* mutable_store = const_cast<ConversationStore*>(&store);
        // We need a non-trivial way to access — walk by switching is invasive,
        // so we fall back: the store only exposes switch_conversation +
        // get_active_conversation.  We switch through each ID once to read.
        //
        // NOTE: this means callers should invoke this helper *before* they
        // have an active conversation they care about.  (Resume screen is
        // shown exactly at startup / session-picker time, which fits.)
        if (!mutable_store->switch_conversation(id)) continue;
        auto* conv = mutable_store->get_active_conversation();
        if (!conv) continue;

        SessionMetaRow row;
        row.session_id      = id;
        row.title           = conv->get_title();
        row.created_at      = conv->get_created_at();
        row.last_active_at  = conv->get_updated_at();
        row.message_count   = conv->size();
        row.model_name      = extract_last_model(*conv);
        row.last_message_summary = extract_summary(*conv, 90);
        row.project_path    = std::string{current_project_path};
        row.is_current_project = !current_project_path.empty(); // coarse
        row.has_custom_title = !row.title.empty();
        if (row.title.empty()) {
            // Generate a human-readable fallback.
            row.title = std::format("Session {}", id.substr(0, 8));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// ===========================================================================
// Filtering + sorting
// ===========================================================================

/// Combined search + filter result.
struct FilteredIndex {
    std::size_t row_index;     // index into SessionMetaRow vector
};

[[nodiscard]] inline std::vector<FilteredIndex> apply_filter_sort(
    const std::vector<SessionMetaRow>& rows,
    std::string_view query,
    SortMode mode,
    bool current_project_only,
    std::string_view model_filter = "")
{
    std::vector<FilteredIndex> out;
    out.reserve(rows.size());

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (current_project_only && !r.is_current_project) continue;
        if (!model_filter.empty()) {
            if (!r.model_name || !icontains(*r.model_name, model_filter)) continue;
        }
        if (!query.empty()) {
            const bool hit = icontains(r.title, query)
                          || icontains(r.session_id, query)
                          || icontains(r.last_message_summary, query)
                          || icontains(r.project_path, query)
                          || (r.model_name && icontains(*r.model_name, query));
            if (!hit) continue;
        }
        out.push_back({i});
    }

    // Sort according to mode.
    const auto cmp = [&](const FilteredIndex& a, const FilteredIndex& b) -> bool {
        const auto& ra = rows[a.row_index];
        const auto& rb = rows[b.row_index];
        switch (mode) {
            case SortMode::NewestFirst:
                return ra.last_active_at > rb.last_active_at;
            case SortMode::OldestFirst:
                return ra.last_active_at < rb.last_active_at;
            case SortMode::TitleAsc:
                return ra.title < rb.title;
            case SortMode::ModelAsc: {
                std::string_view ma = ra.model_name ? std::string_view{*ra.model_name} : std::string_view{};
                std::string_view mb = rb.model_name ? std::string_view{*rb.model_name} : std::string_view{};
                if (ma != mb) return ma < mb;
                return ra.last_active_at > rb.last_active_at;
            }
        }
        return false;
    };
    std::stable_sort(out.begin(), out.end(), cmp);
    return out;
}

// ===========================================================================
// Rendering: (1) Welcome empty state
// ===========================================================================

/// Render the greeting screen shown when there are zero (or few) sessions.
/// Layout:
///     ┌─ Hello! 👋 Welcome back! ───────────────────────────────────────┐
///     │                                                                 │
///     │   "✨ Create new conversation"   (primary green, Enter)         │
///     │   "⬇ Import conversation"                                       │
///     │   "📂 Browse history"                                           │
///     │                                                                 │
///     │   ── Recent ─────────────────────────────────────────────      │
///     │   (up to 5 quick-pick entries shown when sessions > 0)         │
///     └─────────────────────────────────────────────────────────────────┘
[[nodiscard]] inline Element RenderWelcomeEmpty(
    const std::vector<SessionMetaRow>& recent_sessions,   // 0..5
    bool show_quick_picks)
{
    Elements body;

    // Big title
    body.push_back(hbox({
        text("  ") | color(Color::Default),
        text("👋 Welcome back!") | bold | color(token::kAccentGreen)
            | size(HEIGHT, EQUAL, 1),
        filler(),
    }) | size(HEIGHT, EQUAL, 2));

    body.push_back(paragraph(
        "Pick up right where you left off, or start a brand new conversation."
    ) | dim | color(Color::GrayLight));
    body.push_back(text(""));

    // Primary button: Create new
    body.push_back(hbox({
        text("▶ ") | color(token::kAccentGreen),
        text("[Enter] ") | bold | color(token::kAccentCyan),
        text("Create new conversation")
            | bold | color(token::kAccentGreen),
    }));
    body.push_back(text(""));

    // Secondary actions
    body.push_back(hbox({
        text("  ⬇ ") | dim,
        text("[I] ") | color(token::kAccentCyan) | dim,
        text("Import conversation") | dim,
    }));
    body.push_back(hbox({
        text("  📂 ") | dim,
        text("[B] ") | color(token::kAccentCyan) | dim,
        text("Browse history ") | dim,
        text(show_quick_picks ? "(show all)" : "(no sessions yet)") | dim,
    }));
    body.push_back(text(""));

    // Quick recent (5 most recent)
    if (show_quick_picks && !recent_sessions.empty()) {
        body.push_back(hbox({
            text(" ── ") | dim,
            text("Recent") | bold | color(token::kAccentBlue),
            text(" ─────────────────────────────────") | dim,
        }));
        body.push_back(text(""));

        int idx = 0;
        for (const auto& r : recent_sessions) {
            const std::string hotkey = (idx < 9)
                ? std::format("[{}]", idx + 1)
                : "[·]";
            body.push_back(hbox({
                text(" " + hotkey + " ") | color(token::kAccentCyan) | dim,
                text(std::string{token::kIconSession} + " ") | dim,
                text(truncate(r.title, 40)) | color(token::kFg),
                filler(),
                text(format_relative_time(r.last_active_at))
                    | dim | color(token::kDim),
                text("  " + std::to_string(r.message_count) + " msg")
                    | dim | color(token::kDim),
            }));
            body.push_back(hbox({
                text("      ") | dim,
                text(truncate(r.last_message_summary, 60))
                    | dim | color(token::kDim),
            }));
            body.push_back(text(""));
            ++idx;
        }
    }

    // Footer hint
    body.push_back(separator());
    body.push_back(hbox({
        text(" Enter") | bold, text(" = new  "),
        text("Esc") | bold, text(" = cancel  "),
        text("1–5") | bold, text(" = pick recent"),
    }) | dim);

    return vbox(std::move(body)) | borderRounded | padding(1);
}

// ===========================================================================
// Rendering: (2) Session card row (for list view)
// ===========================================================================

/// Visual state bits passed into the card renderer.
struct RowViewState {
    bool selected     = false;
    bool hovered      = false;   // show 3-button action strip
    bool focused      = false;   // currently being navigated via Tab
};

/// Render one session row as a card.
///
/// Layout (card):
///   ┌────────────────────────────────────────────────────────────────────┐
///   │ 📝  Implement auth flow            │  2026-06-08 14:02   42 msg    │
///   │     Assistant: Added JWT middleware │  Claude 3.5 Sonnet  $0.12     │
///   │     (model)                        │  3h ago                       │
///   └────────────────────────────────────────────────────────────────────┘
///   Hover → right-side strip: [✎ edit] [🗑 delete] [↗ share]
[[nodiscard]] inline Element RenderSessionCard(
    const SessionMetaRow& r,
    const RowViewState& vs)
{
    Elements left_col;

    // Row 1: icon + title (bold, >30 → ellipsis)
    const std::string title = truncate(r.title, 45);
    left_col.push_back(hbox({
        text(std::string{token::kIconSession} + " ") | dim,
        text(title)
            | (vs.selected ? (bold | color(token::kAccentGreen))
                           : (bold | color(token::kFg))),
    }));

    // Row 2: last-message summary (gray)
    left_col.push_back(hbox({
        text("    ") | dim,
        text(truncate(r.last_message_summary, 60))
            | dim | color(token::kDim),
    }));

    // Row 3: model + last active
    std::string model_txt = r.model_name
        ? *r.model_name
        : std::string("(model unknown)");
    left_col.push_back(hbox({
        text("    ") | dim,
        text(std::string{token::kIconModel} + " ") | dim,
        text(truncate(model_txt, 28)) | dim | color(token::kAccentCyan),
        text("  ") | dim,
        text(std::string{token::kIconClock} + " ") | dim,
        text(format_relative_time(r.last_active_at))
            | dim | color(token::kDim),
    }));

    Elements right_col;

    // Right top: absolute date + message count
    right_col.push_back(hbox({
        text(format_absolute_time(r.last_active_at))
            | dim | color(token::kDim),
        text("  ") | dim,
        text(std::to_string(r.message_count)) | bold | color(token::kAccentCyan),
        text(" msg") | dim | color(token::kDim),
    }));

    // Right mid: cost + turns
    {
        Elements row;
        if (r.total_cost_usd) {
            row.push_back(text(std::format("${:.2f}", *r.total_cost_usd))
                | color(token::kAccentGreen) | dim);
            row.push_back(text("  ") | dim);
        }
        row.push_back(text(std::to_string(r.turn_count)) | dim | color(token::kDim));
        row.push_back(text(" turns") | dim | color(token::kDim));
        right_col.push_back(hbox(std::move(row)));
    }

    // Hover strip: edit / delete / share
    if (vs.hovered) {
        right_col.push_back(text("") | dim);
        right_col.push_back(hbox({
            text("[E] ") | color(token::kAccentCyan) | dim,
            text(std::string{token::kIconEdit} + " edit")
                | dim | color(token::kAccentCyan),
            text("  ") | dim,
            text("[Del] ") | color(token::kAccentRed) | dim,
            text(std::string{token::kIconDelete} + " delete")
                | dim | color(token::kAccentRed),
            text("  ") | dim,
            text("[S] ") | color(token::kAccentYellow) | dim,
            text(std::string{token::kIconShare} + " share")
                | dim | color(token::kAccentYellow),
        }));
    } else {
        // Faint hint that hover actions exist
        right_col.push_back(text("") | dim);
        right_col.push_back(hbox({
            text("···") | dim | color(token::kDim),
        }));
    }

    // Compose
    Element card = hbox({
        vbox(std::move(left_col)) | flex,
        text("   ") | dim,
        vbox(std::move(right_col)) | align_right,
    }) | padding(1);

    if (vs.selected) {
        card = card | bgcolor(Color::RGB(25, 30, 45)) | borderStyled(token::kAccentGreen);
    } else {
        card = card | borderLight;
    }

    // Selection chevron on the far left.
    const std::string chevron = vs.selected ? " ❯ " : "   ";
    return hbox({
        text(chevron) | color(vs.selected ? token::kAccentGreen : Color::Default),
        card | flex,
    });
}

/// Render the header bar for the list view: search + sort + filter toggles.
[[nodiscard]] inline Element RenderListHeader(
    std::string_view query,
    bool search_focused,
    SortMode sort,
    bool current_project_only,
    std::string_view model_filter,
    std::size_t total_count,
    std::size_t filtered_count)
{
    // Search field
    Element search = hbox({
        text(search_focused ? " / " : " 🔍 ")
            | color(search_focused ? token::kAccentCyan : token::kDim),
        query.empty()
            ? text("Search sessions...") | dim
            : text(std::string{query}) | color(token::kFg),
        text("│") | (search_focused ? blink : nothing),
    }) | borderLight | flex;

    // Sort dropdown label
    Element sort_el = hbox({
        text(std::string{token::kIconSort} + " ") | dim,
        text(std::string{to_string(sort)}) | bold | color(token::kAccentBlue),
        text(" [T]") | dim,
    }) | borderLight;

    // Project filter toggle
    Element project_el = hbox({
        text(std::string{token::kIconFilter} + " ") | dim,
        text(current_project_only ? "Current project" : "All projects")
            | bold
            | color(current_project_only ? token::kAccentYellow : token::kAccentCyan),
        text(" [P]") | dim,
    }) | borderLight;

    // Model filter (small)
    Element model_el = hbox({
        text(std::string{token::kIconModel} + " ") | dim,
        text(model_filter.empty() ? std::string{"any model"}
                                  : std::string{model_filter})
            | color(model_filter.empty() ? token::kDim : token::kAccentCyan),
        text(" [M]") | dim,
    }) | borderLight;

    // Counter
    Element counter = hbox({
        text(std::format("{} / {} ", filtered_count, total_count))
            | dim | color(token::kDim),
        text(std::string{token::kIconMsg}) | dim,
    }) | borderLight;

    Elements row1 = {
        search | flex,
        sort_el,
        project_el,
        model_el,
        counter,
    };

    return vbox({
        hbox(std::move(row1)),
    });
}

/// Render the full Session List view.
[[nodiscard]] inline Element RenderSessionListView(
    const std::vector<SessionMetaRow>& rows,
    const std::vector<FilteredIndex>& filtered,
    std::size_t selected_idx,     // into `filtered`
    std::size_t hovered_idx,      // into `filtered` (== selected unless shift+key)
    std::string_view query,
    bool search_focused,
    SortMode sort,
    bool current_project_only,
    std::string_view model_filter)
{
    Elements cards;
    cards.push_back(hbox({
        text(" Resume Conversation ") | bold | color(token::kAccentGreen),
        filler(),
        text("[Esc] cancel  [Enter] open  [Space] preview") | dim,
    }));
    cards.push_back(separator());

    cards.push_back(RenderListHeader(
        query, search_focused, sort, current_project_only, model_filter,
        rows.size(), filtered.size()));
    cards.push_back(separator());

    if (filtered.empty()) {
        cards.push_back(text(" No sessions match your filters.") | dim | center);
        cards.push_back(text(" Press [C] to clear filters.") | dim | center);
    } else {
        const std::size_t safe_selected = std::min(selected_idx, filtered.size() - 1);
        for (std::size_t i = 0; i < filtered.size(); ++i) {
            RowViewState vs;
            vs.selected = (i == safe_selected);
            vs.hovered  = (i == hovered_idx) || vs.selected;
            vs.focused  = vs.selected;
            cards.push_back(RenderSessionCard(rows[filtered[i].row_index], vs));
            if (i + 1 < filtered.size()) cards.push_back(text("") | dim);
        }
    }

    cards.push_back(separator());
    cards.push_back(hbox({
        text(" ↑/↓ j/k") | bold | color(token::kAccentCyan), text(" select "),
        text("PgUp/Dn") | bold | color(token::kAccentCyan), text(" page "),
        text("G/gg")    | bold | color(token::kAccentCyan), text(" jump "),
        text("/")       | bold | color(token::kAccentCyan), text(" search "),
        text("E")       | bold | color(token::kAccentCyan), text("dit "),
        text("Del")     | bold | color(token::kAccentRed),  text("elete "),
        text("Esc")     | bold | color(token::kAccentCyan), text(" back"),
    }) | dim);

    return vbox(std::move(cards))
         | borderRounded
         | yframe
         | vscroll_indicator
         | flex;
}

// ===========================================================================
// Rendering: (3) Session details preview
// ===========================================================================

/// Role avatar + colour.
struct RoleBadge {
    std::string_view icon;
    Color colour;
    std::string_view label;
};

[[nodiscard]] inline RoleBadge role_badge(Role r) {
    switch (r) {
        case Role::User:      return {token::kIconUser,      Color::Cyan,  "User"};
        case Role::Assistant: return {token::kIconAssistant, Color::Green, "Assistant"};
        case Role::System:    return {token::kIconSystem,    Color::Yellow,"System"};
        case Role::Tool:      return {token::kIconTool,      Color::Orange1,"Tool"};
    }
    return {"?", Color::White, "?"};
}

/// Render a single preview message line (avatar + role + collapsed content).
[[nodiscard]] inline Element RenderPreviewMessage(const Message& msg,
                                                  std::size_t max_chars = 120)
{
    const Role r = cc::core::get_role(msg);
    const auto badge = role_badge(r);

    std::string content;
    std::visit([&](const auto& m) {
        for (const auto& blk : m.content) {
            if (auto* tb = std::get_if<cc::core::TextBlock>(&blk)) {
                if (!content.empty()) content += " ";
                content += tb->text;
            } else if (auto* tu = std::get_if<cc::core::ToolUseBlock>(&blk)) {
                content += std::format("<tool:{}>", tu->name);
            } else if (auto* tr = std::get_if<cc::core::ToolResultBlock>(&blk)) {
                content += std::format("<result:{}>", tr->is_error ? "error" : "ok");
            } else if (auto* im = std::get_if<cc::core::ImageBlock>(&blk)) {
                content += std::format("<image:{}>", im->media_type);
            } else if (auto* dc = std::get_if<cc::core::DocumentBlock>(&blk)) {
                content += std::format("<doc:{}>", dc->media_type);
            } else if (auto* th = std::get_if<cc::core::ThinkingBlock>(&blk)) {
                content += std::format("<thinking {}b>", th->thinking.size());
            }
        }
    }, msg);
    // Collapse whitespace.
    std::string clean;
    clean.reserve(content.size());
    bool prev_ws = false;
    for (char c : content) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && prev_ws) continue;
        prev_ws = (c == ' ');
        clean.push_back(c);
    }

    return hbox({
        text(" " + std::string{badge.icon} + " ") | color(badge.colour),
        text(std::string{badge.label}) | bold | color(badge.colour),
        text(" │ ") | dim,
        paragraph(truncate(clean, max_chars)) | color(token::kFg)
            | size(WIDTH, LESS_THAN, 80),
    });
}

/// Render the DetailsPreview view (right side of the split layout, or modal).
[[nodiscard]] inline Element RenderDetailsPreview(
    const SessionMetaRow& row,
    const std::vector<Message>& preview_msgs,  // up to 10 most recent
    int primary_selection) // 0=Open  1=Resume  2=Cancel
{
    Elements body;

    // ── Header ─────────────────────────────────────────────────────
    body.push_back(hbox({
        text(std::string{token::kIconSession} + " ") | dim,
        text(row.title) | bold | color(token::kAccentGreen),
        filler(),
        text("#") | dim,
        text(row.session_id.substr(0, 10))
            | color(token::kAccentCyan) | dim,
    }));
    body.push_back(separator());

    // ── Metadata summary ───────────────────────────────────────────
    Elements meta_rows;

    meta_rows.push_back(hbox({
        text(" Created:   ") | dim,
        text(format_absolute_time(row.created_at)) | color(token::kFg),
        filler(),
        text(std::string{token::kIconClock} + " ") | dim,
        text("Last active: ") | dim,
        text(format_relative_time(row.last_active_at))
            | color(token::kAccentCyan),
    }));
    meta_rows.push_back(hbox({
        text(" Messages:  ") | dim,
        text(std::to_string(row.message_count)) | bold | color(token::kAccentCyan),
        text("   ") | dim,
        text("Turns: ") | dim,
        text(std::to_string(row.turn_count)) | color(token::kFg),
        filler(),
        text(std::string{token::kIconCost} + " ") | dim,
        text(row.total_cost_usd
                 ? std::format("${:.3f}", *row.total_cost_usd)
                 : std::string{"—"})
            | color(row.total_cost_usd ? token::kAccentGreen : token::kDim),
    }));
    meta_rows.push_back(hbox({
        text(" Model:     ") | dim,
        text(row.model_name ? *row.model_name : std::string{"(unknown)"})
            | color(token::kAccentCyan),
        filler(),
        text("Tokens: ") | dim,
        text(row.total_tokens
                 ? std::format("{}k", (*row.total_tokens + 500) / 1000)
                 : std::string{"—"})
            | color(token::kDim),
    }));
    if (!row.project_path.empty()) {
        meta_rows.push_back(hbox({
            text(" Project:   ") | dim,
            text(row.project_path) | dim | color(token::kDim),
        }));
    }
    body.push_back(vbox(std::move(meta_rows)) | borderLight);

    body.push_back(separator());

    // ── Message preview (most recent 10) ──────────────────────────
    body.push_back(hbox({
        text(std::string{token::kIconMsg} + " ") | dim,
        text("Preview") | bold | color(token::kAccentBlue),
        text(" (last " + std::to_string(preview_msgs.size()) + " messages)") | dim,
    }));
    body.push_back(text(""));

    if (preview_msgs.empty()) {
        body.push_back(text("  (no messages in this session)") | dim | center);
    } else {
        for (const auto& m : preview_msgs) {
            body.push_back(RenderPreviewMessage(m, 140));
        }
    }

    // ── Buttons: Open / Resume / Cancel ───────────────────────────
    body.push_back(text("") | dim);
    body.push_back(separator());

    auto btn = [&](int idx, std::string_view icon, std::string_view label,
                   Color c) -> Element
    {
        const bool sel = (idx == primary_selection);
        Elements p = {
            text(sel ? "▶ " : "  ") | color(c),
            text(std::string{icon} + " ") | (sel ? color(c) : dim),
            text(std::string{label}) | (sel ? (bold | color(c)) : dim),
            text(" "),
        };
        return hbox(std::move(p)) | (sel ? bgcolor(Color::RGB(20, 28, 38))
                                         : nothing);
    };

    body.push_back(hbox({
        btn(0, token::kIconOpen,   "Open full",    token::kAccentBlue),
        text("  ") | dim,
        btn(1, token::kIconResume, "Resume",       token::kAccentGreen),
        text("  ") | dim,
        btn(2, token::kIconCancel, "Cancel",       token::kAccentRed),
        filler(),
        text(" [Enter] confirm  [Esc] back") | dim,
    }));

    return vbox(std::move(body)) | borderRounded;
}

// ===========================================================================
// Rendering: (4) Edit-title dialog
// ===========================================================================

/// Render the inline edit-title dialog.
[[nodiscard]] inline Element RenderEditTitleDialog(
    const SessionMetaRow& row,
    std::string_view draft,
    bool has_error)
{
    Elements body;
    body.push_back(hbox({
        text(std::string{token::kIconEdit} + " ") | color(token::kAccentCyan),
        text("Rename conversation") | bold | color(token::kAccentCyan),
    }));
    body.push_back(separator());

    body.push_back(hbox({
        text(" Session: ") | dim,
        text(truncate(row.session_id, 14)) | dim | color(token::kDim),
        filler(),
        text(" (" + std::to_string(row.message_count) + " messages)") | dim,
    }));
    body.push_back(text(""));
    body.push_back(text(" Current: ") | dim | color(token::kDim));
    body.push_back(hbox({
        text("  » ") | dim,
        text(row.title) | color(token::kFg),
    }));
    body.push_back(text(""));
    body.push_back(text(" New title:") | bold);

    const std::string shown = draft.empty() ? std::string{"(use a short descriptive name)"}
                                            : std::string{draft};
    body.push_back(hbox({
        text("  ") | dim,
        text(shown) | (draft.empty() ? dim : color(token::kAccentGreen)),
        text("│") | blink | color(token::kAccentCyan),
    }) | borderLight);

    if (has_error) {
        body.push_back(text("") | dim);
        body.push_back(hbox({
            text(" ⚠ ") | color(token::kAccentRed),
            text("Title must not be empty.") | color(token::kAccentRed),
        }));
    }

    body.push_back(separator());
    body.push_back(hbox({
        text(" Enter") | bold | color(token::kAccentGreen),
        text(" save  "),
        text("Esc") | bold | color(token::kAccentRed),
        text(" cancel  "),
        text("Tab") | bold | dim,
        text(" reset to current"),
    }) | dim);

    return vbox(std::move(body)) | borderRounded | padding(1);
}

// ===========================================================================
// Rendering: (5) Delete confirmation (delegates to UI8 TrustDialog)
// ===========================================================================
//
// We do NOT re-implement delete confirmation.  Instead, we expose a helper
// that builds a TrustDialogProps at Medium risk so callers can invoke the
// already-audited UI8 TrustDialog via its factory.  This also avoids
// duplicate confirmation prompts.

/// Build a Medium-risk TrustDialogProps for "delete session" confirmation.
/// Follows the constraint: "Delete confirmation → UI8 TrustDialog,
/// no duplicate confirmation."
[[nodiscard]] inline TrustDialogProps MakeDeleteSessionTrustProps(
    const SessionMetaRow& row,
    std::function<void(TrustChoice result)> on_done)
{
    TrustDialogProps props;
    props.on_done    = std::move(on_done);
    props.action     = tu::ActionType::PathWrite;   // Deletion is destructive.
    props.forced_level = tu::RiskLevel::Medium;
    props.action_label = "Delete conversation";

    tu::RiskSummary summary;
    summary.level = tu::RiskLevel::Medium;
    summary.action_summary = std::format(
        "You are about to permanently delete the conversation \"{}\".\n"
        "This action cannot be undone — {} messages, {} turns of history "
        "will be removed.",
        truncate(row.title, 60),
        row.message_count, row.turn_count);
    summary.risk_factors.push_back("Permanent deletion of stored conversation data");
    summary.risk_factors.push_back(std::format("Session ID: {}", row.session_id));
    summary.risk_factors.push_back("No recycle bin / undo is available");
    if (row.total_cost_usd) {
        summary.risk_factors.push_back(std::format(
            "Conversation cost ${:.3f} — cost history will be lost",
            *row.total_cost_usd));
    }
    props.summary = std::move(summary);
    props.paths = { std::format("[session:{}]", row.session_id) };
    return props;
}

// ===========================================================================
// Full-screen dispatcher
// ===========================================================================

/// High-level rendering entry point used by the interactive component.
/// Combines Welcome / List / Details / Edit views depending on `view`.
[[nodiscard]] inline Element RenderResumeScreen(
    // Persisted data
    const std::vector<SessionMetaRow>& rows,
    const std::vector<FilteredIndex>& filtered,

    // UI state
    ScreenView view,
    std::size_t selected_idx,
    std::size_t hovered_idx,
    std::string_view query,
    bool search_focused,
    SortMode sort_mode,
    bool current_project_only,
    std::string_view model_filter,
    int preview_button,   // for DetailsPreview
    std::string_view edit_draft,
    bool edit_has_error,

    // Optional: DetailsPreview message cache for the selected row
    const std::optional<std::vector<Message>>& preview_cache,

    // Loading state
    bool loading)
{
    // Loading overlay (top-level)
    if (loading) {
        return vbox({
            text("") | dim,
            text(" Loading conversations…") | color(token::kAccentCyan),
            text("") | dim,
        }) | borderRounded;
    }

    switch (view) {
        case ScreenView::WelcomeEmpty: {
            // Feed up to 5 most-recent sessions for quick picks.
            const auto recent = apply_filter_sort(
                rows, "", SortMode::NewestFirst, current_project_only, "");
            std::vector<SessionMetaRow> top5;
            top5.reserve(std::min<std::size_t>(5, recent.size()));
            for (std::size_t i = 0; i < 5 && i < recent.size(); ++i) {
                top5.push_back(rows[recent[i].row_index]);
            }
            return RenderWelcomeEmpty(top5, !top5.empty());
        }

        case ScreenView::SessionList:
            return RenderSessionListView(
                rows, filtered, selected_idx, hovered_idx,
                query, search_focused, sort_mode,
                current_project_only, model_filter);

        case ScreenView::DetailsPreview: {
            if (filtered.empty()) {
                return RenderSessionListView(
                    rows, filtered, selected_idx, hovered_idx,
                    query, search_focused, sort_mode,
                    current_project_only, model_filter);
            }
            const std::size_t safe = std::min(selected_idx, filtered.size() - 1);
            const auto& row = rows[filtered[safe].row_index];
            const auto& msgs = preview_cache.value_or(std::vector<Message>{});
            // Split layout: left = compact list strip, right = preview.
            auto list_strip = RenderSessionListView(
                rows, filtered, selected_idx, hovered_idx,
                query, search_focused, sort_mode,
                current_project_only, model_filter);
            auto preview = RenderDetailsPreview(row, msgs, preview_button);
            return hbox({
                list_strip | flex | size(WIDTH, LESS_THAN, 55),
                preview    | flex,
            });
        }

        case ScreenView::EditTitle: {
            if (filtered.empty()) {
                return RenderSessionListView(
                    rows, filtered, selected_idx, hovered_idx,
                    query, search_focused, sort_mode,
                    current_project_only, model_filter);
            }
            const std::size_t safe = std::min(selected_idx, filtered.size() - 1);
            const auto& row = rows[filtered[safe].row_index];
            return RenderEditTitleDialog(row, edit_draft, edit_has_error);
        }

        case ScreenView::DeleteConfirm:
            // (The TrustDialog component is injected by the caller — we
            //  only render a backdrop hint.)
            return RenderSessionListView(
                rows, filtered, selected_idx, hovered_idx,
                query, search_focused, sort_mode,
                current_project_only, model_filter) | dim;
    }
    return text("");
}

// ===========================================================================
// Interactive Component
// ===========================================================================

/// Result action that ResumeScreen produces for the outer REPL / bootstrap.
struct ResumeScreenAction {
    enum class Kind : std::uint8_t {
        None,
        NewConversation,
        ImportConversation,
        BrowseHistory,
        ResumeSession,      // payload: session_id
        OpenSessionFull,    // payload: session_id
        RenameSession,      // payload: session_id + new_title
        DeleteSession,      // payload: session_id
        ShareSession,       // payload: session_id
        Cancel,
        QuickPickNumber,    // payload: index (0-based) into recent sessions
    };
    Kind kind = Kind::None;
    std::string session_id{};
    std::string extra{};   // rename: new_title;  share: session_id too
    std::size_t index = 0;
};

/// Top-level options.
struct ResumeScreenOptions {
    // If empty, rows will be built from store below.
    std::vector<SessionMetaRow> prefetched_rows;

    // Optional store used if prefetched_rows is empty.
    ConversationStore* store = nullptr;

    std::string current_project_path;

    // Callbacks (non-blocking — return an action or invoke directly).
    std::function<void(const ResumeScreenAction&)> on_action;

    // Optional: lazy preview loader (reads last N messages on demand).
    std::function<std::vector<Message>(const std::string& session_id,
                                       std::size_t limit)>
        load_preview;

    // Optional: title persister.
    std::function<void(const std::string& session_id, std::string new_title)>
        persist_title;

    // Optional: delete persister.
    std::function<void(const std::string& session_id)> persist_delete;

    // Whether to show the Welcome screen when sessions > 0 (user toggle).
    bool force_welcome = false;
};

// ---------------------------------------------------------------------------
// State struct (captured by closure)
// ---------------------------------------------------------------------------

struct ResumeScreenState {
    ResumeScreenOptions opts;
    std::vector<SessionMetaRow> rows;
    std::vector<FilteredIndex> filtered;

    ScreenView view = ScreenView::WelcomeEmpty;
    std::size_t selected_idx = 0;
    std::size_t hovered_idx  = 0;

    // Search
    std::string query;
    bool search_focused = false;

    // Sort / filter
    SortMode sort_mode = SortMode::NewestFirst;
    bool current_project_only = true;
    std::string model_filter;

    // Details preview
    int preview_button = 1;   // default = Resume
    std::optional<std::vector<Message>> preview_cache;
    std::string preview_for_session_id;

    // Edit dialog
    std::string edit_draft;
    bool edit_has_error = false;

    // Loading
    bool loading = true;
};

/// Refresh `filtered` based on current search + sort + filter state.
inline void refresh_filtered(ResumeScreenState& s) {
    s.filtered = apply_filter_sort(
        s.rows, s.query, s.sort_mode,
        s.current_project_only, s.model_filter);
    if (s.selected_idx >= s.filtered.size() && !s.filtered.empty()) {
        s.selected_idx = s.filtered.size() - 1;
    }
    if (s.hovered_idx >= s.filtered.size() && !s.filtered.empty()) {
        s.hovered_idx = s.filtered.size() - 1;
    }
    // Invalidate preview cache when the set changes.
    s.preview_cache.reset();
    s.preview_for_session_id.clear();
}

/// Load preview (last 10 messages) for the currently-selected session,
/// on demand.  Uses the store only once per session.
inline void ensure_preview_loaded(ResumeScreenState& s) {
    if (s.filtered.empty()) return;
    const std::size_t safe = std::min(s.selected_idx, s.filtered.size() - 1);
    const auto& row = s.rows[s.filtered[safe].row_index];
    if (s.preview_cache && s.preview_for_session_id == row.session_id) {
        return; // Already loaded
    }

    // Try custom loader first.
    if (s.opts.load_preview) {
        s.preview_cache = s.opts.load_preview(row.session_id, 10);
        s.preview_for_session_id = row.session_id;
        return;
    }
    // Fallback: read via store.
    if (!s.opts.store) {
        s.preview_cache = std::vector<Message>{};
        s.preview_for_session_id = row.session_id;
        return;
    }
    if (!s.opts.store->switch_conversation(row.session_id)) {
        s.preview_cache = std::vector<Message>{};
        s.preview_for_session_id = row.session_id;
        return;
    }
    auto* conv = s.opts.store->get_active_conversation();
    if (!conv) {
        s.preview_cache = std::vector<Message>{};
    } else {
        s.preview_cache = conv->get_recent_messages(10);
    }
    s.preview_for_session_id = row.session_id;
}

// ---------------------------------------------------------------------------
// Component factory
// ---------------------------------------------------------------------------

/// Build the full ResumeScreen FTXUI component.
[[nodiscard]] inline Component ResumeScreen(ResumeScreenOptions options) {
    auto state = std::make_shared<ResumeScreenState>();
    state->opts = std::move(options);

    // --- Populate rows once (single read from store) ---
    if (!state->opts.prefetched_rows.empty()) {
        state->rows = std::move(state->opts.prefetched_rows);
    } else if (state->opts.store) {
        state->rows = build_rows_from_store(
            *state->opts.store, state->opts.current_project_path);
    }
    refresh_filtered(*state);

    // Decide initial view.
    state->view = (state->rows.empty() || state->opts.force_welcome)
        ? ScreenView::WelcomeEmpty
        : ScreenView::SessionList;
    state->loading = false;

    auto emit = [state](const ResumeScreenAction& a) {
        if (state->opts.on_action) state->opts.on_action(a);
    };

    auto base = Renderer([state] {
        return RenderResumeScreen(
            state->rows, state->filtered,
            state->view,
            state->selected_idx,
            state->hovered_idx,
            state->query,
            state->search_focused,
            state->sort_mode,
            state->current_project_only,
            state->model_filter,
            state->preview_button,
            state->edit_draft,
            state->edit_has_error,
            state->preview_cache,
            state->loading);
    });

    // ---- Event handling ----
    return base | CatchEvent([state, emit](Event event) -> bool {
        const auto count = state->filtered.size();

        // ──────────────────────────────────────────────────────────
        // Shared: Escape — always backs out one level
        // ──────────────────────────────────────────────────────────
        if (event == Event::Escape) {
            switch (state->view) {
                case ScreenView::WelcomeEmpty:
                    emit({ResumeScreenAction::Kind::Cancel});
                    return true;
                case ScreenView::SessionList:
                    // Switch back to Welcome if available, else Cancel.
                    if (state->rows.empty()) {
                        emit({ResumeScreenAction::Kind::Cancel});
                    } else {
                        state->view = ScreenView::WelcomeEmpty;
                    }
                    return true;
                case ScreenView::DetailsPreview:
                    state->view = ScreenView::SessionList;
                    return true;
                case ScreenView::EditTitle:
                    state->view = ScreenView::SessionList;
                    state->edit_draft.clear();
                    state->edit_has_error = false;
                    return true;
                case ScreenView::DeleteConfirm:
                    state->view = ScreenView::SessionList;
                    return true;
            }
            return true;
        }

        // ──────────────────────────────────────────────────────────
        // WelcomeEmpty hotkeys
        // ──────────────────────────────────────────────────────────
        if (state->view == ScreenView::WelcomeEmpty) {
            if (event == Event::Return
                || event == Event::Character('n')
                || event == Event::Character('N'))
            {
                emit({ResumeScreenAction::Kind::NewConversation});
                return true;
            }
            if (event == Event::Character('i') || event == Event::Character('I')) {
                emit({ResumeScreenAction::Kind::ImportConversation});
                return true;
            }
            if (event == Event::Character('b') || event == Event::Character('B')
                || event == Event::ArrowDown || event == Event::Character('j'))
            {
                if (!state->rows.empty()) {
                    state->view = ScreenView::SessionList;
                    state->selected_idx = 0;
                    state->hovered_idx = 0;
                    return true;
                }
                return false;
            }
            // Quick pick: 1…5
            if (event.is_character()) {
                char c = event.character()[0];
                if (c >= '1' && c <= '9') {
                    const std::size_t k = static_cast<std::size_t>(c - '1');
                    auto recent = apply_filter_sort(
                        state->rows, "", SortMode::NewestFirst,
                        state->current_project_only, "");
                    if (k < recent.size()) {
                        // Translate → select that row & switch to list view.
                        // Find its position in `filtered`.
                        const auto row_idx = recent[k].row_index;
                        refresh_filtered(*state);
                        for (std::size_t i = 0; i < state->filtered.size(); ++i) {
                            if (state->filtered[i].row_index == row_idx) {
                                state->selected_idx = i;
                                state->hovered_idx = i;
                                break;
                            }
                        }
                        state->view = ScreenView::SessionList;
                        // Directly resume?  Spec says "quick pick = resume",
                        // so fire a QuickPickNumber action for the parent to
                        // decide and also mark the selection.
                        ResumeScreenAction a;
                        a.kind = ResumeScreenAction::Kind::QuickPickNumber;
                        a.index = k;
                        a.session_id = state->rows[row_idx].session_id;
                        emit(a);
                        return true;
                    }
                }
            }
            return false;
        }

        // ──────────────────────────────────────────────────────────
        // EditTitle dialog: type + Enter(save) / Esc(cancel) / Tab(reset)
        // ──────────────────────────────────────────────────────────
        if (state->view == ScreenView::EditTitle) {
            if (event == Event::Return) {
                if (state->edit_draft.empty()) {
                    state->edit_has_error = true;
                    return true;
                }
                if (count == 0) return true;
                const std::size_t safe = std::min(state->selected_idx, count - 1);
                auto& row = state->rows[state->filtered[safe].row_index];
                row.title = state->edit_draft;
                row.has_custom_title = true;
                if (state->opts.persist_title) {
                    state->opts.persist_title(row.session_id, row.title);
                }
                // Apply immediately to store if available.
                if (state->opts.store) {
                    if (state->opts.store->switch_conversation(row.session_id)) {
                        auto* conv = state->opts.store->get_active_conversation();
                        if (conv) conv->set_title(row.title);
                    }
                }
                ResumeScreenAction a;
                a.kind = ResumeScreenAction::Kind::RenameSession;
                a.session_id = row.session_id;
                a.extra = row.title;
                emit(a);
                state->edit_draft.clear();
                state->edit_has_error = false;
                state->view = ScreenView::SessionList;
                return true;
            }
            if (event == Event::Tab) {
                if (count > 0) {
                    const std::size_t safe = std::min(state->selected_idx, count - 1);
                    state->edit_draft =
                        state->rows[state->filtered[safe].row_index].title;
                    state->edit_has_error = false;
                }
                return true;
            }
            if (event == Event::Backspace) {
                if (!state->edit_draft.empty()) {
                    state->edit_draft.pop_back();
                    state->edit_has_error = false;
                }
                return true;
            }
            if (event.is_character() && event.character().size() == 1) {
                const char c = event.character()[0];
                // Reject control-ish characters.
                if (c >= 0x20 && c < 0x7f) {
                    state->edit_draft.push_back(c);
                    state->edit_has_error = false;
                    return true;
                }
            }
            return false;
        }

        // ──────────────────────────────────────────────────────────
        // DetailsPreview: button navigation + Enter / Esc
        // ──────────────────────────────────────────────────────────
        if (state->view == ScreenView::DetailsPreview) {
            if (event == Event::ArrowLeft || event == Event::Character('h')) {
                state->preview_button = std::max(0, state->preview_button - 1);
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                state->preview_button = std::min(2, state->preview_button + 1);
                return true;
            }
            if (event == Event::Return || event == Event::Character(' ')) {
                if (count == 0) return true;
                const std::size_t safe = std::min(state->selected_idx, count - 1);
                const auto& row = state->rows[state->filtered[safe].row_index];
                ResumeScreenAction a;
                a.session_id = row.session_id;
                switch (state->preview_button) {
                    case 0: a.kind = ResumeScreenAction::Kind::OpenSessionFull; break;
                    case 1: a.kind = ResumeScreenAction::Kind::ResumeSession;   break;
                    default:
                        // Cancel
                        state->view = ScreenView::SessionList;
                        return true;
                }
                emit(a);
                return true;
            }
            // Fall through — list navigation still works.
        }

        // ──────────────────────────────────────────────────────────
        // Search-mode: typing is captured only if search_focused
        // ──────────────────────────────────────────────────────────
        if (state->view == ScreenView::SessionList
            || state->view == ScreenView::DetailsPreview)
        {
            // Toggle search mode with '/'
            if (event == Event::Character('/')) {
                state->search_focused = true;
                state->query.clear();
                return true;
            }
            if (state->search_focused) {
                if (event == Event::Return) {
                    state->search_focused = false;
                    refresh_filtered(*state);
                    return true;
                }
                if (event == Event::Backspace) {
                    if (!state->query.empty()) {
                        state->query.pop_back();
                        refresh_filtered(*state);
                    }
                    return true;
                }
                if (event.is_character() && event.character().size() == 1) {
                    const char c = event.character()[0];
                    if (c >= 0x20 && c < 0x7f) {
                        state->query.push_back(c);
                        refresh_filtered(*state);
                        return true;
                    }
                }
                return false;
            }
        }

        // ──────────────────────────────────────────────────────────
        // SessionList + DetailsPreview: navigation + actions
        // ──────────────────────────────────────────────────────────
        {
            const bool list_view =
                (state->view == ScreenView::SessionList) ||
                (state->view == ScreenView::DetailsPreview);

            if (list_view) {
                // Up / Down
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    if (count > 0) {
                        if (state->selected_idx > 0) --state->selected_idx;
                        state->hovered_idx = state->selected_idx;
                    }
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    if (state->selected_idx + 1 < count) ++state->selected_idx;
                    state->hovered_idx = state->selected_idx;
                    return true;
                }
                // Shift-Up / Shift-Down → change hover only (no simple way in
                // FTXUI for shift + arrow; we use 'K'/'J' for hover-only.)
                if (event == Event::Character('K') && count > 0) {
                    if (state->hovered_idx > 0) --state->hovered_idx;
                    return true;
                }
                if (event == Event::Character('J') && count > 0) {
                    if (state->hovered_idx + 1 < count) ++state->hovered_idx;
                    return true;
                }
                // PageUp / PageDown (approximate: 10 steps)
                if (event == Event::PageUp) {
                    state->selected_idx = (state->selected_idx >= 10)
                        ? state->selected_idx - 10 : 0;
                    state->hovered_idx = state->selected_idx;
                    return true;
                }
                if (event == Event::PageDown) {
                    state->selected_idx = std::min(count - 1, state->selected_idx + 10);
                    state->hovered_idx = state->selected_idx;
                    return true;
                }
                // gg / G (we can't detect multi-key, so use 'g' = top, 'G' = end)
                if (event == Event::Character('g') && count > 0) {
                    state->selected_idx = 0;
                    state->hovered_idx = 0;
                    return true;
                }
                if (event == Event::Character('G') && count > 0) {
                    state->selected_idx = count - 1;
                    state->hovered_idx = count - 1;
                    return true;
                }
                // Home / End
                if (event == Event::Home && count > 0) {
                    state->selected_idx = 0;
                    state->hovered_idx = 0;
                    return true;
                }
                if (event == Event::End && count > 0) {
                    state->selected_idx = count - 1;
                    state->hovered_idx = count - 1;
                    return true;
                }

                // Toggles: sort, project filter, model filter, clear filters
                if (event == Event::Character('t') || event == Event::Character('T')) {
                    switch (state->sort_mode) {
                        case SortMode::NewestFirst: state->sort_mode = SortMode::OldestFirst; break;
                        case SortMode::OldestFirst: state->sort_mode = SortMode::TitleAsc;    break;
                        case SortMode::TitleAsc:    state->sort_mode = SortMode::ModelAsc;    break;
                        case SortMode::ModelAsc:    state->sort_mode = SortMode::NewestFirst; break;
                    }
                    refresh_filtered(*state);
                    return true;
                }
                if (event == Event::Character('p') || event == Event::Character('P')) {
                    state->current_project_only = !state->current_project_only;
                    refresh_filtered(*state);
                    return true;
                }
                if (event == Event::Character('m') || event == Event::Character('M')) {
                    // Cycle through common model names (or prompt).
                    // For simplicity we cycle: "" → first model we see → next.
                    // If no model data available, toggle is a no-op.
                    std::vector<std::string> seen = {""};
                    for (const auto& r : state->rows) {
                        if (r.model_name && !r.model_name->empty()) {
                            bool dup = false;
                            for (const auto& s : seen) if (s == *r.model_name) { dup = true; break; }
                            if (!dup) seen.push_back(*r.model_name);
                        }
                    }
                    if (seen.size() > 1) {
                        auto it = std::find(seen.begin(), seen.end(), state->model_filter);
                        std::size_t p = (it == seen.end()) ? 0 : (it - seen.begin());
                        p = (p + 1) % seen.size();
                        state->model_filter = seen[p];
                        refresh_filtered(*state);
                    }
                    return true;
                }
                if (event == Event::Character('c') || event == Event::Character('C')) {
                    state->query.clear();
                    state->model_filter.clear();
                    state->current_project_only = true;
                    state->sort_mode = SortMode::NewestFirst;
                    refresh_filtered(*state);
                    return true;
                }

                // Enter = resume / open details
                if (event == Event::Return) {
                    if (count == 0) return true;
                    if (state->view == ScreenView::DetailsPreview) {
                        // Already handled above; should not reach here.
                        return true;
                    }
                    // Open details preview + load lazy preview.
                    state->view = ScreenView::DetailsPreview;
                    state->preview_button = 1;  // Resume by default
                    ensure_preview_loaded(*state);
                    return true;
                }
                // Space = quick resume (skips preview)
                if (event == Event::Character(' ')) {
                    if (count == 0) return true;
                    const std::size_t safe = std::min(state->selected_idx, count - 1);
                    const auto& row = state->rows[state->filtered[safe].row_index];
                    ResumeScreenAction a;
                    a.kind = ResumeScreenAction::Kind::ResumeSession;
                    a.session_id = row.session_id;
                    emit(a);
                    return true;
                }

                // E = Edit title
                if (event == Event::Character('e') || event == Event::Character('E')) {
                    if (count == 0) return true;
                    const std::size_t safe = std::min(state->selected_idx, count - 1);
                    state->edit_draft =
                        state->rows[state->filtered[safe].row_index].title;
                    state->edit_has_error = false;
                    state->view = ScreenView::EditTitle;
                    return true;
                }
                // Delete / Backspace = open delete-confirm dialog
                if (event == Event::Delete || event == Event::Character('d')
                    || event == Event::Character('D'))
                {
                    if (count == 0) return true;
                    const std::size_t safe = std::min(state->selected_idx, count - 1);
                    const auto& row = state->rows[state->filtered[safe].row_index];

                    state->view = ScreenView::DeleteConfirm;

                    // Build and emit a delete-confirmation request so the
                    // REPL shell can overlay the UI8 TrustDialog component.
                    // The shell calls back through persist_delete / on_action
                    // with the TrustChoice.
                    //
                    // We hand back the TrustDialogProps via the `extra`
                    // field of a synthetic action; the shell must detect
                    // Kind::DeleteSession and invoke
                    // MakeTrustDialogComponent(MakeDeleteSessionTrustProps(...))
                    // itself.  (This avoids a circular dependency with the
                    // Component container here.)
                    ResumeScreenAction a;
                    a.kind = ResumeScreenAction::Kind::DeleteSession;
                    a.session_id = row.session_id;
                    a.extra = row.title;
                    emit(a);
                    return true;
                }
                // S = Share (placeholder)
                if (event == Event::Character('s') || event == Event::Character('S')) {
                    if (count == 0) return true;
                    const std::size_t safe = std::min(state->selected_idx, count - 1);
                    const auto& row = state->rows[state->filtered[safe].row_index];
                    ResumeScreenAction a;
                    a.kind = ResumeScreenAction::Kind::ShareSession;
                    a.session_id = row.session_id;
                    emit(a);
                    return true;
                }

                // N = new conversation (also works from list view)
                if (event == Event::Character('n') || event == Event::Character('N')) {
                    emit({ResumeScreenAction::Kind::NewConversation});
                    return true;
                }
                // W = go back to welcome screen
                if (event == Event::Character('w') || event == Event::Character('W')) {
                    state->view = ScreenView::WelcomeEmpty;
                    return true;
                }
            }
        }

        return false;
    });
}

// ===========================================================================
// Utility: create a medium-risk trust dialog component for deleting the
// currently-selected row.  (Convenience wrapper so callers don't need to
// re-read the row from storage a second time.)
// ===========================================================================
[[nodiscard]] inline Component MakeDeleteSessionTrustDialog(
    const SessionMetaRow& row,
    std::function<void(TrustChoice result)> on_done)
{
    return MakeTrustDialogComponent(MakeDeleteSessionTrustProps(row, std::move(on_done)));
}

} // namespace cc::ui::resume_screen
