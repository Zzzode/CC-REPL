/// @file permission_single_prompt.cppm
/// @brief Single-request permission prompt dialog.
///
/// Covers ~80% of permission scenarios: one tool + one action needs approval.
///
/// Layout:
///   Header : tool icon + tool name + action kind + RiskPill (right)
///   Body   : one-line description ("BashTool wants to execute `rm -rf *.log`")
///          : optional destructive-warning banner (yellow)
///          : optional affected paths (with danger highlighting + depth tag)
///   Footer : 4-button row
///            [x] Deny (red)   + [ ] Always deny this tool  (checkbox)
///            [✓] Allow once   + [ ] Always allow this scope (checkbox)
///            [?] Sandbox mode toggle (grey, right-aligned)
///
/// Keyboard shortcuts: y=Allow, n=Deny, a=Always allow, d=Always deny,
///                     s=Toggle sandbox, Esc=Deny, Enter=Allow once
///
/// Engine logic is 100% delegated to cc.utils.permissions_engine.
/// This file only formats the UI and routes user input.
module;

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.single_prompt;

import cc.utils.permissions_engine;
import cc.ui.permissions.components;

export namespace cc::ui::permissions::single_prompt {
using namespace ftxui;
namespace pc = cc::ui::permissions::components;
namespace eng = cc::utils::permissions;

// ============================================================
// Types
// ============================================================

/// Final decision returned by the prompt.
enum class Decision : std::uint8_t {
    AllowOnce,      // Allow only this invocation
    AlwaysAllow,    // Persist an allow rule for this scope/tool
    Deny,           // Deny this invocation only
    AlwaysDeny,     // Persist a deny rule
    Abort,          // Escape / abort the whole operation
};

/// Action category mirrors the shared component enum for convenience.
using ActionKind = pc::ActionKind;
using RiskLevel  = pc::RiskLevel;

/// Tool-specific detail payload (rendered inside the body).
struct DetailBash {
    std::string command;
    std::optional<std::string> working_dir;
    bool is_destructive = false;
    std::string destructive_reason; // empty if is_destructive=false
};
struct DetailFileEdit {
    std::string file_path;
    std::string old_snippet;   // first 80 chars of old_string
    std::string new_snippet;   // first 80 chars of new_string
    bool creates_file = false;
};
struct DetailFileWrite {
    std::string file_path;
    std::size_t content_bytes = 0;
    std::size_t content_lines = 0;
    bool creates_file = false;
};
struct DetailFileRead {
    std::string file_path;
    std::optional<std::size_t> line_count;
};
struct DetailNetwork {
    std::string url_or_domain;
    bool is_upload = false;     // POST / PUT with a body
};
struct DetailMCP {
    std::string server_id;
    std::string tool_name;
    std::string description;
};
struct DetailSkill {
    std::string skill_name;
    std::string description;
};
struct DetailPlan {
    bool is_entry = true;   // true = Enter, false = Exit
};
struct DetailAskUser {
    std::string question;
    std::vector<std::string> options;
};
struct DetailGeneric {
    std::string description;   // free-form body
};

using ToolDetail = std::variant<
    DetailBash,
    DetailFileEdit,
    DetailFileWrite,
    DetailFileRead,
    DetailNetwork,
    DetailMCP,
    DetailSkill,
    DetailPlan,
    DetailAskUser,
    DetailGeneric
>;

/// Props for creating a single-prompt dialog.
struct SinglePromptProps {
    std::string tool_name;             // e.g. "BashTool"
    ActionKind action_kind;            // Read / Write / Execute / Network ...
    RiskLevel risk_level;              // Risk classification (from engine)
    std::string description;           // One-liner, e.g. "BashTool wants to execute…"
    std::vector<std::string> affected_paths;
    std::optional<std::string> workspace_root;   // for "inside workspace" badges
    ToolDetail detail;                 // Tool-specific rendering payload
    std::string rule_match_explanation; // from PermissionResult::explanation (may be "")
    // Callbacks
    std::function<void(Decision, bool sandbox_requested)> on_decide;
    std::function<void()> on_abort;  // optional
    // Initial state (optional)
    bool initial_sandbox_toggle = false;
};

// ============================================================
// Internal rendering helpers
// ============================================================

namespace detail {

/// Render the title bar: icon, tool name, action kind, risk pill.
[[nodiscard]] inline Element RenderTitleBar(const SinglePromptProps& p) {
    auto action_label = [&] {
        switch (p.action_kind) {
            case ActionKind::Read:        return "read";
            case ActionKind::Write:       return "write";
            case ActionKind::Execute:     return "execute";
            case ActionKind::Network:     return "network";
            case ActionKind::MCP:         return "mcp";
            case ActionKind::Environment: return "env";
            case ActionKind::Plan:        return "plan";
            case ActionKind::Other:       return "use";
        }
        return "use";
    }();
    return hbox({
        text(std::string{pc::ToolIconGlyph(p.tool_name)}) | bold,
        text(" "),
        text(p.tool_name) | bold | color(pc::ActionColor(p.action_kind)),
        text(" wants to ") | dim,
        text(action_label),
        filler(),
        pc::RiskPill(p.risk_level),
    });
}

/// Render tool-specific detail content.
[[nodiscard]] inline Element RenderToolDetail(const ToolDetail& det) {
    return std::visit([](const auto& d) -> Element {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, DetailBash>) {
            Elements els = { pc::CommandPreview(d.command, 80) };
            if (d.working_dir) {
                els.push_back(hbox({
                    text(" in: ") | dim,
                    pc::PathLabel(*d.working_dir, 60),
                }));
            }
            if (d.is_destructive) {
                els.insert(els.begin(),
                    pc::DestructiveWarningBanner(d.destructive_reason.empty()
                        ? "This command may permanently delete or overwrite files."
                        : d.destructive_reason));
            }
            return vbox(els);
        } else if constexpr (std::is_same_v<T, DetailFileEdit>) {
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
        } else if constexpr (std::is_same_v<T, DetailFileWrite>) {
            return vbox({
                hbox({ text("File: ") | dim,
                       pc::PathLabel(d.file_path, 70) }),
                hbox({ text("Size: ") | dim,
                       text(std::format("{} bytes (~{} line{})",
                           d.content_bytes, d.content_lines,
                           d.content_lines == 1 ? "" : "s")) | dim }),
                (d.creates_file ? hbox({ text("new file") | color(Color::Yellow) | dim })
                                : text("")),
            });
        } else if constexpr (std::is_same_v<T, DetailFileRead>) {
            auto line_el = d.line_count
                ? text(std::format(" ({})", *d.line_count)) | dim
                : text("");
            return hbox({
                text("File: ") | dim,
                pc::PathLabel(d.file_path, 70),
                line_el,
            });
        } else if constexpr (std::is_same_v<T, DetailNetwork>) {
            auto tag = d.is_upload
                ? hbox({ text(" UPLOAD ") | color(Color::White)
                                     | bgcolor(Color::Red) | bold })
                : hbox({ text(" fetch ") | color(Color::White)
                                     | bgcolor(Color::Blue) | dim });
            return hbox({ tag, text("  "), pc::PathLabel(d.url_or_domain, 70) });
        } else if constexpr (std::is_same_v<T, DetailMCP>) {
            return vbox({
                hbox({ text("MCP server: ") | dim,
                       text(d.server_id) | color(Color::Purple) }),
                hbox({ text("Tool: ") | dim,
                       text(d.tool_name) | bold }),
                (d.description.empty() ? text("")
                     : paragraph(d.description) | dim),
            });
        } else if constexpr (std::is_same_v<T, DetailSkill>) {
            return vbox({
                hbox({ text("Skill: ") | dim,
                       text(d.skill_name) | color(Color::Magenta) | bold }),
                (d.description.empty() ? text("")
                     : paragraph(d.description) | dim),
            });
        } else if constexpr (std::is_same_v<T, DetailPlan>) {
            return text(d.is_entry ? "Enter plan mode (output-only, pauses execution)"
                                   : "Exit plan mode and resume execution");
        } else if constexpr (std::is_same_v<T, DetailAskUser>) {
            Elements els = { paragraph(d.question) };
            for (const auto& opt : d.options) {
                els.push_back(hbox({ text("  • ") | dim, text(opt) }));
            }
            return vbox(els);
        } else if constexpr (std::is_same_v<T, DetailGeneric>) {
            return paragraph(d.description);
        }
    }, det);
}

/// Render affected paths section.
[[nodiscard]] inline Element RenderAffectedPaths(
    const std::vector<std::string>& paths,
    std::optional<std::string_view>)
{
    if (paths.empty()) return text("");
    Elements header = {
        hbox({ text(" Affected paths") | bold, filler() }),
    };
    Elements list = pc::SummarisePathList(paths, 6, 60);
    // Prepend 2-space indent
    for (auto& el : list) {
        el = hbox({ text("  "), el });
    }
    Elements all = header;
    all.insert(all.end(), list.begin(), list.end());
    return vbox(all) | color(Color::GrayLight);
}

/// Render matched-rule explanation footnote.
[[nodiscard]] inline Element RenderRuleExplanation(
    std::string_view explanation)
{
    if (explanation.empty()) return text("");
    return hbox({
        text(" rule: ") | dim,
        text(std::string{explanation}) | dim | color(Color::Cyan),
    });
}

} // namespace detail

// ============================================================
// Full element renderer
// ============================================================

/// Internal state shared between Renderer and CatchEvent.
struct PromptState {
    SinglePromptProps props;
    bool always_deny_checkbox = false;
    bool always_allow_checkbox = false;
    bool sandbox_toggle = false;
    int  focused_button = 0;   // 0=Allow once  1=Deny  2=Always allow  3=Always deny
    // One-shot guard: TS contract fires EXACTLY ONE terminal callback per prompt.
    // Priority 1) on_abort (if present), 2) else on_decide(...). NEVER both.
    bool callback_fired = false;
};

/// Render the complete single-prompt dialog body.
[[nodiscard]] inline Element RenderSinglePrompt(std::shared_ptr<PromptState> st) {
    const auto& p = st->props;

    auto title_bar = detail::RenderTitleBar(p);
    auto detail_el = detail::RenderToolDetail(p.detail);
    auto desc_el   = paragraph(p.description) | color(Color::White);
    auto paths_el  = detail::RenderAffectedPaths(p.affected_paths,
                        p.workspace_root ?
                            std::optional<std::string_view>{*p.workspace_root}
                          : std::nullopt);
    auto rule_el   = detail::RenderRuleExplanation(p.rule_match_explanation);

    // ---- Footer: button row ----
    // Buttons: [x] Deny + chk  |  [✓] Allow once + chk  |  Sandbox toggle
    auto deny_btn = [&] {
        auto focused = st->focused_button == 1;
        auto el = hbox({
            text("× ") | bold | color(Color::Red),
            text("Deny"),
        });
        if (focused) el = el | inverted | bold;
        return hbox({ text(" "), el, text(" ") })
               | borderStyled(Color::Red) | color(Color::Red);
    }();
    auto always_deny_cb = pc::CheckboxBox(st->always_deny_checkbox);

    auto allow_btn = [&] {
        auto focused = st->focused_button == 0;
        auto el = hbox({
            text("✓ ") | bold | color(Color::Green),
            text("Allow once"),
        });
        if (focused) el = el | inverted | bold;
        return hbox({ text(" "), el, text(" ") })
               | borderStyled(Color::Green) | color(Color::Green);
    }();
    auto always_allow_cb = pc::CheckboxBox(st->always_allow_checkbox);

    auto sandbox_btn = [&] {
        auto on = st->sandbox_toggle;
        return hbox({
            text("Sandbox ") | dim,
            on ? text("[ON]")  | color(Color::Cyan) | bold
               : text("[OFF]") | dim,
        }) | borderStyled(Color::GrayDark);
    }();

    auto footer = vbox({
        pc::ThinDivider(),
        hbox({
            // Left: Deny column
            vbox({
                hbox({ deny_btn, text("  "), always_deny_cb,
                       text(" Always deny this tool") | dim }),
                text("") | size(HEIGHT, EQUAL, 1),
            }) | xflex,
            // Middle: Allow column
            vbox({
                hbox({ allow_btn, text("  "), always_allow_cb,
                       text(" Always allow this scope") | dim }),
                text("") | size(HEIGHT, EQUAL, 1),
            }) | xflex,
            // Right: Sandbox toggle
            sandbox_btn,
        }),
        text(""),
        pc::KeyboardHintRow({
            {"y",     "Allow"},
            {"n",     "Deny"},
            {"a",     "Always allow"},
            {"d",     "Always deny"},
            {"s",     "Toggle sandbox"},
            {"Esc",   "Cancel"},
        }),
    });

    // Assemble the body
    Elements body_els;
    body_els.push_back(title_bar);
    body_els.push_back(pc::ThinDivider());
    body_els.push_back(text(""));
    body_els.push_back(desc_el);
    body_els.push_back(text(""));
    body_els.push_back(detail_el);
    if (!p.affected_paths.empty()) {
        body_els.push_back(text(""));
        body_els.push_back(paths_el);
    }
    if (!p.rule_match_explanation.empty()) {
        body_els.push_back(text(""));
        body_els.push_back(rule_el);
    }
    body_els.push_back(text(""));
    body_els.push_back(footer);

    auto body = vbox(body_els) | xflex;

    auto border_col = [&] {
        switch (p.risk_level) {
            case RiskLevel::Low:      return Color::Cyan;
            case RiskLevel::Medium:   return Color::Yellow;
            case RiskLevel::High:     return Color::Red;
            case RiskLevel::Critical: return Color::RedLight;
        }
        return Color::GrayLight;
    }();

    return window(
        text(" \U0001F510 Permission ") | bold | color(Color::Yellow),
        body | xflex
    ) | color(border_col) | size(WIDTH, LESS_THAN, 90);
}

// ============================================================
// Public factory: make the interactive component
// ============================================================

/// Construct a full interactive single-permission-prompt component.
[[nodiscard]] inline Component MakeSinglePromptDialog(SinglePromptProps props) {
    constexpr int kButtonCount = 4;
    auto state = std::make_shared<PromptState>();
    state->props = std::move(props);
    state->sandbox_toggle = state->props.initial_sandbox_toggle;

    auto emit = [state](Decision d) {
        if (state->callback_fired) return;
        state->callback_fired = true;
        // TS contract: one-shot. For Abort, prefer on_abort when present; else
        // fall back to on_decide(Abort). For all other decisions, use on_decide.
        if (d == Decision::Abort && state->props.on_abort) {
            state->props.on_abort();
            return;
        }
        if (state->props.on_decide) {
            state->props.on_decide(d, state->sandbox_toggle);
        }
    };

    return Renderer([state] { return RenderSinglePrompt(state); })
         | CatchEvent([state, emit](Event event) -> bool {
        // ----- Movement between buttons -----
        if (event == Event::ArrowLeft || event == Event::Character('h')) {
            state->focused_button = (state->focused_button - 1 + kButtonCount) % kButtonCount;
            return true;
        }
        if (event == Event::ArrowRight || event == Event::Character('l')) {
            state->focused_button = (state->focused_button + 1) % kButtonCount;
            return true;
        }
        if (event == Event::Tab) {
            state->focused_button = (state->focused_button + 1) % kButtonCount;
            return true;
        }
        if (event == Event::TabReverse) {
            state->focused_button = (state->focused_button - 1 + kButtonCount) % kButtonCount;
            return true;
        }

        // ----- Direct shortcuts -----
        if (event == Event::Character('y')) {
            emit(state->always_allow_checkbox ? Decision::AlwaysAllow : Decision::AllowOnce);
            return true;
        }
        if (event == Event::Character('n')) {
            emit(state->always_deny_checkbox ? Decision::AlwaysDeny : Decision::Deny);
            return true;
        }
        if (event == Event::Character('a')) {
            emit(Decision::AlwaysAllow);
            return true;
        }
        if (event == Event::Character('d')) {
            emit(Decision::AlwaysDeny);
            return true;
        }
        if (event == Event::Character('s')) {
            state->sandbox_toggle = !state->sandbox_toggle;
            return true;
        }

        // ----- Checkbox toggles -----
        if (event == Event::Character('1')) { state->always_deny_checkbox  = !state->always_deny_checkbox;  return true; }
        if (event == Event::Character('2')) { state->always_allow_checkbox = !state->always_allow_checkbox; return true; }

        // ----- Enter activates focused button -----
        if (event == Event::Return) {
            switch (state->focused_button) {
                case 0: // Allow once
                    emit(state->always_allow_checkbox ? Decision::AlwaysAllow : Decision::AllowOnce);
                    return true;
                case 1: // Deny
                    emit(state->always_deny_checkbox ? Decision::AlwaysDeny : Decision::Deny);
                    return true;
                case 2: // Always allow
                    emit(Decision::AlwaysAllow);
                    return true;
                case 3: // Always deny
                    emit(Decision::AlwaysDeny);
                    return true;
            }
            return true;
        }

        // ----- Escape: abort with one-shot priority ----
        // Priority: (1) on_abort if set, (2) else on_decide(Abort).
        // Never both — emit() enforces the oneshot via callback_fired.
        if (event == Event::Escape) {
            emit(Decision::Abort);
            return true;
        }

        // Space toggles the closest checkbox
        if (event == Event::Character(' ')) {
            if (state->focused_button == 1 || state->focused_button == 3)
                state->always_deny_checkbox  = !state->always_deny_checkbox;
            else
                state->always_allow_checkbox = !state->always_allow_checkbox;
            return true;
        }

        return false;
    });
}

// ============================================================
// Convenience overloads (most common variants)
// ============================================================

/// Build a bash-command permission prompt (the most common case).
[[nodiscard]] inline Component MakeBashPrompt(
    std::string command,
    std::optional<std::string> working_dir,
    RiskLevel risk,
    bool is_destructive,
    std::string destructive_reason,
    std::vector<std::string> affected_paths,
    std::function<void(Decision, bool sandbox)> on_decide)
{
    SinglePromptProps p;
    p.tool_name      = "BashTool";
    p.action_kind    = ActionKind::Execute;
    p.risk_level     = risk;
    p.description    = "BashTool wants to execute this command.";
    p.affected_paths = std::move(affected_paths);
    p.detail         = DetailBash{
        .command = std::move(command),
        .working_dir = std::move(working_dir),
        .is_destructive = is_destructive,
        .destructive_reason = std::move(destructive_reason),
    };
    p.on_decide = std::move(on_decide);
    return MakeSinglePromptDialog(std::move(p));
}

/// Build a file-edit permission prompt.
[[nodiscard]] inline Component MakeFileEditPrompt(
    std::string file_path,
    std::string old_snippet,
    std::string new_snippet,
    bool creates_file,
    RiskLevel risk,
    std::optional<std::string_view> workspace_root,
    std::function<void(Decision, bool sandbox)> on_decide)
{
    SinglePromptProps p;
    p.tool_name      = "FileEditTool";
    p.action_kind    = ActionKind::Write;
    p.risk_level     = risk;
    p.description    = creates_file
        ? "FileEditTool wants to create and edit a file."
        : "FileEditTool wants to modify a file.";
    p.affected_paths = { std::string{file_path} };
    if (workspace_root) p.workspace_root = std::string{*workspace_root};
    p.detail         = DetailFileEdit{
        .file_path = std::move(file_path),
        .old_snippet = std::move(old_snippet),
        .new_snippet = std::move(new_snippet),
        .creates_file = creates_file,
    };
    p.on_decide = std::move(on_decide);
    return MakeSinglePromptDialog(std::move(p));
}

/// Build a network / web-fetch permission prompt.
[[nodiscard]] inline Component MakeNetworkPrompt(
    std::string url_or_domain,
    bool is_upload,
    RiskLevel risk,
    std::function<void(Decision, bool sandbox)> on_decide)
{
    SinglePromptProps p;
    p.tool_name      = is_upload ? "WebFetchTool (POST)" : "WebFetchTool";
    p.action_kind    = ActionKind::Network;
    p.risk_level     = risk;
    p.description    = is_upload
        ? "WebFetchTool wants to send data over the network."
        : "WebFetchTool wants to fetch content from the network.";
    p.detail         = DetailNetwork{
        .url_or_domain = std::move(url_or_domain),
        .is_upload = is_upload,
    };
    p.on_decide = std::move(on_decide);
    return MakeSinglePromptDialog(std::move(p));
}

} // namespace cc::ui::permissions::single_prompt
