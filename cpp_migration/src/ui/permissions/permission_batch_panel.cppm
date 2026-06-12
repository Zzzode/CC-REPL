/// @file permission_batch_panel.cppm
/// @brief Batch permission processing panel.
///
/// Handles the case where N pending permission requests are queued.
/// Layout:
///   Top bar : progress indicator ("3/24 approved (12%)") + toolbar
///   Body    : split horizontally
///             Left  : scrollable list of pending requests
///                     (tool icon + name + action + path preview + status dot)
///             Right : embedded single-prompt detail (no footer)
///   Bottom  : navigation row  [Skip]  [Deny & next]  [Approve & next]
///
/// Toolbar actions:
///   Approve all Low  /  Deny all High  /  Approve same-tool N times  /  Clear queue
///
/// Reuses permission_single_prompt for the detail panel, but strips the
/// footer buttons since decisions are driven from the batch toolbar/nav.
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.batch_panel;

import cc.utils.permissions_engine;
import cc.ui.permissions.components;
import cc.ui.permissions.single_prompt;

export namespace cc::ui::permissions::batch_panel {
using namespace ftxui;
namespace pc = cc::ui::permissions::components;
namespace sp = cc::ui::permissions::single_prompt;
namespace eng = cc::utils::permissions;

using Decision  = sp::Decision;
using RiskLevel = pc::RiskLevel;
using ActionKind = pc::ActionKind;
using ItemStatus = pc::ItemStatus;

// ============================================================
// Types
// ============================================================

/// State of a single request in the batch queue.
enum class RequestDecision : std::uint8_t {
    Pending,       // not yet decided
    ApprovedOnce,  // Allow once
    ApprovedAlways,// Always allow
    Denied,        // Deny this time
    DeniedAlways,  // Always deny
    Skipped,       // Skipped by user (still pending, will be revisited)
};

/// A queued request — reuses the single-prompt props for detail rendering.
struct QueuedRequest {
    std::size_t id = 0;
    sp::SinglePromptProps prompt_props;   // full detail (reused)
    RequestDecision decision = RequestDecision::Pending;
};

/// Summary of batch decisions (returned on completion).
struct BatchSummary {
    std::size_t total_requests = 0;
    std::size_t approved_once = 0;
    std::size_t approved_always = 0;
    std::size_t denied = 0;
    std::size_t denied_always = 0;
    std::size_t skipped = 0;
};

/// Per-request decision callback and overall completion callback.
struct BatchPanelCallbacks {
    /// Called for each individual decision (id + decision).
    std::function<void(std::size_t request_id, Decision d, bool sandbox)> on_each;
    /// Called when the user confirms the batch (or presses Escape).
    std::function<void(BatchSummary)> on_complete;
};

// ============================================================
// Helpers
// ============================================================

namespace detail {

[[nodiscard]] inline ItemStatus ToItemStatus(RequestDecision d) {
    switch (d) {
        case RequestDecision::Pending:
        case RequestDecision::Skipped:
            return ItemStatus::Pending;
        case RequestDecision::ApprovedOnce:
        case RequestDecision::ApprovedAlways:
            return ItemStatus::Approved;
        case RequestDecision::Denied:
        case RequestDecision::DeniedAlways:
            return ItemStatus::Denied;
    }
    return ItemStatus::Pending;
}

/// Count how many requests have been finalised (not pending or skipped).
[[nodiscard]] inline std::size_t CountFinalised(const std::vector<QueuedRequest>& q) {
    return std::count_if(q.begin(), q.end(), [](const QueuedRequest& r) {
        return r.decision != RequestDecision::Pending
            && r.decision != RequestDecision::Skipped;
    });
}

[[nodiscard]] inline BatchSummary BuildSummary(const std::vector<QueuedRequest>& q) {
    BatchSummary s;
    s.total_requests = q.size();
    for (const auto& r : q) {
        switch (r.decision) {
            case RequestDecision::Pending:                           break;
            case RequestDecision::ApprovedOnce:  ++s.approved_once;  break;
            case RequestDecision::ApprovedAlways:++s.approved_always;break;
            case RequestDecision::Denied:        ++s.denied;         break;
            case RequestDecision::DeniedAlways:  ++s.denied_always;  break;
            case RequestDecision::Skipped:       ++s.skipped;        break;
        }
    }
    return s;
}

/// Render a compact 1-line summary of a request (for the left-side list).
[[nodiscard]] inline Element RenderListItem(
    const QueuedRequest& req,
    bool is_selected,
    std::size_t list_max_len = 40)
{
    const auto& p = req.prompt_props;

    // Build a compact path preview (or command preview)
    std::string preview;
    std::visit([&](const auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, sp::DetailBash>) {
            preview = pc::HeadEllipsisCommand(d.command, list_max_len);
        } else if constexpr (std::is_same_v<T, sp::DetailFileEdit> ||
                             std::is_same_v<T, sp::DetailFileWrite> ||
                             std::is_same_v<T, sp::DetailFileRead>) {
            preview = pc::MiddleEllipsisPath(d.file_path, list_max_len);
        } else if constexpr (std::is_same_v<T, sp::DetailNetwork>) {
            preview = pc::MiddleEllipsisPath(d.url_or_domain, list_max_len);
        } else if constexpr (std::is_same_v<T, sp::DetailMCP>) {
            preview = d.server_id + "::" + d.tool_name;
            if (preview.size() > list_max_len) preview.resize(list_max_len - 3), preview += "…";
        } else if constexpr (std::is_same_v<T, sp::DetailSkill>) {
            preview = d.skill_name;
        } else if constexpr (std::is_same_v<T, sp::DetailPlan>) {
            preview = d.is_entry ? "enter plan mode" : "exit plan mode";
        } else if constexpr (std::is_same_v<T, sp::DetailAskUser>) {
            preview = pc::HeadEllipsisCommand(d.question, list_max_len);
        } else if constexpr (std::is_same_v<T, sp::DetailGeneric>) {
            preview = pc::HeadEllipsisCommand(d.description, list_max_len);
        }
    }, p.detail);

    auto icon = pc::ToolIcon(p.tool_name);
    auto status = pc::StatusDot(ToItemStatus(req.decision));
    auto name_el = text(p.tool_name) | color(pc::ActionColor(p.action_kind)) | bold;
    auto preview_el = text(" " + preview) | dim;

    auto row = hbox({
        status,
        icon,
        text(" "),
        name_el,
        text("  "),
        preview_el | xflex_grow,
        pc::RiskPill(p.risk_level),
    });

    if (is_selected) {
        row = row | inverted | focus;
    }
    return row;
}

} // namespace detail

// ============================================================
// State & component
// ============================================================

struct BatchState {
    std::vector<QueuedRequest> queue;
    std::size_t cursor = 0;          // currently selected list item
    bool sandbox_toggle = false;
    int  toolbar_cursor = -1;        // -1 = no toolbar focus, 0..3 toolbar buttons
    BatchPanelCallbacks cbs;
    std::vector<std::size_t> detail_viewport;   // for future scrolling
};

// ---- Progress bar element ----

[[nodiscard]] inline Element RenderProgressBar(const BatchState& st) {
    const auto total = st.queue.size();
    const auto done  = detail::CountFinalised(st.queue);
    const double pct = total == 0 ? 0.0 : static_cast<double>(done) / total;
    auto summary = detail::BuildSummary(st.queue);

    auto gauge_el = gauge(pct) | color(Color::Cyan) | size(HEIGHT, EQUAL, 1);
    auto label_el = text(std::format(" {}/{}", done, total))
                    | color(Color::White);
    auto pct_el = text(std::format(" {:3.0f}%", pct * 100))
                  | color(Color::GrayLight);
    auto detail_el = text(std::format("  ✓{} ✓a{} ✗{} ✗a{} ⏭{}",
        summary.approved_once, summary.approved_always,
        summary.denied, summary.denied_always, summary.skipped))
        | dim;
    return hbox({
        gauge_el | xflex_grow,
        label_el,
        pct_el,
        detail_el,
    });
}

// ---- Toolbar: 4 action buttons ----

[[nodiscard]] inline Element RenderToolbar(BatchState& st) {
    struct Btn { std::string label; Color col; };
    std::array<Btn, 4> btns = {{
        { "Approve all Low",    Color::Green  },
        { "Deny all High+",     Color::Red    },
        { "Same-tool approve",  Color::Cyan   },
        { "Clear queue",        Color::Yellow },
    }};
    Elements els;
    for (int i = 0; i < 4; ++i) {
        const auto& b = btns[i];
        bool focused = st.toolbar_cursor == i;
        auto el = hbox({ text(" "), text(b.label), text(" ") })
                  | borderStyled(b.col) | color(b.col);
        if (focused) el = el | inverted | bold;
        els.push_back(el);
        if (i < 3) els.push_back(text("  "));
    }
    return hbox(els);
}

// ---- Left: request list ----

[[nodiscard]] inline Element RenderRequestList(BatchState& st) {
    Elements items;
    if (st.queue.empty()) {
        items.push_back(text(" (queue empty)") | dim);
    } else {
        // Simple windowing: show up to 12 items around the cursor
        const std::size_t kShow = 12;
        std::size_t start = 0;
        if (st.cursor >= kShow) start = st.cursor - kShow / 2;
        const std::size_t end = std::min(st.queue.size(), start + kShow);
        for (std::size_t i = start; i < end; ++i) {
            items.push_back(detail::RenderListItem(
                st.queue[i], i == st.cursor, 40));
        }
        if (start > 0) {
            items.insert(items.begin(),
                text(std::format(" ↑ {} above", start)) | dim | color(Color::GrayDark));
        }
        if (end < st.queue.size()) {
            items.push_back(
                text(std::format(" ↓ {} below", st.queue.size() - end))
                | dim | color(Color::GrayDark));
        }
    }
    auto title = hbox({
        text("Queue") | bold,
        text(std::format(" ({})", st.queue.size())) | dim,
    });
    return vbox({
        title,
        pc::ThinDivider(),
        vbox(items) | yframe | size(HEIGHT, GREATER_THAN, 10) | xflex_grow,
    });
}

// ---- Right: detail panel (single-prompt without footer) ----

[[nodiscard]] inline Element RenderDetailPanel(BatchState& st) {
    Elements content;
    if (st.cursor >= st.queue.size()) {
        content.push_back(text("Select a request from the list.") | dim);
    } else {
        const auto& p = st.queue[st.cursor].prompt_props;

        // Build the same header/body as single prompt but omit footer.
        auto title_bar = hbox({
            text(std::string{pc::ToolIconGlyph(p.tool_name)}) | bold,
            text(" "),
            text(p.tool_name) | bold | color(pc::ActionColor(p.action_kind)),
            filler(),
            pc::RiskPill(p.risk_level),
        });

        content.push_back(title_bar);
        content.push_back(pc::ThinDivider());
        content.push_back(text(""));
        content.push_back(paragraph(p.description) | color(Color::White));
        content.push_back(text(""));

        auto detail_el = std::visit([](const auto& d) -> Element {
            using T = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<T, sp::DetailBash>) {
                Elements els = { pc::CommandPreview(d.command, 80) };
                if (d.working_dir) {
                    els.push_back(hbox({ text(" in: ") | dim,
                        pc::PathLabel(*d.working_dir, 60) }));
                }
                if (d.is_destructive) {
                    els.insert(els.begin(),
                        pc::DestructiveWarningBanner(
                            d.destructive_reason.empty()
                                ? "This command may permanently delete files."
                                : d.destructive_reason));
                }
                return vbox(els);
            } else if constexpr (std::is_same_v<T, sp::DetailFileEdit>) {
                return vbox({
                    hbox({ text("File: ") | dim,
                           pc::PathLabel(d.file_path, 70) }),
                    text(""),
                    hbox({ text("- ") | color(Color::Red),
                           text(pc::HeadEllipsisCommand(d.old_snippet, 78))
                           | color(Color::Red) }),
                    hbox({ text("+ ") | color(Color::Green),
                           text(pc::HeadEllipsisCommand(d.new_snippet, 78))
                           | color(Color::Green) }),
                });
            } else if constexpr (std::is_same_v<T, sp::DetailFileWrite>) {
                return vbox({
                    hbox({ text("File: ") | dim,
                           pc::PathLabel(d.file_path, 70) }),
                    hbox({ text("Size: ") | dim,
                           text(std::format("{} bytes (~{} lines)",
                               d.content_bytes, d.content_lines)) | dim }),
                });
            } else if constexpr (std::is_same_v<T, sp::DetailFileRead>) {
                return hbox({ text("File: ") | dim,
                              pc::PathLabel(d.file_path, 70) });
            } else if constexpr (std::is_same_v<T, sp::DetailNetwork>) {
                auto tag = d.is_upload
                    ? hbox({ text(" UP ") | color(Color::White)
                                         | bgcolor(Color::Red) | bold })
                    : hbox({ text(" ↓ ") | color(Color::White)
                                         | bgcolor(Color::Blue) | dim });
                return hbox({ tag, text("  "),
                              pc::PathLabel(d.url_or_domain, 70) });
            } else if constexpr (std::is_same_v<T, sp::DetailMCP>) {
                return vbox({
                    hbox({ text("MCP: ") | dim,
                           text(d.server_id) | color(Color::Purple) }),
                    hbox({ text("Tool: ") | dim,
                           text(d.tool_name) | bold }),
                });
            } else if constexpr (std::is_same_v<T, sp::DetailSkill>) {
                return hbox({ text("Skill: ") | dim,
                              text(d.skill_name)
                              | color(Color::Magenta) | bold });
            } else {
                return paragraph("(tool detail)");
            }
        }, p.detail);
        content.push_back(detail_el);

        if (!p.affected_paths.empty()) {
            content.push_back(text(""));
            content.push_back(text(" Affected paths") | bold | dim);
            auto paths = pc::SummarisePathList(p.affected_paths, 6, 60);
            for (auto& el : paths) el = hbox({ text("  "), el });
            content.insert(content.end(), paths.begin(), paths.end());
        }

        // Decision status line at the bottom of the detail panel
        const auto& cur = st.queue[st.cursor];
        auto status_line = [&]() -> Element {
            switch (cur.decision) {
                case RequestDecision::Pending:
                    return text(" Status: PENDING") | color(Color::Yellow);
                case RequestDecision::ApprovedOnce:
                    return text(" Status: ✓ Allow once") | color(Color::Green);
                case RequestDecision::ApprovedAlways:
                    return text(" Status: ✓ Always allow") | color(Color::Green) | bold;
                case RequestDecision::Denied:
                    return text(" Status: ✗ Deny") | color(Color::Red);
                case RequestDecision::DeniedAlways:
                    return text(" Status: ✗ Always deny") | color(Color::Red) | bold;
                case RequestDecision::Skipped:
                    return text(" Status: ⏭ Skipped") | dim;
            }
            return text("");
        }();
        content.push_back(text(""));
        content.push_back(status_line);
    }

    auto title = hbox({ text("Detail") | bold });
    return vbox({
        title,
        pc::ThinDivider(),
        vbox(content) | xflex_grow,
    }) | size(WIDTH, GREATER_THAN, 50);
}

// ---- Bottom navigation bar ----

[[nodiscard]] inline Element RenderBottomNav(const BatchState& /*st*/) {
    auto skip   = hbox({ text(" Skip ") })   | borderStyled(Color::GrayDark) | dim;
    auto deny   = hbox({ text(" ✗ Deny & next ") }) | borderStyled(Color::Red) | color(Color::Red);
    auto allow  = hbox({ text(" ✓ Approve & next ") }) | borderStyled(Color::Green) | color(Color::Green);
    auto done   = hbox({ text(" Done ✅ ") }) | borderStyled(Color::Cyan) | color(Color::Cyan) | bold;
    return hbox({
        skip, text("   "), deny, text("   "), allow, filler(), done,
    });
}

// ============================================================
// Main element
// ============================================================

[[nodiscard]] inline Element RenderBatchPanel(std::shared_ptr<BatchState> st) {
    auto prog  = RenderProgressBar(*st);
    auto tbar  = RenderToolbar(*st);
    auto list  = RenderRequestList(*st);
    auto det   = RenderDetailPanel(*st);
    auto bot   = RenderBottomNav(*st);

    auto top = vbox({
        prog,
        text(""),
        tbar,
    });

    auto body = hbox({
        list | size(WIDTH, GREATER_THAN, 38) | xflex_grow,
        separator(),
        det | xflex_grow | size(WIDTH, GREATER_THAN, 48),
    });

    auto full = vbox({
        top,
        pc::ThinDivider(),
        body | yflex_grow,
        pc::ThinDivider(),
        bot,
    });

    return window(
        text(" ⚡ Batch Permissions ") | bold | color(Color::Magenta),
        full | xflex_grow
    ) | color(Color::Magenta) | size(WIDTH, LESS_THAN, 120);
}

// ============================================================
// Event handling
// ============================================================

/// Apply a decision to the currently-cursor request and advance the cursor.
inline void ApplyDecisionAdvance(BatchState& st, RequestDecision decision) {
    if (st.cursor >= st.queue.size()) return;
    auto& req = st.queue[st.cursor];
    req.decision = decision;

    // Fire per-request callback
    if (st.cbs.on_each) {
        Decision d = Decision::AllowOnce;
        switch (decision) {
            case RequestDecision::ApprovedOnce:   d = Decision::AllowOnce;     break;
            case RequestDecision::ApprovedAlways: d = Decision::AlwaysAllow;   break;
            case RequestDecision::Denied:         d = Decision::Deny;          break;
            case RequestDecision::DeniedAlways:   d = Decision::AlwaysDeny;    break;
            case RequestDecision::Pending:
            case RequestDecision::Skipped:        d = Decision::Abort;         break;
        }
        st.cbs.on_each(req.id, d, st.sandbox_toggle);
    }

    // Advance cursor to next pending
    auto next = st.cursor;
    for (std::size_t off = 1; off <= st.queue.size(); ++off) {
        next = (st.cursor + off) % st.queue.size();
        if (st.queue[next].decision == RequestDecision::Pending) {
            st.cursor = next;
            return;
        }
    }
    // All done — don't move cursor (will hit Escape / Done button)
}

/// Apply a batch operation from the toolbar.
inline void ApplyToolbarAction(BatchState& st, int action_idx) {
    switch (action_idx) {
        case 0: { // Approve all Low
            for (auto& r : st.queue) {
                if (r.prompt_props.risk_level == RiskLevel::Low &&
                    r.decision == RequestDecision::Pending) {
                    r.decision = RequestDecision::ApprovedOnce;
                    if (st.cbs.on_each)
                        st.cbs.on_each(r.id, Decision::AllowOnce, st.sandbox_toggle);
                }
            }
            break;
        }
        case 1: { // Deny all High+
            for (auto& r : st.queue) {
                if ((r.prompt_props.risk_level == RiskLevel::High ||
                     r.prompt_props.risk_level == RiskLevel::Critical) &&
                    r.decision == RequestDecision::Pending) {
                    r.decision = RequestDecision::Denied;
                    if (st.cbs.on_each)
                        st.cbs.on_each(r.id, Decision::Deny, st.sandbox_toggle);
                }
            }
            break;
        }
        case 2: { // Approve same tool as current request
            if (st.cursor >= st.queue.size()) break;
            auto tool = st.queue[st.cursor].prompt_props.tool_name;
            std::size_t count = 0;
            for (auto& r : st.queue) {
                if (r.prompt_props.tool_name == tool &&
                    r.decision == RequestDecision::Pending) {
                    r.decision = RequestDecision::ApprovedOnce;
                    ++count;
                    if (st.cbs.on_each)
                        st.cbs.on_each(r.id, Decision::AllowOnce, st.sandbox_toggle);
                }
            }
            (void)count;
            break;
        }
        case 3: { // Clear queue (mark all Skipped)
            for (auto& r : st.queue) {
                if (r.decision == RequestDecision::Pending) {
                    r.decision = RequestDecision::Skipped;
                    if (st.cbs.on_each)
                        st.cbs.on_each(r.id, Decision::Abort, false);
                }
            }
            break;
        }
    }
}

// ============================================================
// Public factory
// ============================================================

/// Create a full interactive batch-permission panel.
[[nodiscard]] inline Component MakeBatchPanel(
    std::vector<QueuedRequest> queue,
    BatchPanelCallbacks callbacks)
{
    auto st = std::make_shared<BatchState>();
    st->queue = std::move(queue);
    st->cbs   = std::move(callbacks);
    // Move cursor to first pending
    for (std::size_t i = 0; i < st->queue.size(); ++i) {
        if (st->queue[i].decision == RequestDecision::Pending) {
            st->cursor = i; break;
        }
    }

    return Renderer([st] { return RenderBatchPanel(st); })
         | CatchEvent([st](Event event) -> bool {
        const std::size_t N = st->queue.size();

        // ---- List navigation ----
        if (st->toolbar_cursor < 0) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                if (N > 0) st->cursor = (st->cursor - 1 + N) % N;
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                if (N > 0) st->cursor = (st->cursor + 1) % N;
                return true;
            }
        }

        // ---- Switch focus between toolbar and list ----
        if (event == Event::Character('t') || event == Event::Tab) {
            if (st->toolbar_cursor < 0) st->toolbar_cursor = 0;
            else if (st->toolbar_cursor < 3) ++st->toolbar_cursor;
            else st->toolbar_cursor = -1;
            return true;
        }
        if (event == Event::TabReverse) {
            if (st->toolbar_cursor > 0) --st->toolbar_cursor;
            else if (st->toolbar_cursor == 0) st->toolbar_cursor = -1;
            else st->toolbar_cursor = 3;
            return true;
        }
        if (st->toolbar_cursor >= 0) {
            if (event == Event::ArrowLeft || event == Event::Character('h')) {
                st->toolbar_cursor = std::max(0, st->toolbar_cursor - 1);
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                st->toolbar_cursor = std::min(3, st->toolbar_cursor + 1);
                return true;
            }
            if (event == Event::Return) {
                ApplyToolbarAction(*st, st->toolbar_cursor);
                return true;
            }
        }

        // ---- Quick action keys (always work regardless of focus) ----
        if (event == Event::Character('y')) {
            ApplyDecisionAdvance(*st, RequestDecision::ApprovedOnce);
            return true;
        }
        if (event == Event::Character('a')) {
            ApplyDecisionAdvance(*st, RequestDecision::ApprovedAlways);
            return true;
        }
        if (event == Event::Character('n')) {
            ApplyDecisionAdvance(*st, RequestDecision::Denied);
            return true;
        }
        if (event == Event::Character('d')) {
            ApplyDecisionAdvance(*st, RequestDecision::DeniedAlways);
            return true;
        }
        if (event == Event::Character('s')) {
            if (st->cursor < N) {
                st->queue[st->cursor].decision = RequestDecision::Skipped;
                auto next = (st->cursor + 1) % std::max<std::size_t>(1, N);
                st->cursor = next;
            }
            return true;
        }

        // ---- Enter on list = allow once and advance ----
        if (event == Event::Return && st->toolbar_cursor < 0) {
            ApplyDecisionAdvance(*st, RequestDecision::ApprovedOnce);
            return true;
        }

        // ---- Escape / Q = complete batch ----
        if (event == Event::Escape || event == Event::Character('q')) {
            if (st->cbs.on_complete)
                st->cbs.on_complete(detail::BuildSummary(st->queue));
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::permissions::batch_panel
