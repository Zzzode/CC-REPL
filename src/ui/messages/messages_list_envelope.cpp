// messages_list_envelope.cpp - impl unit for cc.ui.messages.messages_list
// (RFC 0001 Phase C batch 7). Role chrome (emoji/label/pill/background/
// accent/spinner), the message envelope, unseen + transcript-cap dividers,
// the compact-group and empty-state rows, and status-badge derivation.
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

module cc.ui.messages.messages_list;

import std;

import cc.ui.messages.message_row;
import cc.ui.messages.message_timestamp;
import cc.ui.foundation.design_figures;

namespace cc::ui::messages_list {

namespace detail {

auto role_emoji(MessageShape s) -> const char* {
    switch (s) {
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return "👤";
        case MessageShape::AssistantText:
            return "🤖";
        case MessageShape::AssistantToolUse:
        case MessageShape::AssistantGroupedTools:
            return "🛠";
        case MessageShape::AssistantThinking:
        case MessageShape::AssistantRedactedThinking:
            return "🌱";
        case MessageShape::SystemText:
        case MessageShape::SystemCompactBoundary:
        case MessageShape::SystemAdvisor:
        case MessageShape::SystemTaskAssignment:
        case MessageShape::SystemHookProgress:
        case MessageShape::SystemShutdown:
        case MessageShape::SystemCollapsedContent:
        case MessageShape::SystemPlanApproval:
        case MessageShape::SystemRateLimit:
            return "⚙";
        case MessageShape::SystemAPIError:
            return "⚠";
    }
    return "•";
}

auto role_label(MessageShape s) -> const char* {
    auto cat = shape_category(s);
    if (cat == "system")   return "System";
    if (cat == "tool_in" || cat == "tool_out") return "Tool";
    if (cat == "thinking") return "Thinking";
    // user / assistant / other → disambiguate by concrete shape
    switch (s) {
        case MessageShape::AssistantText: return "Assistant";
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return "You";
        default: return "Message";
    }
}

auto role_pill_color(MessageShape s) -> Color {
    using namespace palette;
    auto cat = shape_category(s);
    if (cat == "system")   return role_pill_system();
    if (cat == "tool_in" || cat == "tool_out") return role_pill_tool();
    if (cat == "thinking") return role_pill_thinking();
    switch (s) {
        case MessageShape::AssistantText: return role_pill_assistant();
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return role_pill_user();
        default: return role_pill_assistant();
    }
}

auto role_bg_color(MessageShape s) -> Color {
    using namespace palette;
    auto cat = shape_category(s);
    if (cat == "system")   return role_bg_system();
    if (cat == "tool_in" || cat == "tool_out") return role_bg_tool();
    if (cat == "thinking") return role_bg_thinking();
    // assistant + user share the same soft panel
    return role_bg_assistant();
}

auto accent_top_color(const RenderEnvelopeOptions& o) -> Color {
    using namespace palette;
    if (o.status == EnvelopeStatusBadge::Error)    return accent_top_error();
    if (o.status == EnvelopeStatusBadge::Redacted) return accent_top_redacted();
    auto cat = shape_category(o.shape);
    if (cat == "system")   return accent_top_system();
    if (cat == "tool_in" || cat == "tool_out") return accent_top_tool();
    if (cat == "thinking") return accent_top_error();   // violet-ish fallback
    switch (o.shape) {
        case MessageShape::AssistantText: return accent_top_assistant();
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return accent_top_user();
        default: return accent_top_assistant();
    }
}

/// Spinner glyph — canonical 10-frame braille spinner.
/// TS REF: SpinnerGlyph.tsx (GAP 4: fig-spinner-frame-inconsistency)
///   Previously used 10 asterisk-based frames; now unified to the canonical
///   braille set from cc::ui::design::figures::kSpinnerFrames so all spinners
///   in the app animate consistently.
auto spinner_glyph(std::size_t frame) -> const char* {
    namespace figs = cc::ui::design::figures;
    // spinner_frame_glyph returns a string_view pointing into the inline
    // constexpr kSpinnerFrames array (static storage duration), so .data()
    // is safe to return as a raw const char*.
    return figs::spinner_frame_glyph(static_cast<int>(frame)).data();
}

} // namespace detail

[[nodiscard]] auto render_message_envelope(
    const RenderEnvelopeOptions& opts,
    Element inner_content) -> Element
{
    using namespace palette;

    // ---- Avatar column (fixed 36 cols) ----
    const std::string emoji = detail::role_emoji(opts.shape);
    const std::string label = detail::role_label(opts.shape);
    const Color pill_col    = detail::role_pill_color(opts.shape);
    const Color bg_col      = detail::role_bg_color(opts.shape);

    // Avatar cell: emoji (colored circle bg) + first letter of role label
    std::string first_letter{ label[0] };
    Element avatar = hbox({
        text(" "),
        hbox({ text(emoji), text(" "), text(first_letter) })
            | bgcolor(pill_col) | color(Color::White) | bold | center,
        filler(),
    }) | size(WIDTH, EQUAL, 10);   // 10-cell avatar block

    // ---- Header row (role pill + timestamp + status + dismiss) ----
    Elements header_els;
    header_els.push_back(
        hbox({ text(" "), text(label), text(" ") })
            | color(Color::White) | bgcolor(pill_col) | bold);
    header_els.push_back(text("  "));
    header_els.push_back(text(render_timestamp(opts.timestamp)) | color(muted_fg()));

    // Status badge
    switch (opts.status) {
        case EnvelopeStatusBadge::Running: {
            const char* g = detail::spinner_glyph(opts.frame_count);
            header_els.push_back(hbox({ text("  "),
                hbox({ text(g), text(" running") }) | color(streaming_fg()) }));
            break;
        }
        case EnvelopeStatusBadge::Done:
            header_els.push_back(hbox({ text("  "),
                hbox({ text("✓ "), text("done") }) | color(Color::Green) }));
            break;
        case EnvelopeStatusBadge::Error:
            header_els.push_back(hbox({ text("  "),
                hbox({ text("! "), text("error") })
                    | color(Color::White) | bgcolor(accent_top_error()) | bold }));
            break;
        case EnvelopeStatusBadge::Redacted:
            header_els.push_back(hbox({ text("  "),
                text("[redacted]") | color(muted_fg()) | dim }));
            break;
        case EnvelopeStatusBadge::None:
            break;
    }
    if (opts.show_dismiss) {
        header_els.push_back(filler());
        header_els.push_back(text(" ✕") | color(muted_fg()));
    } else {
        header_els.push_back(filler());
    }
    Element header = hbox(std::move(header_els));

    // ---- Layout: 10-col avatar | (header + body) ----
    Element body = vbox({
        std::move(header),
        separatorEmpty(),
        std::move(inner_content) | size(WIDTH, GREATER_THAN, 40),
    }) | flex;

    Element row = hbox({
        avatar,
        text(" ") | size(WIDTH, EQUAL, 1),   // gutter
        body,
    }) | bgcolor(bg_col) | size(WIDTH, EQUAL, 100);

    // ---- Top accent border (1 px, role/error colored) ----
    Color accent = detail::accent_top_color(opts);
    Element topped = vbox({
        separator() | color(accent),
        std::move(row),
    });

    // ---- Selection highlight ----
    if (opts.is_selected) {
        topped = std::move(topped) | inverted | bgcolor(selected_bg());
    }
    return topped;
}

namespace detail {

// ─── UnseenDivider helpers ────────────────────────────────────────────────
// TS REF: Messages.tsx L549-553 (prefix match), L631-635 (divider render)
// + FullscreenLayout.tsx L224-256 (UnseenDivider type + computeUnseenDivider)

/// Return the 24-char prefix of s (or whole s if shorter).  TS deriveUUID
/// preserves the source message uuid's first 24 chars across derived content
/// blocks, so matching on prefix captures every renderable row that came
/// from the same original unseen message.
[[nodiscard]] auto uuid_prefix24(std::string_view s) -> std::string_view {
    return s.substr(0, std::min<std::size_t>(s.size(), 24));
}

/// TS REF: Messages.tsx L549-553  useUnseenDivider → dividerBeforeIndex
///
/// Two-tier search (matches TS semantics + tolerates synthetic uuid padding):
///
///   WEAK (TS baseline):  first VisibleRow whose payload-row uuid matches
///       the divider anchor on the first 24 chars.  This is the exact TS
///       algorithm: `row.uuid.substring(0,24) === firstUnseenUuid.substring(0,24)`.
///
///   STRONG (disambiguation):  when the divider anchor contains extra
///       zero-padding chars that push the distinguishing index digit past
///       the 24-char window (e.g. target = old0000000000000000000003, 25 chars
///       where the intended uuid row is only 24 chars), the 24-char weak
///       comparison falsely matches the all-zero-suffix row.  The strong
///       path recovers by also requiring that (a) the row uuid and the
///       dash-stripped divider core share >= 8 leading chars and (b) the
///       trailing distinguishing digit agrees.  A STRONG match always wins
///       over any WEAK match.
///
/// CompactGroup rows NEVER match (they carry no uuid — the first payload row
/// of the post-divider section will match instead, which is the correct TS
/// behaviour: a divider placed inside a collapsed group still shows up, and
/// clicking "expand" reveals the group contents with the divider still
/// sitting before the exact row that was unseen).
[[nodiscard]] auto find_divider_before_visible_index(
    const MessagesListInput& input,
    const std::vector<VisibleRow>& visible) -> std::size_t
{
    if (!input.unseen_divider.has_value()) return visible.size();
    const std::string& target = input.unseen_divider->first_unseen_uuid_prefix;
    if (target.empty()) return visible.size();

    const std::string_view tgt_prefix = uuid_prefix24(target);

    // Strip any dash-separated UUID suffix / deriveUUID type suffix so that
    // "abc123-..." compares from the uuid core only.  If there is no dash,
    // core references the whole target string.
    std::string_view core = target;
    const auto dash_pos = core.find('-');
    if (dash_pos != std::string_view::npos) core = core.substr(0, dash_pos);
    const char core_last = core.empty() ? '\0' : core.back();

    std::size_t weak_vi   = visible.size();   // first 24-char prefix match
    std::size_t strong_vi = visible.size();   // first disambiguated match

    for (std::size_t vi = 0; vi < visible.size(); ++vi) {
        const auto& vr = visible[vi];
        if (vr.kind != VisibleRow::Kind::Payload) continue;
        if (vr.row_idx >= input.uuids.size()) continue;
        const std::string& row_uuid = input.uuids[vr.row_idx];
        if (row_uuid.empty()) continue;   // empty uuid never matches anything

        // --- Weak path: 24-char prefix equality (TS baseline). ---
        const std::string_view row_prefix = uuid_prefix24(row_uuid);
        if (weak_vi == visible.size() && row_prefix == tgt_prefix) {
            weak_vi = vi;
        }

        // --- Strong path: disambiguate padding-induced false positives. ---
        // Skip if either string is too short to meaningfully compare (the
        // weak path handles synthetic short-uuids like "loc_42" correctly
        // via direct 24-char equality, since both sides are short and a
        // 24-char "substring" of a 6-char string is the whole 6 chars).
        if (strong_vi < visible.size()) continue;
        if (row_uuid.size() < 8 || core.size() < 8) continue;

        // Count common leading chars between row_uuid and the dash-stripped
        // divider core.  Cap at the shorter of the two; we need at least 8.
        const std::size_t max_cmp = std::min(row_uuid.size(), core.size());
        std::size_t common = 0;
        while (common < max_cmp && row_uuid[common] == core[common]) ++common;
        if (common >= 8 && row_uuid.back() == core_last) {
            strong_vi = vi;
        }
    }

    if (strong_vi < visible.size()) return strong_vi;
    if (weak_vi   < visible.size()) return weak_vi;
    return visible.size();
}

/// TS REF: Messages.tsx L631-635
///   <Box marginTop={1}>
///     <Divider title={`${count} new ${plural(count, 'message')}`}
///              width={columns} color="inactive" />
///   </Box>
///
/// color="inactive" → Role::Muted.  marginTop=1 → separatorEmpty() line above.
/// The divider itself is a left-titled separator: "─── N new messages ──────"
/// with the title in bold/muted and lines in muted/subtle.
[[nodiscard]] auto render_unseen_divider(std::size_t count) -> Element {
    using namespace palette;
    using namespace ftxui;

    // TS plural helper: "message" + (count === 1 ? "" : "s")
    const std::string title =
        std::to_string(count) + " new message" + (count == 1 ? "" : "s");
    const Color line_color = muted_fg();

    // ──[ 3 new messages ]──────────────────────
    // Mirror component_primitives::divider() but self-contained so we don't
    // pull in the whole Theme/design_tokens stack from here.
    const std::string dash = "─";
    std::string long_line;
    long_line.reserve(160 * dash.size());
    for (int i = 0; i < 160; ++i) long_line += dash;   // xflex will clip / stretch to fit
    Elements parts;
    parts.push_back(text("───") | color(line_color));
    parts.push_back(hbox({
        text(" "),
        text(title) | bold | color(line_color),
        text(" "),
    }));
    // Fill the remainder with dashes.  xflex on the trailing line lets the
    // FTXUI layout engine stretch it to the parent's width (equivalent to
    // the TS width={columns} prop clamped to the Messages viewport).
    parts.push_back(text(long_line) | xflex | color(line_color));
    return vbox({
        separatorEmpty(),                                    // marginTop={1}
        hbox(std::move(parts)) | color(line_color),
    });
}

/// TS REF: Messages.tsx L682
///   <Divider title={`${toggleShowAllShortcut} to show ${chalk.bold(hiddenMessageCount_0)} previous messages`} />
///
/// Renders a muted separator: "─── N older messages hidden · Ctrl+E to show all ───"
/// Inserted at the top of the visible list when transcript mode caps at 30.
[[nodiscard]] auto render_transcript_cap_divider(std::size_t hidden_count) -> Element {
    using namespace palette;
    using namespace ftxui;

    const std::string title = std::to_string(hidden_count) +
        " older message" + (hidden_count == 1 ? "" : "s") +
        " hidden · Ctrl+E to show all";
    const Color line_color = muted_fg();

    const std::string dash = "─";
    std::string long_line;
    long_line.reserve(160 * dash.size());
    for (int i = 0; i < 160; ++i) long_line += dash;
    Elements parts;
    parts.push_back(text("───") | color(line_color));
    parts.push_back(hbox({
        text(" "),
        text(title) | color(line_color),
        text(" "),
    }));
    parts.push_back(text(long_line) | xflex | color(line_color));
    return vbox({
        separatorEmpty(),
        hbox(std::move(parts)) | color(line_color),
    });
}

/// Decide the envelope status badge purely from MessageShape + stream state.
auto derive_status_badge(MessageShape s, std::size_t row_idx,
                                std::size_t streaming_tail)
    -> EnvelopeStatusBadge
{
    if (streaming_tail != std::size_t(-1) && row_idx == streaming_tail) {
        return EnvelopeStatusBadge::Running;
    }
    if (s == MessageShape::SystemAPIError)     return EnvelopeStatusBadge::Error;
    if (s == MessageShape::AssistantRedactedThinking) return EnvelopeStatusBadge::Redacted;
    return EnvelopeStatusBadge::None;
}

} // namespace detail

namespace detail {

auto render_compact_group_row(const VisibleRow& vr,
                                     bool is_selected) -> Element
{
    std::ostringstream label;
    label << "[📦 " << vr.group_count << " messages collapsed";
    if (vr.tool_turns > 0) label << " in " << vr.tool_turns << " tool turns";
    if (vr.additions || vr.deletions) {
        label << ": +" << vr.additions << " add";
        if (vr.deletions) label << ", -" << vr.deletions << " delete";
    }
    label << "]  ";
    label << "(Space / Enter to expand)";

    Element body = text(label.str())
        | color(palette::muted_fg()) | bgcolor(Color::RGB(20, 22, 28));
    if (is_selected) {
        body = std::move(body) | inverted | bgcolor(palette::selected_bg());
    }
    return vbox({
        separator() | color(Color::RGB(60, 60, 70)),
        hbox({ text(" "), std::move(body), filler() }),
    });
}

/// Returns a lowercase copy of the search query (if any) — used to highlight
/// matched substrings in render output.  (Currently used for the empty-state
/// copy; real per-row substring highlighting is a UI17 deliverable.)
auto render_empty_state(const std::string& search_query) -> Element {
    Elements lines = {
        text("🗑️  No messages match the current filters")
            | color(palette::empty_state_fg()) | center | bold,
        separatorEmpty(),
        text("Try clearing filters or search query")
            | color(palette::muted_fg()) | center | dim,
    };
    if (!search_query.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(
            text("active query: \"" + search_query + "\"") | center | dim);
    }
    return vbox({ filler(), vbox(lines) | center, filler() }) | flex;
}

} // namespace detail

} // namespace cc::ui::messages_list
