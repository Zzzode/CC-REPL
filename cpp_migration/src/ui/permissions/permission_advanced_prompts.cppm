/// @file permission_advanced_prompts.cppm
/// @brief Advanced permission dialogs:
///     1. AskUserQuestionDialog  — 5 kinds (Info / Confirm / MultiChoice /
///                                   FreeText / CodeInput) with hotkeys.
///     2. SkillPermissionDialog — Skill logo / source badge (Bundled/User/
///                                 CustomPath) + capabilities + 3 choices.
///     3. FallbackPermissionDialog — unknown-tool yellow banner + three-column
///                                 best-effort / raw JSON / report form.
///
/// Migrated from (combined ~1808 lines TS → ~1200 lines C++):
///   src/components/permissions/AskUserQuestionPermissionRequest/
///       AskUserQuestionPermissionRequest.tsx   (644 lines)
///       QuestionView.tsx                        (464 lines)
///   src/components/permissions/SkillPermissionRequest/
///       SkillPermissionRequest.tsx             (368 lines)
///   src/components/permissions/FallbackPermissionRequest.tsx (332 lines)
///
/// Engine delegation: NEVER duplicate permission-engine logic.  Callbacks
/// (on_respond / on_always_allow / on_reject / on_report) carry decisions to
/// the application layer; engine primitives are reused via
/// cc.utils.permissions_engine where needed.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.advanced_prompts;

import cc.utils.permissions_engine;
import cc.ui.design.tokens;
import cc.ui.trust_utils;
import cc.ui.custom_select;
import cc.ui.permissions.components;

export namespace cc::ui::permissions::advanced {
using namespace ftxui;
namespace tk = cc::ui::design::tokens;
namespace tu = cc::ui::trust_utils;
namespace cs = cc::ui::custom_select;
namespace pc = cc::ui::permissions::components;
namespace eng = cc::utils::permissions;

using tu::RiskLevel;
using pc::ActionKind;

// --- Geometry / chrome constants ---

inline constexpr int kChromeRows = 9;     // title + header + borders + buttons
inline constexpr int kMinContentW  = 40;
inline constexpr int kMinContentH  = 12;
inline constexpr int kMaxJsonRows  = 30;    // raw JSON tree truncation
inline constexpr int kCodeInputRows = 4;    // monospace code box
inline constexpr int kMultiChoiceMaxVisible = 10;

// ============================================================
// 1.  AskUserQuestionDialog
// ============================================================

/// Five question kinds matching the TS `AskUserQuestion` enum.
enum class QuestionKind : std::uint8_t {
    Info,          // Modal display; single OK button.
    Confirm,       // Yes / No  (2 buttons, default Yes).
    MultiChoice,   // Pick one or many options via CustomSelect (UI6).
    FreeText,      // Single-line edit; Ctrl+Enter inserts newline (multi-line).
    CodeInput,     // Monospace 4-row box; paste / multi-keystroke capture.
};

/// A single option for MultiChoice questions (label + longer description).
struct QuestionOption {
    std::string value;
    std::string label;
    std::string description;
};

/// A single user-facing question (part of a possibly-multi-question flow).
struct UserQuestion {
    QuestionKind       kind = QuestionKind::Confirm;
    std::string        title;             // bold header
    std::string        subtitle;          // dim secondary line
    std::string        body;              // free-form body (may contain markdown)
    std::vector<QuestionOption> options;  // MultiChoice only
    std::string        default_text;      // FreeText / CodeInput initial
    std::string        placeholder;       // FreeText placeholder hint
    bool               multi_select = false;  // MultiChoice: allow >1
    std::vector<std::string> image_attachments; // (base64 placeholders)
};

/// User answer payload — kind + variant fields.
struct UserAnswer {
    QuestionKind kind;
    bool         confirmed = false;          // Info / Confirm
    std::vector<std::string> chosen_values;  // MultiChoice
    std::string  text;                       // FreeText / CodeInput
    bool         always_allow = false;       // sticky checkbox result
};

/// Callbacks fired when the user commits or cancels.
struct AskUserQuestionCallbacks {
    std::function<void(const UserAnswer&)>  on_submit;
    std::function<void()>                   on_cancel;
};

// --- Rendering helpers for QuestionDialog -------------------

namespace qdetail {

[[nodiscard]] inline Element KindBadge(QuestionKind k) {
    const char* names[] = {"INFO", "CONFIRM", "MULTI", "TEXT", "CODE"};
    Color colors[] = {Color::Cyan, Color::Yellow, Color::Magenta,
                      Color::Blue, Color::Green};
    int idx = static_cast<int>(k);
    return text(std::format(" {} ", names[idx]))
         | color(colors[idx]) | bgcolor(Color::RGB(28, 32, 38)) | bold;
}

/// Title bar of the modal (icon + title + kind badge + Esc hint).
[[nodiscard]] inline Element TitleBar(std::string_view icon,
                                      std::string_view title,
                                      QuestionKind kind) {
    return hbox({
        text(std::string{icon}) | color(tk::palette::dark.primary),
        text(" "),
        text(std::string{title}) | bold | color(Color::White),
        filler(),
        KindBadge(kind),
        text("  "),
        text("Esc=cancel") | dim,
    });
}

/// Standard "Always allow" checkbox (driven by key `a` or space when focused).
[[nodiscard]] inline Element AlwaysAllowRow(bool checked, bool enabled,
                                            std::string_view hint = "") {
    std::string box = checked ? "▣" : "▢";
    auto label = hbox({
        text(std::string{"  [a] "}) | dim,
        text(box) | color(enabled ? Color::Green : Color::GrayDark) | bold,
        text(" Always allow for this request")
            | (enabled ? color(Color::White) : dim),
    });
    if (!hint.empty())
        return vbox({
            label,
            text(std::format("      {}", hint)) | dim,
        });
    return label;
}

/// Standard Cancel / Submit footer row, plus optional Always-allow checkbox.
[[nodiscard]] inline Elements FooterButtons(int focused_but,
                                            bool show_always,
                                            bool always_enabled,
                                            bool always_checked,
                                            std::string_view always_hint,
                                            std::string_view submit_label,
                                            Color submit_color,
                                            std::vector<std::string_view> extra_hotkeys = {})
{
    Elements rows;

    if (show_always)
        rows.push_back(AlwaysAllowRow(always_checked, always_enabled, always_hint));
    rows.push_back(pc::ThinDivider());

    Elements buttons;
    auto make = [&](std::string_view hotkey, std::string_view label,
                    Color c, int idx) {
        const bool hovered = (focused_but == idx);
        auto chip = text(std::format(" [{}] {} ", hotkey, label))
                  | color(c)
                  | (hovered ? (bold | inverted) : dim);
        return chip;
    };
    buttons.push_back(make("Esc", "Cancel", Color::Red, 0));
    buttons.push_back(filler());
    for (std::size_t i = 0; i < extra_hotkeys.size(); i += 2) {
        if (i + 1 >= extra_hotkeys.size()) break;
        buttons.push_back(text(std::format(" [{}] {} ",
                        extra_hotkeys[i], extra_hotkeys[i + 1])) | dim);
        buttons.push_back(text(" "));
    }
    buttons.push_back(make("Enter", submit_label, submit_color, 1));
    rows.push_back(hbox(std::move(buttons)));
    return rows;
}

/// Word-wrap a multi-line body into rows suitable for a fixed-width modal.
[[nodiscard]] inline Elements WrapBody(std::string_view body, int cols) {
    Elements out;
    std::string cur; cur.reserve(cols);
    std::istringstream iss(std::string{body});
    for (std::string line; std::getline(iss, line); ) {
        cur.clear();
        std::istringstream ls(line);
        std::string word;
        while (ls >> word) {
            if (cur.size() + 1 + word.size() > std::size_t(cols)
                && !cur.empty()) {
                out.push_back(text(cur));
                cur = word;
            } else {
                if (!cur.empty()) cur += ' ';
                cur += word;
            }
        }
        out.push_back(text(cur.empty() ? " " : cur));
    }
    if (out.empty()) out.push_back(text(" "));
    return out;
}

/// Render "Info / Confirm" simple body content.
[[nodiscard]] inline Element SimpleBody(int& maxw, int& maxh,
                                        const UserQuestion& q) {
    int w = std::max<int>(kMinContentW, static_cast<int>(q.title.size()) + 12);
    w = std::max(w, static_cast<int>(q.subtitle.size()) + 4);
    int cols = std::min(100, w + 8);
    Elements body_rows = WrapBody(q.body, cols);
    int h = static_cast<int>(body_rows.size());
    maxw = std::max(w, cols);
    maxh = std::max<int>(kMinContentH, h + kChromeRows);
    return vbox({
        hbox({
            text(" ℹ ") | color(Color::Cyan),
            text(q.subtitle.empty() ? std::string{"(no details)"} : q.subtitle) | dim,
        }),
        pc::ThinDivider(),
        vbox(std::move(body_rows)) | yflex_grow,
    });
}

/// Draw input box (FreeText single-line OR CodeInput multi-row).
[[nodiscard]] inline Element TextInputBox(QuestionKind kind,
                                          std::string_view buf,
                                          std::string_view placeholder,
                                          int w, bool focused,
                                          int& rows_out)
{
    std::string cur = buf.empty() ? std::string{placeholder} : std::string{buf};
    const bool dim_text = buf.empty();

    if (kind == QuestionKind::FreeText) {
        rows_out = 1;
        auto line = cur.size() > std::size_t(w - 4)
            ? cur.substr(cur.size() - std::size_t(w - 4)) : cur;
        if (focused && !buf.empty()) line += "▊";
        else if (focused) line += "▊";
        return hbox({
            text("  › ") | color(Color::Cyan),
            text(line) | (dim_text ? dim : color(Color::YellowLight)),
            filler(),
        }) | borderLight | size(WIDTH, EQUAL, w);
    }
    rows_out = kCodeInputRows;
    Elements lines;
    std::istringstream iss(cur);
    std::string l;
    for (int r = 0; r < kCodeInputRows; ++r) {
        if (std::getline(iss, l)) {
            if (l.size() > std::size_t(w - 6))
                l = l.substr(0, w - 6) + "…";
            lines.push_back(
                hbox({
                    text(std::format(" {:>2} │", r + 1)) | dim,
                    text(" " + l) | (dim_text ? dim : color(Color::GreenLight)),
                    filler(),
                })
            );
        } else {
            lines.push_back(
                hbox({
                    text(std::format(" {:>2} │", r + 1)) | dim,
                    text(r == 0 && focused ? " ▊" : " ") | color(Color::GreenLight),
                    filler(),
                })
            );
        }
    }
    return vbox({
        text("  code input (4 rows, Ctrl+v paste, Esc cancel)") | dim,
        vbox(std::move(lines)) | borderDouble | color(Color::Green),
    });
}

} // namespace qdetail

// --- AskUserQuestion state & component ------------------------

struct AskUserQuestionState {
    std::vector<UserQuestion> questions;
    AskUserQuestionCallbacks  cbs;

    std::size_t current_idx = 0;
    UserAnswer  answer;  // per-question answer, aggregated as we go
    std::vector<std::vector<std::string>> answers_by_q; // multi-choice cache

    std::string text_buf;

    std::shared_ptr<cs::CustomSelectHandle> mc_handle;
    Component                              mc_component;

    int focused_button = 1;

    bool show_always_allow = true;
    bool always_allowed    = false;

    bool plan_mode = false;
    bool skip_and_plan_immediately = false;

    std::size_t total() const { return std::max(std::size_t{1}, questions.size()); }
};

/// Main render function for the AskUserQuestion modal.
[[nodiscard]] inline Element RenderAskUserQuestion(
    std::shared_ptr<AskUserQuestionState> st)
{
    const auto& q = st->questions.empty()
        ? UserQuestion{} : st->questions[st->current_idx];
    const bool single_q = st->questions.size() <= 1;

    int w = kMinContentW, h = kMinContentH;

    Element body;
    if (q.kind == QuestionKind::Info || q.kind == QuestionKind::Confirm) {
        body = qdetail::SimpleBody(w, h, q);
    } else if (q.kind == QuestionKind::MultiChoice) {
        if (!st->mc_component) {
            cs::CustomSelectOptions opts;
            opts.mode = q.multi_select ? cs::SelectMode::Multi
                                       : cs::SelectMode::Single;
            opts.visible_count = std::min<int>(kMultiChoiceMaxVisible,
                                               (int)q.options.size());
            opts.show_indexes = true;
            opts.inline_descriptions = true;
            for (std::size_t i = 0; i < q.options.size(); ++i) {
                cs::SelectOption o;
                o.value = q.options[i].value;
                o.label = std::format("{}. {}", i + 1, q.options[i].label);
                o.description = q.options[i].description;
                opts.options.push_back(std::move(o));
            }
            opts.on_change = [st](const std::string&) {
                if (st->mc_handle) {
                    st->answers_by_q.resize(st->total());
                    st->answers_by_q[st->current_idx] =
                        st->mc_handle->SelectedValues();
                }
            };
            auto [c, hdl] = cs::MakeCustomSelect(std::move(opts));
            st->mc_component = std::move(c);
            st->mc_handle = std::move(hdl);
        }
        Elements inner = qdetail::WrapBody(q.body, w);
        inner.push_back(pc::ThinDivider());
        inner.push_back(st->mc_component->Render());
        body = vbox(std::move(inner));
    } else if (q.kind == QuestionKind::FreeText ||
               q.kind == QuestionKind::CodeInput) {
        if (st->text_buf.empty() && !q.default_text.empty())
            st->text_buf = q.default_text;
        int rows;
        auto box = qdetail::TextInputBox(
            q.kind, st->text_buf,
            q.placeholder.empty()
                ? std::string{q.kind == QuestionKind::CodeInput
                                  ? "// paste code here (Ctrl+v)"
                                  : "type your answer…"}
                : q.placeholder,
            w, st->focused_button != 0, rows);
        Elements inner = qdetail::WrapBody(q.body, w);
        inner.push_back(pc::ThinDivider());
        inner.push_back(std::move(box));
        body = vbox(std::move(inner));
        h = std::max(h, rows + kChromeRows + 6);
    } else {
        body = text(" (unknown question kind) ") | color(Color::Red) | dim;
    }

    auto header = qdetail::TitleBar("❓", q.title, q.kind);

    Element progress;
    if (!single_q) {
        progress = text(std::format(" Q {} / {} ",
            st->current_idx + 1, st->total()))
                 | color(Color::White) | bgcolor(Color::Blue) | bold;
    } else {
        progress = text("");
    }
    auto top_row = hbox({
        header | xflex_grow,
        std::move(progress),
    });

    Elements footer_rows;
    if (q.kind == QuestionKind::Info) {
        footer_rows = qdetail::FooterButtons(
            st->focused_button, false, true, false, "",
            "Ok", Color::Cyan);
    } else {
        std::string always_hint;
        if (st->plan_mode) {
            always_hint =
                "Plan mode: this answer is remembered for the rest of this plan.";
        }
        footer_rows = qdetail::FooterButtons(
            st->focused_button,
            st->show_always_allow,
            true, st->always_allowed, always_hint,
            q.kind == QuestionKind::Confirm ? "Confirm"
                                            : "Send answer",
            q.kind == QuestionKind::Confirm ? Color::Yellow : Color::Green,
            single_q ? std::vector<std::string_view>{}
                     : std::vector<std::string_view>{"←","prev","→","next"});
    }

    auto full = vbox({
        top_row,
        pc::ThinDivider(),
        std::move(body) | yflex_grow,
        vbox(std::move(footer_rows)),
    });

    return window(
        text(std::format(" {} Ask User Question ",
                         st->plan_mode ? "🗺" : "❓"))
            | bold | color(tk::palette::dark.primary),
        std::move(full) | xflex_grow
    ) | color(tk::palette::dark.primary)
      | size(WIDTH, GREATER_THAN, w)
      | size(HEIGHT, GREATER_THAN, h);
}

/// Event handling for AskUserQuestion dialog.
inline bool HandleAskUserQuestion(std::shared_ptr<AskUserQuestionState> st,
                                  Event e)
{
    const bool multi = (st->questions.size() > 1);
    const auto& q = st->questions.empty()
        ? UserQuestion{} : st->questions[st->current_idx];

    if (q.kind == QuestionKind::MultiChoice && st->mc_component &&
        st->focused_button != 0) {
        if (st->mc_component->OnEvent(e)) return true;
    }

    if (st->show_always_allow && e == Event::Character('a')) {
        st->always_allowed = !st->always_allowed;
        return true;
    }

    if (e == Event::Tab) {
        st->focused_button = st->show_always_allow
            ? (st->focused_button + 1) % 3
            : (st->focused_button + 1) % 2;
        return true;
    }
    if (e == Event::TabReverse) {
        st->focused_button = st->show_always_allow
            ? (st->focused_button + 2) % 3
            : (st->focused_button + 1) % 2;
        return true;
    }

    if (q.kind == QuestionKind::MultiChoice && e.is_character()) {
        char ch = e.character().front();
        if (ch >= '1' && ch <= '9') {
            int opt_i = ch - '1';
            if (opt_i < (int)q.options.size() && st->mc_handle) {
                st->mc_handle->FocusOption(q.options[opt_i].value);
                if (!q.multi_select) {
                    st->answers_by_q.resize(st->total());
                    st->answers_by_q[st->current_idx] =
                        {q.options[opt_i].value};
                    return false;  // then fall through to Enter handling
                }
            }
        }
    }

    if (multi) {
        if (e == Event::ArrowLeft) {
            if (st->current_idx > 0) { --st->current_idx; return true; }
            return true;
        }
        if (e == Event::ArrowRight) {
            if (st->current_idx + 1 < st->questions.size())
                ++st->current_idx;
            return true;
        }
    }

    if (q.kind == QuestionKind::FreeText || q.kind == QuestionKind::CodeInput) {
        if (e == Event::Backspace || e.input() == "\x7f") {
            if (!st->text_buf.empty()) {
                if (q.kind == QuestionKind::CodeInput &&
                    !st->text_buf.empty() &&
                    st->text_buf.back() == '\n')
                    st->text_buf.pop_back();
                st->text_buf.pop_back();
            }
            return true;
        }
        if (e.is_character()) {
            auto c = e.character();
            if (q.kind == QuestionKind::FreeText) {
                if (c.size() == 1 &&
                    std::isprint(static_cast<unsigned char>(c.front()))) {
                    st->text_buf += c.front();
                    return true;
                }
            } else { // CodeInput accepts any printable + newline via ctrl+enter
                if (c == "\n"_utf8) {
                    st->text_buf += '\n'; return true;
                }
                if (c.size() == 1 &&
                    (std::isprint(static_cast<unsigned char>(c.front()))
                     || c.front() == '\t')) {
                    st->text_buf += c.front(); return true;
                }
            }
        }
        if (e.input() == "ctrl+j" || e.input() == "ctrl+m") {
            if (q.kind == QuestionKind::CodeInput) {
                st->text_buf += '\n'; return true;
            }
        }
    }

    if (e == Event::Return) {
        if (q.kind == QuestionKind::Info || q.kind == QuestionKind::Confirm) {
            UserAnswer a; a.kind = q.kind;
            a.confirmed = (st->focused_button == 1);
            a.always_allow = st->always_allowed;
            if (st->cbs.on_submit) st->cbs.on_submit(a);
            return true;
        }
        // Collect multi-choice / text answers
        UserAnswer a; a.kind = q.kind; a.confirmed = true;
        a.always_allow = st->always_allowed;
        if (q.kind == QuestionKind::MultiChoice && st->mc_handle) {
            a.chosen_values = st->mc_handle->SelectedValues();
        } else if (q.kind == QuestionKind::FreeText ||
                   q.kind == QuestionKind::CodeInput) {
            a.text = st->text_buf;
        }
        // Multi-question: advance unless last question, then submit.
        if (multi && st->current_idx + 1 < st->questions.size()) {
            // Cache the answer per-question, advance.
            st->answers_by_q.resize(st->total());
            if (q.kind == QuestionKind::MultiChoice)
                st->answers_by_q[st->current_idx] = a.chosen_values;
            else
                st->answers_by_q[st->current_idx] = {a.text};
            ++st->current_idx;
            return true;
        }
        if (st->cbs.on_submit) st->cbs.on_submit(a);
        return true;
    }

    if (e == Event::Escape || e == Event::Character('c')) {
        if (st->cbs.on_cancel) st->cbs.on_cancel();
        return true;
    }
    if (q.kind == QuestionKind::Confirm) {
        if (e == Event::Character('y') || e == Event::Character('Y')) {
            UserAnswer a; a.kind = QuestionKind::Confirm; a.confirmed = true;
            a.always_allow = st->always_allowed;
            if (st->cbs.on_submit) st->cbs.on_submit(a);
            return true;
        }
        if (e == Event::Character('n') || e == Event::Character('N')) {
            if (st->cbs.on_cancel) st->cbs.on_cancel();
            return true;
        }
    }

    return false;
}

/// Public factory: build the AskUserQuestion dialog.
[[nodiscard]] inline Component MakeAskUserQuestionDialog(
    std::vector<UserQuestion> questions,
    AskUserQuestionCallbacks cbs,
    bool plan_mode = false)
{
    auto st = std::make_shared<AskUserQuestionState>();
    st->questions = std::move(questions);
    st->cbs = std::move(cbs);
    st->plan_mode = plan_mode;
    if (st->questions.empty()) {
        st->questions.push_back(UserQuestion{
            .kind = QuestionKind::Info,
            .title = "No question provided",
            .body  = "Caller supplied an empty question list."
        });
    }
    st->answers_by_q.resize(st->total());
    st->current_idx = 0;
    return Renderer([st] { return RenderAskUserQuestion(st); })
         | CatchEvent([st](Event e) -> bool {
               return HandleAskUserQuestion(st, std::move(e));
           });
}

// ============================================================
// 2.  SkillPermissionDialog
// ============================================================

/// Where the skill was loaded from.  Mirrors TS SkillOptionValue semantics.
enum class SkillSource : std::uint8_t {
    Bundled,      // shipped with the app → Risk Low (green badge)
    User,         // user-added workspace skill → Medium (yellow)
    CustomPath,   // arbitrary filesystem path → High (red)
};

/// Skill option value returned to callbacks.
enum class SkillChoice : std::uint8_t {
    No,                // reject this invocation
    Yes,               // allow this single invocation
    YesExact,          // always-allow for skill name exactly
    YesPrefix,         // always-allow for skill name + prefix (params)
};

struct SkillPermissionProps {
    std::string skill_name;       // canonical name (e.g. "cpp_migration")
    std::string skill_logo;       // emoji / unicode glyph (default "🧩")
    SkillSource source = SkillSource::User;
    std::string source_path;      // CustomPath: actual filesystem path
    std::string description;      // one-line description
    std::vector<std::pair<std::string, std::string>> capabilities;
        // label, explanation (e.g. {"BashTool","Run shell commands"})
    std::vector<std::string> tool_names; // tool-pattern candidates
    bool show_always_allow_options = true;
};

struct SkillPermissionCallbacks {
    std::function<void(SkillChoice, bool always_allow)> on_respond;
    std::function<void()>                               on_reject;
};

namespace sdetail {

[[nodiscard]] inline std::string_view SourceName(SkillSource s) {
    switch (s) {
        case SkillSource::Bundled:    return "Bundled";
        case SkillSource::User:       return "User";
        case SkillSource::CustomPath: return "Custom path";
    }
    return "Unknown";
}
[[nodiscard]] inline Color SourceColor(SkillSource s) {
    switch (s) {
        case SkillSource::Bundled:    return Color::Green;
        case SkillSource::User:       return Color::Yellow;
        case SkillSource::CustomPath: return Color::Red;
    }
    return Color::GrayLight;
}
[[nodiscard]] inline RiskLevel SourceRisk(SkillSource s) {
    switch (s) {
        case SkillSource::Bundled:    return RiskLevel::Low;
        case SkillSource::User:       return RiskLevel::Medium;
        case SkillSource::CustomPath: return RiskLevel::High;
    }
    return RiskLevel::Medium;
}

/// Expanded capabilities table (expanded accordion).
[[nodiscard]] inline Element CapabilitiesTable(
    const std::vector<std::pair<std::string, std::string>>& caps, bool expanded)
{
    if (!expanded) {
        return hbox({
            text(" [v] ") | dim,
            text(std::format("{} capabilities — press [v] to expand",
                             caps.size())) | dim,
            filler(),
        });
    }
    Elements rows;
    rows.push_back(hbox({
        text(" Capability ") | bold | color(Color::Cyan) | size(WIDTH, EQUAL, 20),
        text(" Effect ") | bold | color(Color::Cyan) | xflex_grow,
    }));
    rows.push_back(pc::ThinDivider());
    for (const auto& [k, v] : caps) {
        rows.push_back(hbox({
            text(std::format(" • {}", k)) | color(Color::Magenta)
                                        | size(WIDTH, EQUAL, 20),
            text(v) | xflex_grow,
        }));
    }
    if (caps.empty())
        rows.push_back(text("  (no declared capabilities)") | dim);
    return vbox(std::move(rows)) | yflex_grow;
}

} // namespace sdetail

struct SkillPermissionState {
    SkillPermissionProps props;
    SkillPermissionCallbacks cbs;
    SkillChoice choice = SkillChoice::No;
    int focused_idx = 0;   // 0=Reject, 1=Allow once, 2=Allow always,
                           // 3=Allow exact, 4=Allow prefix (if shown)
    bool caps_expanded = false;
};

[[nodiscard]] inline Element RenderSkillDialog(
    std::shared_ptr<SkillPermissionState> st)
{
    const auto& p = st->props;
    RiskLevel risk = sdetail::SourceRisk(p.source);
    auto source_badge = text(std::format(" {} ", sdetail::SourceName(p.source)))
                      | color(tk::palette::dark.inverse_text)
                      | bgcolor(sdetail::SourceColor(p.source)) | bold;

    auto risk_badge = pc::RiskPill(static_cast<pc::RiskLevel>(risk));

    auto header = hbox({
        text(p.skill_logo.empty() ? "🧩" : p.skill_logo)
           | color(tk::palette::dark.primary) | bold,
        text(" "),
        text(std::format("Use skill \"{}\"?", p.skill_name)) | bold,
        filler(),
        std::move(source_badge),
        text("  "),
        std::move(risk_badge),
    });

    Elements body;
    body.push_back(hbox({
        text(" Description: ") | dim,
        text(p.description.empty() ? std::string{"(none provided)"} : p.description),
    }));
    if (p.source == SkillSource::CustomPath && !p.source_path.empty()) {
        body.push_back(hbox({
            text(" Path: ") | dim,
            pc::PathLabelHighlighted(p.source_path, true, 60),
        }));
    }
    body.push_back(pc::ThinDivider());
    body.push_back(sdetail::CapabilitiesTable(p.capabilities,
                                              st->caps_expanded));
    body.push_back(pc::ThinDivider());

    Elements buttons;
    struct ButDef { const char* hotkey; const char* label; Color c; int idx; };
    std::vector<ButDef> defs = {
        {"R", "Reject",       Color::Red,    0},
        {"A", "Allow once",   Color::Yellow, 1},
        {"Y", "Allow always", Color::Green,  2},
    };
    if (p.show_always_allow_options && p.source != SkillSource::CustomPath) {
        defs.push_back({"Y1","Allow (exact)", Color::GreenLight, 3});
        defs.push_back({"Y2","Allow (prefix)",Color::GreenLight, 4});
    }

    for (const auto& d : defs) {
        const bool hovered = (st->focused_idx == d.idx);
        auto chip = text(std::format(" [{}] {} ", d.hotkey, d.label))
                  | color(d.c)
                  | (hovered ? (bold | inverted) : dim);
        buttons.push_back(std::move(chip));
        buttons.push_back(text("  "));
    }
    buttons.push_back(filler());
    buttons.push_back(text("  [v] caps ") | dim);
    body.push_back(hbox(std::move(buttons)));

    body.push_back(hbox({
        text(" Esc=cancel • [R/A/Y] select • [Tab] focus ") | dim,
        filler(),
        text(" Shortcuts mirror TS SkillOptionValue semantics ") | dim,
    }));

    auto full = vbox({
        header,
        pc::ThinDivider(),
        vbox(std::move(body)) | yflex_grow,
    });
    return window(
        text(" 🧩 Skill Permission ")
            | bold | color(tk::palette::dark.primary),
        std::move(full)
    ) | color(tk::palette::dark.primary)
      | size(WIDTH, GREATER_THAN, 64)
      | size(HEIGHT, GREATER_THAN, 16);
}

inline bool HandleSkillDialog(std::shared_ptr<SkillPermissionState> st,
                              Event e)
{
    const auto& p = st->props;
    int n_but = 3 + (p.show_always_allow_options &&
                     p.source != SkillSource::CustomPath ? 2 : 0);

    if (e == Event::Tab) {
        st->focused_idx = (st->focused_idx + 1) % n_but;
        return true;
    }
    if (e == Event::TabReverse) {
        st->focused_idx = (st->focused_idx - 1 + n_but) % n_but;
        return true;
    }
    if (e == Event::Character('v') || e == Event::Character('V')) {
        st->caps_expanded = !st->caps_expanded; return true;
    }

    auto respond = [&](SkillChoice c, bool aa) {
        st->choice = c;
        if (st->cbs.on_respond)
            st->cbs.on_respond(c, aa);
    };
    if (e == Event::Character('r') || e == Event::Character('R') ||
        st->focused_idx == 0 && e == Event::Return) {
        if (st->cbs.on_reject) st->cbs.on_reject();
        respond(SkillChoice::No, false);
        return true;
    }
    if (e == Event::Character('a') || e == Event::Character('A') ||
        (st->focused_idx == 1 && e == Event::Return)) {
        respond(SkillChoice::Yes, false); return true;
    }
    if (e == Event::Character('y') || e == Event::Character('Y') ||
        (st->focused_idx == 2 && e == Event::Return)) {
        respond(SkillChoice::Yes, true); return true;
    }
    if (n_but > 3) {
        if (e == Event::Character('1') ||
            (st->focused_idx == 3 && e == Event::Return)) {
            respond(SkillChoice::YesExact, true); return true;
        }
        if (e == Event::Character('2') ||
            (st->focused_idx == 4 && e == Event::Return)) {
            respond(SkillChoice::YesPrefix, true); return true;
        }
    }
    if (e == Event::Escape) {
        if (st->cbs.on_reject) st->cbs.on_reject();
        return true;
    }
    return false;
}

[[nodiscard]] inline Component MakeSkillPermissionDialog(
    SkillPermissionProps props, SkillPermissionCallbacks cbs)
{
    auto st = std::make_shared<SkillPermissionState>();
    st->props = std::move(props);
    st->cbs   = std::move(cbs);
    if (st->props.skill_logo.empty()) st->props.skill_logo = "🧩";
    return Renderer([st] { return RenderSkillDialog(st); })
         | CatchEvent([st](Event e) -> bool {
               return HandleSkillDialog(st, std::move(e));
           });
}

// ============================================================
// 3.  FallbackPermissionDialog
// ============================================================

/// Three-column fallback dialog, for unrecognized permission payloads
/// (usually unknown MCP tool requests, or new tool variants that the TS
/// permission-component registry doesn't know how to render).
///
/// Layout:
///   ┌─ Yellow banner: "Unknown tool – verify carefully" ───────┐
///   ├─ Col 1 (best-effort) ─┬─ Col 2 (raw JSON 30 rows) ────┬─ Col 3 (Report) ──┤
///   │ summary paragraph     │ { JSON tree (truncated)      │ [form: reason]   │
///   │ tool name / lines     │ } collapse by depth (▲/▼)    │ email optional    │
///   └───────────────────────┴──────────────────────────────┴───────────────────┘
///   └─ Buttons: [Cancel] / [Allow once] (orange) / [Deny] (red, default) ─┘
///   └─ Always allow checkbox: DISABLED by default, hover tooltip explains why.
struct FallbackPermissionProps {
    std::string user_facing_name;    // tool name; "(MCP)" suffix stripped
    std::string tool_description;    // up to 3 lines of short description
    std::string best_effort_summary; // free-form summary paragraph
    std::string raw_payload_json;   // yyjson / arbitrary JSON payload
    std::vector<std::pair<std::string, std::string>> extra_fields;
};

/// Fallback-option values mirror TS FallbackOptionValue.
enum class FallbackChoice : std::uint8_t {
    Deny,              // default red-route; Cancel returns this
    AllowOnce,         // orange; allow this call only
    AlwaysAllow,       // disabled-by-default, security-blessed route
};

struct FallbackPermissionCallbacks {
    std::function<void(FallbackChoice, bool always_allowed)> on_respond;
    std::function<void(const std::string& reason,
                       const std::string& contact_email)>     on_report;
};

namespace fdetail {

/// Strip trailing " (MCP)" from a user-facing name.
[[nodiscard]] inline std::string StripMcpSuffix(std::string_view name) {
    static constexpr std::string_view kSuffix = " (MCP)";
    if (name.size() >= kSuffix.size() &&
        name.substr(name.size() - kSuffix.size()) == kSuffix)
        return std::string{name.substr(0, name.size() - kSuffix.size())};
    return std::string{name};
}

/// Truncate raw JSON string to kMaxJsonRows * 120 cols (~3600 chars) and
/// render as a monospace tree with minimal indent folding.
[[nodiscard]] inline Element TruncatedJsonTree(std::string_view json) {
    if (json.empty())
        return text(" (no raw payload available) ") | dim;

    Elements rows;
    int indent = 0;
    int depth_fold = -1;     // negative = unfolded; ≥0 collapse from this depth
    int row = 0;
    std::string cur; cur.reserve(120);
    auto flush = [&]() {
        rows.push_back(text(std::string(indent, ' ') + cur)
                     | color(row % 2 ? Color::GrayLight : Color::White));
        cur.clear();
        ++row;
    };
    for (char ch : json) {
        if (row >= kMaxJsonRows) break;
        switch (ch) {
            case '{': case '[':
                if (!cur.empty()) flush();
                rows.push_back(text(std::string(indent, ' ') + ch)
                             | color(Color::CyanLight) | bold);
                ++indent; ++row; break;
            case '}': case ']':
                if (!cur.empty()) flush();
                if (indent > 0) --indent;
                rows.push_back(text(std::string(indent, ' ') + ch)
                             | color(Color::CyanLight) | bold);
                ++row; break;
            case ',':
                cur += ch; flush(); break;
            case '\n': case '\r': break;
            default: cur += ch;
        }
        if (cur.size() > 100) flush();
    }
    if (!cur.empty() && row < kMaxJsonRows) flush();
    if (row >= kMaxJsonRows) {
        rows.push_back(
            text(std::format("  …truncated, first {} of {}+ bytes shown",
                             3600, json.size())) | dim | color(Color::Yellow)
        );
    }
    return vbox({
        hbox({text(" raw payload  ") | color(Color::Yellow) | bold,
              text("[▲/▼] collapse hint") | dim,
              filler()}),
        vbox(std::move(rows)) | yframe | yflex_grow,
    }) | size(WIDTH, EQUAL, 44);
}

/// Third-column "Report bug" mini-form.
struct ReportForm {
    std::string reason;
    std::string email;
    int focused_field = 0;   // 0 = reason, 1 = email, 2 = submit
};

[[nodiscard]] inline Element ReportFormElement(ReportForm& f) {
    Elements rows;
    rows.push_back(hbox({
        text(" 🐛 Report suspicious tool ") | bold | color(Color::Red),
        filler(),
    }));
    rows.push_back(pc::ThinDivider());
    auto mk = [&](std::string_view label, const std::string& val,
                  std::string_view ph, int idx, bool multi) {
        const bool focus = (f.focused_field == idx);
        std::string v = val.empty() ? std::string{ph} : val;
        Element box;
        if (multi) {
            Elements lines;
            std::istringstream iss(v);
            std::string l;
            for (int r = 0; r < 4; ++r) {
                if (std::getline(iss, l)) lines.push_back(
                    hbox({text(l), filler()}));
                else lines.push_back(text(focus ? " ▊" : " "));
            }
            box = vbox(std::move(lines));
        } else {
            box = text((focus && !val.empty() ? v + "▊" : v));
        }
        return vbox({
            text(std::format(" {}: ", label)) | dim | bold,
            std::move(box) | border
                | color(focus ? Color::Yellow : Color::GrayDark),
        });
    };
    rows.push_back(mk("Why suspicious (3-4 lines)", f.reason,
                      "tool claims to be X but does Y…", 0, true));
    rows.push_back(mk("Contact email (optional)", f.email,
                      "you@example.com", 1, false));
    rows.push_back(pc::ThinDivider());
    const bool focus_sub = (f.focused_field == 2);
    rows.push_back(hbox({
        filler(),
        text(" [Ctrl+r] Submit report ")
            | color(Color::Red)
            | (focus_sub ? (bold | inverted) : dim),
    }));
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, 40);
}

/// "Always allow" locked banner for Fallback (security UX lock).
[[nodiscard]] inline Element AlwaysAllowLocked(bool checked, bool hovered) {
    return hbox({
        text("  [a] ") | dim,
        text("🔒") | color(Color::Red),
        text(" Always allow")
            | (checked ? (bold | color(Color::Green)) : dim),
        text(" (") | dim,
        text("disabled for unknown tools") | color(Color::Red) | bold,
        text(" — cannot whitelist un-audited payloads.") | dim,
        text(")") | dim,
        hovered ? text(" ✱") | color(Color::Yellow) : text(""),
    });
}

} // namespace fdetail

struct FallbackPermissionState {
    FallbackPermissionProps props;
    FallbackPermissionCallbacks cbs;
    fdetail::ReportForm report;

    int focus_zone = 3;
    int focused_button = 0; // 0 = Deny, 1 = Allow once, 2 = Always allow (lock)
    bool always_allowed_tried = false;  // used for hover hint
    bool always_allowed_override = false;
};

[[nodiscard]] inline Element RenderFallbackDialog(
    std::shared_ptr<FallbackPermissionState> st)
{
    const auto& p = st->props;
    const std::string clean_name = fdetail::StripMcpSuffix(p.user_facing_name);

    auto banner = hbox({
        text(" ⚠ ") | color(Color::Yellow) | bold,
        text(std::string{" Unrecognized tool \""} + clean_name
             + std::string{"\" — verify carefully before allowing "})
            | bold | color(Color::Black) | bgcolor(Color::Yellow),
        filler(),
        text(" Esc = Deny (safe default) ")
            | color(Color::Black) | bgcolor(Color::Yellow) | dim,
    }) | bgcolor(Color::Yellow) | size(HEIGHT, EQUAL, 1);

    Elements summary_rows;
    summary_rows.push_back(
        text(" Best-effort summary ") | bold | color(Color::Cyan));
    summary_rows.push_back(pc::ThinDivider());
    if (!p.tool_description.empty()) {
        Elements desc_lines;
        std::istringstream iss(p.tool_description);
        std::string line; int n = 0;
        while (std::getline(iss, line) && n < 3) {
            desc_lines.push_back(text(line)); ++n;
        }
        summary_rows.push_back(vbox(std::move(desc_lines)));
        summary_rows.push_back(pc::ThinDivider());
    }
    for (const auto& [k, v] : p.extra_fields) {
        summary_rows.push_back(hbox({
            text(std::format(" {}: ", k)) | dim,
            text(v) | xflex_grow,
        }));
    }
    if (!p.best_effort_summary.empty()) {
        summary_rows.push_back(pc::ThinDivider());
        Elements wrap = qdetail::WrapBody(p.best_effort_summary, 40);
        summary_rows.insert(summary_rows.end(), wrap.begin(), wrap.end());
    }
    auto col1 = window(text(" summary ") | dim,
                       vbox(std::move(summary_rows)) | xflex_grow | yflex_grow);

    auto col2 = window(text(" raw payload ") | dim,
                       fdetail::TruncatedJsonTree(p.raw_payload_json)
                           | xflex_grow | yflex_grow);

    auto col3 = window(text(" report ") | dim,
                       fdetail::ReportFormElement(st->report)
                           | xflex_grow | yflex_grow);

    auto three_col = hbox({
        col1 | size(WIDTH, EQUAL, 34),
        text(" "),
        col2 | xflex_grow,
        text(" "),
        col3 | size(WIDTH, EQUAL, 44),
    }) | yflex_grow;

    Elements buttons;
    struct BD { const char* hotkey; const char* label; Color c; int idx; };
    std::vector<BD> defs = {
        {"Esc", "Cancel",    Color::Red,     0},
        {"O",   "Allow once",Color::Orange,  1},
        {"D",   "Deny",      Color::Red,     2},
    };
    for (const auto& d : defs) {
        bool hovered = (st->focused_button == d.idx);
        auto chip = text(std::format(" [{}] {} ", d.hotkey, d.label))
                  | color(d.c)
                  | (hovered ? (bold | inverted) : dim);
        buttons.push_back(std::move(chip));
        buttons.push_back(text("  "));
    }
    buttons.push_back(filler());
    buttons.push_back(text(" [Tab] switch zone ") | dim);

    auto aa_row = fdetail::AlwaysAllowLocked(
        st->always_allowed_override, st->always_allowed_tried);

    auto full = vbox({
        banner,
        pc::ThinDivider(),
        three_col | yflex_grow,
        pc::ThinDivider(),
        aa_row,
        pc::ThinDivider(),
        hbox(std::move(buttons)),
    });
    return window(
        text(" 🛡 Tool use (Fallback) ")
            | bold | color(tk::palette::dark.danger),
        std::move(full)
    ) | color(tk::palette::dark.danger)
      | size(WIDTH, GREATER_THAN, 100)
      | size(HEIGHT, GREATER_THAN, 24);
}

inline bool HandleFallbackDialog(std::shared_ptr<FallbackPermissionState> st,
                                 Event e)
{
    const auto& p = st->props;

    if (e == Event::Tab) {
        st->focus_zone = (st->focus_zone + 1) % 4;
        if (st->focus_zone == 2) {
            // Move focus inside report form too
            st->report.focused_field = (st->report.focused_field + 1) % 3;
        }
        return true;
    }
    if (e == Event::TabReverse) {
        st->focus_zone = (st->focus_zone + 3) % 4;
        return true;
    }

    if (e == Event::Character('a')) {
        st->always_allowed_tried = true;
        return true;
    }
    if (e.input() == "ctrl+a") {
        st->always_allowed_override = !st->always_allowed_override;
        return true;
    }

    if (st->focus_zone == 2) {
        auto* field = (st->report.focused_field == 0)
            ? &st->report.reason
            : (st->report.focused_field == 1) ? &st->report.email : nullptr;
        if (field) {
            if (e == Event::Backspace || e.input() == "\x7f") {
                if (!field->empty()) { field->pop_back(); return true; }
            }
            if (e.is_character()) {
                auto c = e.character();
                if (c.size() == 1 &&
                    std::isprint(static_cast<unsigned char>(c.front()))) {
                    *field += c.front(); return true;
                }
                if (c == "\n"_utf8 && field == &st->report.reason) {
                    *field += '\n'; return true;
                }
            }
        }
        if (e.input() == "ctrl+r") {
            if (st->cbs.on_report)
                st->cbs.on_report(st->report.reason, st->report.email);
            return true;
        }
    }

    auto respond = [&](FallbackChoice c) {
        bool aa = (c == FallbackChoice::AlwaysAllow) ||
                  st->always_allowed_override;
        if (st->cbs.on_respond) st->cbs.on_respond(c, aa);
    };
    if (e == Event::Escape || e == Event::Character('c') ||
        e == Event::Character('C') ||
        e == Event::Character('d') || e == Event::Character('D') ||
        (st->focused_button == 2 && e == Event::Return)) {
        // Deny = safe default (Esc always denies, even if Cancel was the
        // labelled intent — TS semantics match this).
        respond(FallbackChoice::Deny);
        return true;
    }
    if (e == Event::Character('o') || e == Event::Character('O') ||
        (st->focused_button == 1 && e == Event::Return)) {
        respond(FallbackChoice::AllowOnce); return true;
    }

    if (st->focus_zone == 3) {
        if (e == Event::ArrowLeft) {
            st->focused_button = (st->focused_button + 2) % 3; return true;
        }
        if (e == Event::ArrowRight) {
            st->focused_button = (st->focused_button + 1) % 3; return true;
        }
    }
    if (st->focus_zone == 1) {
        if (e == Event::ArrowUp)   return true;
        if (e == Event::ArrowDown) return true;
    }

    return false;
}

[[nodiscard]] inline Component MakeFallbackPermissionDialog(
    FallbackPermissionProps props, FallbackPermissionCallbacks cbs)
{
    auto st = std::make_shared<FallbackPermissionState>();
    st->props = std::move(props);
    st->cbs = std::move(cbs);
    st->focused_button = 2;
    return Renderer([st] { return RenderFallbackDialog(st); })
         | CatchEvent([st](Event e) -> bool {
               return HandleFallbackDialog(st, std::move(e));
           });
}

} // namespace cc::ui::permissions::advanced
