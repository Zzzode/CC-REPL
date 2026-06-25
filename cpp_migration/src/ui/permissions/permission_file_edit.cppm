/// =========================================================================
/// @file permission_file_edit.cppm
/// @brief Faithful C++/FTXUI port of TS FileEditPermissionRequest +
///        FileEditToolDiff + FilePermissionDialog.
///
/// MODULE:   cc.ui.permissions.permission_file_edit
/// LICENCE:  Exported.  Callers instantiate via MakeFileEditPermissionPrompt.
///
/// TS REFERENCE (3 files, ~380 lines total):
///   src/components/permissions/FileEditPermissionRequest/
///       FileEditPermissionRequest.tsx  — top-level component
///   src/components/FileEditToolDiff.tsx  — diff preview (shared)
///   src/components/permissions/FilePermissionDialog/
///       FilePermissionDialog.tsx         — shared dialog shell + options
///       permissionOptions.tsx            — option builder
///
/// FAITHFUL FEATURES (1:1 with TS, same pattern as FileWrite):
///   • Title: "Edit file"
///   • Subtitle: relative path (dimmed)
///   • Symlink warning banner (yellow) when path resolves through a symlink
///   • Content preview: structured diff via FileEditToolDiff (hunks)
///   • Question: "Do you want to make this edit to <filename>?"
///   • 3-option Select list (matches FilePermissionDialog Select):
///       1. Yes                (accept-once)
///       2. Yes, session-wide  (dynamic label based on path location)
///       3. No                 (reject)
///   • Feedback input modes (Tab / Shift+Tab toggle, "amend" pattern):
///       – Yes input: "and tell Claude what to do next"
///       – No input:  "and tell Claude what to do differently"
///   • .claude/ folder special session option (project + global)
///   • Worker badge (teammate permission requests)
///   • Bottom hints: "Esc to cancel · Tab to amend"
///
/// ENGINE:  100% delegated to callers via callbacks.  This module only
///          renders UI and routes keyboard input — no permission logic.
/// =========================================================================

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.permission_file_edit;

import cc.ui.components.file_edit_tool_diff;
import cc.ui.permissions.components;
import cc.utils.file_edit;

export namespace cc::ui::permissions::file_edit {

using namespace ftxui;
namespace pc = cc::ui::permissions::components;
namespace fed = cc::ui::components::file_edit_tool_diff;
namespace fe = cc::utils::file_edit;

// ─── Types ──────────────────────────────────────────────────────────────────

/// Decision returned by the prompt (1:1 with TS PermissionOption type).
enum class Decision : std::uint8_t {
    AllowOnce,    ///< Allow only this edit (accept-once)
    AllowSession, ///< Allow all edits session-wide (accept-session)
    Deny,         ///< Deny this edit (reject)
    Abort,        ///< Escape / abort the whole operation
};

/// Scope for session-level allow (mirrors TS accept-session scope).
enum class SessionScope : std::uint8_t {
    Default,          ///< Normal directory-wide session allow
    ClaudeFolder,     ///< Project .claude/ folder special case
    GlobalClaudeFolder, ///< Global ~/.claude/ folder special case
};

/// One selectable option in the dialog.
struct Option {
    std::string value{};            ///< Machine-readable identifier
    std::string label{};            ///< Display label
    std::string description{};      ///< Inline description (dimmed, below)
    Decision decision{Decision::Deny}; ///< Which decision this option triggers
    SessionScope scope{SessionScope::Default}; ///< For session options
    bool is_input = false;          ///< True when in feedback-input mode
    std::string input_value{};      ///< Current input text (if is_input)
    std::string input_placeholder{}; ///< Placeholder hint (if is_input)
};

/// Props for the file edit permission prompt.
struct FileEditPermissionProps {
    // ── File identity ──
    std::string file_path;           ///< Full path to the file
    std::string old_string;          ///< Old string to replace
    std::string new_string;          ///< New replacement string
    bool replace_all = false;        ///< Whether to replace all occurrences
    std::string file_content;        ///< Current file content (for diff)
    std::string relative_path;       ///< Path relative to cwd (subtitle)
    std::string filename;            ///< Basename (for question text)
    std::size_t max_visible_lines = 20; ///< Max diff lines before truncation
    std::optional<std::string> language; ///< Language for syntax (optional)

    // ── Context flags (drive dynamic UI) ──
    bool in_allowed_path = true;     ///< Path is inside working directory
    bool is_claude_folder = false;   ///< Inside project .claude/
    bool is_global_claude_folder = false; ///< Inside ~/.claude/
    std::optional<std::string> symlink_target; ///< Symlink target if any

    // ── Worker badge ──
    bool has_worker_badge = false;
    std::string worker_name;
    std::string worker_color;

    // ── Feedback input modes ──
    bool yes_input_mode = false;     ///< Yes option shows input field
    bool no_input_mode = false;      ///< No option shows input field
    std::string yes_feedback;        ///< Current yes-feedback text
    std::string no_feedback;         ///< Current no-feedback text

    // ── Callbacks ──
    std::function<void(Decision, SessionScope, std::string_view feedback)> on_decide;
    std::function<void()> on_abort;
    std::function<void(bool yes_mode, bool no_mode)> on_input_mode_change;
    std::function<void(std::string_view text)> on_yes_feedback_change;
    std::function<void(std::string_view text)> on_no_feedback_change;
};

// ─── Option builder ────────────────────────────────────────────────────────
//
// Same logic as FileWrite, but with edit-specific wording.
// Mirrors TS getFilePermissionOptions() with operationType='write'.

namespace detail {

/// Build option list from props.
inline std::vector<Option> build_options(
    const FileEditPermissionProps& p,
    std::string_view yes_feedback,
    std::string_view no_feedback)
{
    std::vector<Option> opts;

    // ── Option 1: Yes (accept-once) ──
    if (p.yes_input_mode) {
        opts.push_back({
            .value = "yes",
            .label = "Yes",
            .description = "",
            .decision = Decision::AllowOnce,
            .is_input = true,
            .input_value = std::string{yes_feedback},
            .input_placeholder = "and tell Claude what to do next",
        });
    } else {
        opts.push_back({
            .value = "yes",
            .label = "Yes",
            .description = "",
            .decision = Decision::AllowOnce,
        });
    }

    // ── Option 2: Session-wide accept (dynamic label) ──
    if ((p.is_claude_folder || p.is_global_claude_folder)) {
        opts.push_back({
            .value = "yes-claude-folder",
            .label = "Yes, and allow Claude to edit its own settings for this session",
            .description = "",
            .decision = Decision::AllowSession,
            .scope = p.is_global_claude_folder
                ? SessionScope::GlobalClaudeFolder
                : SessionScope::ClaudeFolder,
        });
    } else if (p.in_allowed_path) {
        opts.push_back({
            .value = "yes-session",
            .label = "Yes, allow all edits during this session",
            .description = "",
            .decision = Decision::AllowSession,
        });
    } else {
        // Outside working directory — include directory name
        std::string dir_name = "this directory";
        auto last_slash = p.file_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            auto prev_slash = p.file_path.find_last_of('/', last_slash - 1);
            if (prev_slash != std::string::npos) {
                dir_name = p.file_path.substr(prev_slash + 1, last_slash - prev_slash - 1);
            } else if (last_slash > 0) {
                dir_name = p.file_path.substr(0, last_slash);
            }
        }
        if (dir_name.empty()) dir_name = "this directory";

        opts.push_back({
            .value = "yes-session",
            .label = "Yes, allow all edits in " + dir_name + "/ during this session",
            .description = "",
            .decision = Decision::AllowSession,
        });
    }

    // ── Option 3: No (reject) ──
    if (p.no_input_mode) {
        opts.push_back({
            .value = "no",
            .label = "No",
            .description = "",
            .decision = Decision::Deny,
            .is_input = true,
            .input_value = std::string{no_feedback},
            .input_placeholder = "and tell Claude what to do differently",
        });
    } else {
        opts.push_back({
            .value = "no",
            .label = "No",
            .description = "",
            .decision = Decision::Deny,
        });
    }

    return opts;
}

/// Render one option row (Select-style, with ❯ marker and optional input).
[[nodiscard]] inline Element RenderOptionRow(const Option& opt, bool focused) {
    auto marker = focused
        ? text("❯ ") | color(Color::Cyan) | bold
        : text("  ");

    Element label_el = focused
        ? text(opt.label) | bold | color(Color::Cyan)
        : text(opt.label);

    Elements row_children;
    row_children.push_back(marker);

    if (opt.is_input) {
        std::string display_text = opt.input_value.empty()
            ? opt.input_placeholder
            : opt.input_value;
        bool is_placeholder = opt.input_value.empty();

        Element input_el = is_placeholder
            ? text(display_text) | dim | color(Color::GrayDark)
            : text(display_text) | color(Color::White);

        if (focused && !is_placeholder) {
            input_el = input_el | underlined;
        }

        row_children.push_back(vbox({
            label_el,
            hbox({ text("  "), input_el }),
        }));
    } else {
        row_children.push_back(label_el);
    }

    return hbox(std::move(row_children)) | xflex;
}

} // namespace detail

// ─── Content area ──────────────────────────────────────────────────────────
//
// Dashed-border box with structured diff via FileEditToolDiff.
// Mirrors TS FileEditToolDiff rendering.

/// Render the content preview area.
/// Uses the faithful FileEditToolDiff component (shared module).
[[nodiscard]] inline Element RenderContentBox(const FileEditPermissionProps& p) {
    std::vector<fe::FileEdit> edits;
    edits.push_back({
        .old_string = p.old_string,
        .new_string = p.new_string,
        .replace_all = p.replace_all,
    });

    fed::FileEditToolDiffProps diff_props{
        .file_path = p.file_path,
        .edits = std::move(edits),
    };

    return fed::render_file_edit_tool_diff(
        diff_props, p.file_content, /*terminal_width=*/80,
        /*placeholder=*/false);
}

// ─── Internal state ────────────────────────────────────────────────────────

struct PromptState {
    FileEditPermissionProps props;
    int focused_option = 0;
};

// ─── Renderer ──────────────────────────────────────────────────────────────
//
// Same layout pattern as FileWrite (faithful to FilePermissionDialog).

/// Render the complete file edit permission prompt.
[[nodiscard]] inline Element RenderFileEditPrompt(std::shared_ptr<PromptState> st) {
    const auto& p = st->props;
    auto options = detail::build_options(p, p.yes_feedback, p.no_feedback);

    // ── Title + subtitle ──
    auto title_el = text("Edit file") | bold | color(Color::Yellow);
    auto subtitle_el = text(p.relative_path) | dim;

    // Worker badge
    Element header_right = filler();
    if (p.has_worker_badge && !p.worker_name.empty()) {
        header_right = hbox({
            text(" ● ") | color(Color::RGB(80, 200, 120)),
            text(p.worker_name) | dim | color(Color::GrayLight),
        });
    }

    auto title_block = vbox({
        hbox({ title_el, filler(), header_right }),
        subtitle_el,
    });

    // ── Symlink warning ──
    Element symlink_warning = text("");
    if (p.symlink_target && !p.symlink_target->empty()) {
        std::string warning_text = "Symlink target: " + *p.symlink_target;
        symlink_warning = hbox({
            text("⚠ ") | color(Color::Yellow),
            text(warning_text) | color(Color::Yellow) | dim,
        });
    }

    // ── Content box (structured diff) ──
    auto content_box = RenderContentBox(p);

    // ── Question text ──
    auto question_el = hbox({
        text("Do you want to make this edit to "),
        text(p.filename) | bold,
        text("?"),
    });

    // ── Options list ──
    Elements option_els;
    for (size_t i = 0; i < options.size(); ++i) {
        bool focused = static_cast<int>(i) == st->focused_option;
        option_els.push_back(detail::RenderOptionRow(options[i], focused));
    }
    auto options_el = vbox(option_els);

    // ── Body assembly ──
    Elements body_els;
    body_els.push_back(text(""));
    body_els.push_back(title_block);
    body_els.push_back(pc::ThinDivider());

    if (p.symlink_target && !p.symlink_target->empty()) {
        body_els.push_back(text(""));
        body_els.push_back(symlink_warning);
    }

    body_els.push_back(text(""));
    body_els.push_back(content_box);
    body_els.push_back(text(""));

    // Question + options in paddingX=1 container
    Elements padded_els;
    padded_els.push_back(question_el);
    padded_els.push_back(text(""));
    padded_els.push_back(options_el);

    auto padded_box = vbox(std::move(padded_els));

    body_els.push_back(padded_box);
    body_els.push_back(text(""));

    auto body = vbox(std::move(body_els))
        | xflex
        | size(WIDTH, LESS_THAN, 80);

    // ── Outer frame ──
    auto dialog = window(
        text(" 🔒 Permission ") | bold | color(Color::Yellow),
        body | xflex
    ) | color(Color::Yellow);

    // ── Bottom hint bar ──
    bool show_amend_hint = false;
    if (st->focused_option >= 0 &&
        st->focused_option < static_cast<int>(options.size()))
    {
        const auto& opt = options[static_cast<size_t>(st->focused_option)];
        if ((opt.value == "yes" && !p.yes_input_mode) ||
            (opt.value == "no" && !p.no_input_mode))
        {
            show_amend_hint = true;
        }
    }

    Elements hint_els;
    hint_els.push_back(text("Esc") | color(Color::Cyan) | dim);
    hint_els.push_back(text(" to cancel") | dim);
    if (show_amend_hint) {
        hint_els.push_back(text("  ·  ") | dim);
        hint_els.push_back(text("Tab") | color(Color::Cyan) | dim);
        hint_els.push_back(text(" to amend") | dim);
    }

    auto footer_hint = hbox(std::move(hint_els)) | dim;

    return vbox({
        dialog,
        text(" "),
        hbox({ text("  "), footer_hint }),
    });
}

// ─── Public factory ────────────────────────────────────────────────────────

/// Create an interactive file edit permission prompt component.
///
/// Keyboard shortcuts (faithful to TS FilePermissionDialog + Select):
///   ↑ / ↓ / j / k  = Navigate options
///   Enter          = Confirm selected option
///   y / Y          = Quick accept (AllowOnce)
///   n / N          = Quick reject (Deny)
///   Tab            = Toggle feedback input mode on focused option
///   Esc            = Abort / cancel (exits input mode first)
///   Backspace      = Delete char in input mode
///   Printable chars = Type into focused input field
[[nodiscard]] inline Component MakeFileEditPermissionPrompt(
    FileEditPermissionProps props)
{
    auto state = std::make_shared<PromptState>();
    state->props = std::move(props);

    auto emit = [state](Decision d, SessionScope scope, std::string_view feedback) {
        if (state->props.on_decide) {
            state->props.on_decide(d, scope, feedback);
        }
    };

    return Renderer([state] { return RenderFileEditPrompt(state); })
         | CatchEvent([state, emit](Event event) -> bool
    {
        auto& p = state->props;
        auto options = detail::build_options(p, p.yes_feedback, p.no_feedback);
        int count = static_cast<int>(options.size());
        int& focus = state->focused_option;

        // Clamp focus
        if (focus >= count) focus = count - 1;
        if (focus < 0) focus = 0;

        const auto& current_opt = options[static_cast<size_t>(focus)];
        bool in_input_mode = current_opt.is_input;

        // ── Input-mode typing ──
        if (in_input_mode && event.is_character() &&
            event.character().size() == 1)
        {
            char c = event.character()[0];
            if (c >= 0x20 && c < 0x7F) {
                if (current_opt.value == "yes") {
                    p.yes_feedback += c;
                    if (p.on_yes_feedback_change) {
                        p.on_yes_feedback_change(p.yes_feedback);
                    }
                } else if (current_opt.value == "no") {
                    p.no_feedback += c;
                    if (p.on_no_feedback_change) {
                        p.on_no_feedback_change(p.no_feedback);
                    }
                }
                return true;
            }
        }

        // ── Backspace in input mode ──
        if (in_input_mode && event == Event::Backspace) {
            if (current_opt.value == "yes" && !p.yes_feedback.empty()) {
                p.yes_feedback.pop_back();
                if (p.on_yes_feedback_change) {
                    p.on_yes_feedback_change(p.yes_feedback);
                }
            } else if (current_opt.value == "no" && !p.no_feedback.empty()) {
                p.no_feedback.pop_back();
                if (p.on_no_feedback_change) {
                    p.on_no_feedback_change(p.no_feedback);
                }
            }
            return true;
        }

        // ── Navigation ──
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (event == Event::ArrowUp && in_input_mode) return false;
            focus = (focus - 1 + count) % count;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (event == Event::ArrowDown && in_input_mode) return false;
            focus = (focus + 1) % count;
            return true;
        }

        // ── Tab: toggle input mode on focused option ──
        if (event == Event::Tab) {
            if (current_opt.value == "yes") {
                p.yes_input_mode = !p.yes_input_mode;
                if (p.on_input_mode_change) {
                    p.on_input_mode_change(p.yes_input_mode, p.no_input_mode);
                }
                return true;
            }
            if (current_opt.value == "no") {
                p.no_input_mode = !p.no_input_mode;
                if (p.on_input_mode_change) {
                    p.on_input_mode_change(p.yes_input_mode, p.no_input_mode);
                }
                return true;
            }
            // For session option, Tab cycles to next
            focus = (focus + 1) % count;
            return true;
        }

        // ── Direct shortcuts ──
        if (event == Event::Character('y') || event == Event::Character('Y')) {
            for (int i = 0; i < count; ++i) {
                if (options[static_cast<size_t>(i)].decision == Decision::AllowOnce) {
                    focus = i;
                    break;
                }
            }
            emit(Decision::AllowOnce, SessionScope::Default, p.yes_feedback);
            return true;
        }
        if (event == Event::Character('n') || event == Event::Character('N')) {
            for (int i = 0; i < count; ++i) {
                if (options[static_cast<size_t>(i)].decision == Decision::Deny) {
                    focus = i;
                    break;
                }
            }
            emit(Decision::Deny, SessionScope::Default, p.no_feedback);
            return true;
        }

        // ── Enter: activate selected option ──
        if (event == Event::Return) {
            if (in_input_mode) {
                if (current_opt.value == "yes") {
                    emit(Decision::AllowOnce, SessionScope::Default, p.yes_feedback);
                } else if (current_opt.value == "no") {
                    emit(Decision::Deny, SessionScope::Default, p.no_feedback);
                }
            } else {
                const auto& opt = options[static_cast<size_t>(focus)];
                std::string feedback;
                if (opt.decision == Decision::AllowOnce && p.yes_input_mode) {
                    feedback = p.yes_feedback;
                } else if (opt.decision == Decision::Deny && p.no_input_mode) {
                    feedback = p.no_feedback;
                }
                emit(opt.decision, opt.scope, feedback);
            }
            return true;
        }

        // ── Escape: abort (exits input mode first) ──
        if (event == Event::Escape) {
            if (in_input_mode) {
                if (current_opt.value == "yes") {
                    p.yes_input_mode = false;
                } else if (current_opt.value == "no") {
                    p.no_input_mode = false;
                }
                if (p.on_input_mode_change) {
                    p.on_input_mode_change(p.yes_input_mode, p.no_input_mode);
                }
                return true;
            }
            if (p.on_abort) p.on_abort();
            emit(Decision::Abort, SessionScope::Default, "");
            return true;
        }

        return false;
    });
}

// ─── Convenience: simple yes/no prompt ────────────────────────────────────

/// Create a simple yes/no file edit prompt for quick prototyping or tests.
[[nodiscard]] inline Component MakeSimpleFileEditPrompt(
    std::string file_path,
    std::string old_string,
    std::string new_string,
    std::string file_content,
    std::function<void(bool allowed)> on_result)
{
    FileEditPermissionProps props;
    props.file_path = std::move(file_path);
    props.old_string = std::move(old_string);
    props.new_string = std::move(new_string);
    props.replace_all = false;
    props.file_content = std::move(file_content);
    props.relative_path = props.file_path;
    props.filename = props.file_path;

    auto last_slash = props.file_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        props.filename = props.file_path.substr(last_slash + 1);
    }

    props.on_decide = [on_result = std::move(on_result)](
        Decision d, SessionScope, std::string_view)
    {
        if (on_result) {
            on_result(d == Decision::AllowOnce || d == Decision::AllowSession);
        }
    };
    props.on_abort = [on_result] {
        if (on_result) on_result(false);
    };

    return MakeFileEditPermissionPrompt(std::move(props));
}

// ─── Convenience: static render for golden snapshot tests ──────────────────

/// Render a static file edit permission prompt for golden snapshot tests.
/// Builds the props internally and returns the rendered Element.
[[nodiscard]] inline Element RenderFileEditPromptForTest(
    std::string_view file_path,
    std::string_view old_string,
    std::string_view new_string,
    std::string_view file_content,
    std::string_view relative_path = "",
    int selected = 0)
{
    FileEditPermissionProps props;
    props.file_path = std::string{file_path};
    props.old_string = std::string{old_string};
    props.new_string = std::string{new_string};
    props.replace_all = false;
    props.file_content = std::string{file_content};
    props.relative_path = relative_path.empty()
        ? std::string{file_path}
        : std::string{relative_path};

    // Extract basename
    auto pos = file_path.find_last_of("/\\");
    props.filename = (pos == std::string_view::npos)
        ? std::string{file_path}
        : std::string{file_path.substr(pos + 1)};

    auto state = std::make_shared<PromptState>();
    state->props = std::move(props);
    state->focused_option = selected;

    return RenderFileEditPrompt(state);
}

} // namespace cc::ui::permissions::file_edit
