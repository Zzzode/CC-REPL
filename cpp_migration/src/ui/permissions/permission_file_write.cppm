/// =========================================================================
/// @file permission_file_write.cppm
/// @brief Faithful C++/FTXUI port of TS FileWritePermissionRequest +
///        FileWriteToolDiff + FilePermissionDialog.
///
/// MODULE:   cc.ui.permissions.permission_file_write
/// LICENCE:  Exported.  Callers instantiate via MakeFileWritePermissionPrompt.
///
/// TS REFERENCE (3 files, ~450 lines total):
///   src/components/permissions/FileWritePermissionRequest/
///       FileWritePermissionRequest.tsx  — top-level component
///       FileWriteToolDiff.tsx            — diff/code preview
///   src/components/permissions/FilePermissionDialog/
///       FilePermissionDialog.tsx         — shared dialog shell + options
///       permissionOptions.tsx            — option builder
///
/// FAITHFUL FEATURES (1:1 with TS):
///   • Title: "Create file" / "Overwrite file" (depends on file_exists)
///   • Subtitle: relative path (dimmed)
///   • Symlink warning banner (yellow) when path resolves through a symlink
///   • Content preview:
///       – New file:  syntax-highlighted code
///       – Overwrite: unified diff (red deletions, green additions)
///   • Question: "Do you want to {create|overwrite} <filename>?"
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
#include <format>
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

export module cc.ui.permissions.permission_file_write;

import cc.ui.code_highlight;
import cc.ui.permissions.components;

export namespace cc::ui::permissions::file_write {

using namespace ftxui;
namespace pc = cc::ui::permissions::components;
namespace ch = cc::ui::code_highlight;

// ─── Types ──────────────────────────────────────────────────────────────────

/// Decision returned by the prompt (1:1 with TS PermissionOption type).
enum class Decision : std::uint8_t {
    AllowOnce,    ///< Allow only this write (accept-once)
    AllowSession, ///< Allow all writes session-wide (accept-session)
    Deny,         ///< Deny this write (reject)
    Abort,        ///< Escape / abort the whole operation
};

/// Scope for session-level allow (mirrors TS accept-session scope).
enum class SessionScope : std::uint8_t {
    Default,          ///< Normal directory-wide session allow
    ClaudeFolder,     ///< Project .claude/ folder special case
    GlobalClaudeFolder, ///< Global ~/.claude/ folder special case
};

/// One selectable option in the dialog.  Mirrors TS `OptionWithDescription`
/// plus the `type: 'input'` variant used in feedback mode.
struct Option {
    std::string value;              ///< Machine-readable identifier
    std::string label;              ///< Display label (may be rich text)
    std::string description;        ///< Inline description (dimmed, below)
    Decision decision;              ///< Which decision this option triggers
    SessionScope scope{SessionScope::Default}; ///< For session options
    bool is_input = false;          ///< True when in feedback-input mode
    std::string input_value;        ///< Current input text (if is_input)
    std::string input_placeholder;  ///< Placeholder hint (if is_input)
};

/// Props for the file write permission prompt.
///
/// Every field has a default so callers can build incrementally.
/// Fields that drive dynamic UI (symlink, claude-folder, in-allowed-path)
/// are computed by the caller and passed in — this module is UI-only.
struct FileWritePermissionProps {
    // ── File identity ──
    std::string file_path;           ///< Full path to the file
    std::string content;             ///< New content to write
    std::string old_content;         ///< Old content (empty = new file)
    bool file_exists = false;        ///< True if overwriting

    // ── Display labels ──
    std::string relative_path;       ///< Path relative to cwd (subtitle)
    std::string filename;            ///< Basename (for question text)
    std::optional<std::string> language; ///< Language for syntax highlighting
    std::size_t max_visible_lines = 20;  ///< Max preview lines

    // ── Context flags (drive dynamic UI) ──
    bool in_allowed_path = true;     ///< Path is inside working directory
    bool is_claude_folder = false;   ///< Inside project .claude/
    bool is_global_claude_folder = false; ///< Inside ~/.claude/
    std::optional<std::string> symlink_target; ///< Symlink target if any

    // ── Worker badge (teammate requests) ──
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

// ─── Internal: Diff computation (LCS-based) ────────────────────────────────
//
// Kept inline here because it's small and self-contained; shared diff
// utilities live in cc.ui.components.file_edit_tool_diff but that module
// is geared toward edit-hunk style diffs.  For file-write we do a full
// unified diff of old vs new content.

namespace detail {

inline std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty()) lines.emplace_back("");
    return lines;
}

inline std::vector<std::pair<int, int>> compute_lcs(
    const std::vector<std::string>& old_lines,
    const std::vector<std::string>& new_lines)
{
    int m = static_cast<int>(old_lines.size());
    int n = static_cast<int>(new_lines.size());
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            if (old_lines[i - 1] == new_lines[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
            }
        }
    }
    std::vector<std::pair<int, int>> matches;
    int i = m, j = n;
    while (i > 0 && j > 0) {
        if (old_lines[i - 1] == new_lines[j - 1]) {
            matches.emplace_back(i - 1, j - 1);
            --i; --j;
        } else if (dp[i - 1][j] > dp[i][j - 1]) {
            --i;
        } else {
            --j;
        }
    }
    std::reverse(matches.begin(), matches.end());
    return matches;
}

/// Render a unified diff as FTXUI Elements (one per line).
/// Matches TS StructuredDiff visual style:
///   - Deletions: red, prefixed with "- "
///   - Additions: green, prefixed with "+ "
///   - Context: dim, prefixed with "  "
///   - Truncation: "… +N more lines" dim indicator
inline Elements render_diff_elements(
    std::string_view old_content,
    std::string_view new_content,
    int max_lines)
{
    auto old_lines = split_lines(old_content);
    auto new_lines = split_lines(new_content);
    auto matches = compute_lcs(old_lines, new_lines);

    Elements lines;
    int old_idx = 0, new_idx = 0;
    size_t match_idx = 0;
    int visible = 0;

    auto add_line = [&](Element el) {
        if (visible >= max_lines) return;
        lines.push_back(std::move(el));
        ++visible;
    };

    while (old_idx < static_cast<int>(old_lines.size()) ||
           new_idx < static_cast<int>(new_lines.size()))
    {
        if (match_idx < matches.size()) {
            auto [mo, mn] = matches[match_idx];
            // Deletions
            while (old_idx < mo && visible < max_lines) {
                add_line(hbox({
                    text("- ") | color(Color::Red),
                    text(old_lines[old_idx]) | color(Color::Red),
                }));
                ++old_idx;
            }
            // Additions
            while (new_idx < mn && visible < max_lines) {
                add_line(hbox({
                    text("+ ") | color(Color::Green),
                    text(new_lines[new_idx]) | color(Color::Green),
                }));
                ++new_idx;
            }
            // Context line (match)
            if (visible < max_lines) {
                add_line(hbox({
                    text("  "),
                    text(old_lines[old_idx]) | dim,
                }));
                ++old_idx;
                ++new_idx;
                ++match_idx;
            }
        } else {
            // Remaining deletions
            while (old_idx < static_cast<int>(old_lines.size()) && visible < max_lines) {
                add_line(hbox({
                    text("- ") | color(Color::Red),
                    text(old_lines[old_idx]) | color(Color::Red),
                }));
                ++old_idx;
            }
            // Remaining additions
            while (new_idx < static_cast<int>(new_lines.size()) && visible < max_lines) {
                add_line(hbox({
                    text("+ ") | color(Color::Green),
                    text(new_lines[new_idx]) | color(Color::Green),
                }));
                ++new_idx;
            }
            break;
        }
    }

    // Truncation indicator
    int total_old = static_cast<int>(old_lines.size());
    int total_new = static_cast<int>(new_lines.size());
    int total = std::max(total_old, total_new);
    if (visible < total) {
        lines.push_back(
            text(std::format("… +{} more lines", total - visible))
                | dim | color(Color::GrayLight)
        );
    }

    return lines;
}

/// Render syntax-highlighted code preview.
inline Element render_highlighted_code(
    std::string_view content,
    std::string_view file_path,
    std::optional<std::string_view> language,
    int max_lines)
{
    std::string lang;
    if (language && !language->empty()) {
        lang = std::string{*language};
    } else {
        auto dot = file_path.find_last_of('.');
        if (dot != std::string_view::npos) {
            lang = std::string{file_path.substr(dot + 1)};
        }
    }

    ch::CodeHighlightOptions opts;
    opts.source = content.empty() ? "(No content)" : std::string(content);
    opts.language = lang;
    opts.show_line_numbers = false;
    opts.visible_lines = max_lines;

    auto result = ch::highlight_source(opts.source, opts.language);
    return ch::RenderCodeHighlight(opts, result);
}

} // namespace detail

// ─── Content area ──────────────────────────────────────────────────────────
//
// Dashed top+bottom border box with either a unified diff (overwrite) or
// syntax-highlighted code (new file).  Mirrors TS FileWriteToolDiff.tsx.

/// Render the content preview area.
/// Matches TS FileWriteToolDiff.tsx dashed-border Box.
[[nodiscard]] inline Element RenderContentBox(const FileWritePermissionProps& p) {
    Element content_el;

    if (p.file_exists && !p.old_content.empty()) {
        // Overwrite: show unified diff
        auto diff_lines = detail::render_diff_elements(
            p.old_content, p.content,
            static_cast<int>(p.max_visible_lines));
        content_el = vbox(diff_lines) | xflex;
    } else {
        // New file: show syntax-highlighted code
        std::optional<std::string_view> lang = std::nullopt;
        if (p.language) lang = *p.language;
        content_el = detail::render_highlighted_code(
            p.content, p.file_path, lang,
            static_cast<int>(p.max_visible_lines)) | xflex;
    }

    // Dashed top+bottom border (subtle) — matches TS:
    //   borderStyle="dashed", borderColor="subtle",
    //   borderLeft={false}, borderRight={false}, paddingX={1}
    auto dashed_top = separatorDouble() | color(Color::GrayDark);
    auto dashed_bottom = separatorDouble() | color(Color::GrayDark);

    return vbox({
        dashed_top,
        content_el | size(HEIGHT, GREATER_THAN, 3),
        dashed_bottom,
    }) | xflex;
}

// ─── Option builder ────────────────────────────────────────────────────────
//
// Builds the 3-option list dynamically based on path context.
// Mirrors TS getFilePermissionOptions() in permissionOptions.tsx.

namespace detail {

/// Build option list from props.  Mirrors TS getFilePermissionOptions().
/// Caller passes feedback text for input-mode options.
inline std::vector<Option> build_options(
    const FileWritePermissionProps& p,
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
    //
    // TS logic:
    //   • In .claude/ folder → special "edit its own settings" label
    //   • In allowed path → generic "allow all edits during this session"
    //   • Outside allowed path → includes directory name

    if ((p.is_claude_folder || p.is_global_claude_folder)) {
        // Special .claude folder session option
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
        // Inside working directory — generic label
        opts.push_back({
            .value = "yes-session",
            .label = "Yes, allow all writes during this session",
            .description = "",
            .decision = Decision::AllowSession,
        });
    } else {
        // Outside working directory — include directory name
        // Extract directory name from file_path
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
            .label = "Yes, allow all writes in " + dir_name + "/ during this session",
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
/// Mirrors TS CustomSelect option rendering + input variant.
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
        // Input option: label on first line, input field below
        // TS shows the label + a text input with placeholder
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

// ─── Internal state ────────────────────────────────────────────────────────

struct PromptState {
    FileWritePermissionProps props;
    int focused_option = 0;
};

// ─── Renderer ──────────────────────────────────────────────────────────────
//
// Layout mirrors TS FilePermissionDialog.tsx (from outside in):
//   PermissionDialog (colored frame, title, subtitle, worker badge)
//     ├── symlink warning (optional)
//     ├── content (diff / code)
//     └── paddingX=1 box:
//           ├── question
//           └── Select (options)
//   Bottom hint bar (outside dialog):
//     "Esc to cancel · Tab to amend"

/// Render the complete file write permission prompt.
[[nodiscard]] inline Element RenderFileWritePrompt(std::shared_ptr<PromptState> st) {
    const auto& p = st->props;
    auto options = detail::build_options(p, p.yes_feedback, p.no_feedback);

    // ── Title + subtitle (matches PermissionRequestTitle) ──
    std::string title_text = p.file_exists ? "Overwrite file" : "Create file";
    auto title_el = text(title_text) | bold | color(Color::Yellow);
    auto subtitle_el = text(p.relative_path) | dim;

    // Worker badge (if present, shows to the right of title)
    // TS: workerBadge prop → small colored badge with worker name
    Element header_right = filler();
    if (p.has_worker_badge && !p.worker_name.empty()) {
        header_right = hbox({
            text(" ● ") | color(Color::RGB(80, 200, 120)), // green dot
            text(p.worker_name) | dim | color(Color::GrayLight),
        });
    }

    auto title_block = vbox({
        hbox({ title_el, filler(), header_right }),
        subtitle_el,
    });

    // ── Symlink warning (yellow, matches TS symlinkWarning) ──
    Element symlink_warning = text("");
    if (p.symlink_target && !p.symlink_target->empty()) {
        // TS: "This will modify {target} (outside working directory) via a symlink"
        // or "Symlink target: {target}" if inside cwd
        std::string warning_text = "Symlink target: " + *p.symlink_target;
        symlink_warning = hbox({
            text("⚠ ") | color(Color::Yellow),
            text(warning_text) | color(Color::Yellow) | dim,
        });
    }

    // ── Content box (diff or highlighted code) ──
    auto content_box = RenderContentBox(p);

    // ── Question text ──
    std::string action_text = p.file_exists ? "overwrite" : "create";
    auto question_el = hbox({
        text("Do you want to " + action_text + " "),
        text(p.filename) | bold,
        text("?"),
    });

    // ── Options list (Select component style) ──
    Elements option_els;
    for (size_t i = 0; i < options.size(); ++i) {
        bool focused = static_cast<int>(i) == st->focused_option;
        option_els.push_back(detail::RenderOptionRow(options[i], focused));
    }
    auto options_el = vbox(option_els);

    // ── Body assembly (paddingX=1 like TS inner box) ──
    Elements body_els;
    body_els.push_back(text(""));  // top padding
    body_els.push_back(title_block);
    body_els.push_back(pc::ThinDivider());

    // Symlink warning (if present)
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

    // ── Outer frame: permission-colored window ──
    // Matches PermissionDialog.tsx borderColor="permission" top border
    auto dialog = window(
        text(" 🔒 Permission ") | bold | color(Color::Yellow),
        body | xflex
    ) | color(Color::Yellow);

    // ── Bottom hint bar (outside dialog, matches TS) ──
    // TS: "Esc to cancel · Tab to amend" — amend shown when focused on
    // yes/no option and not currently in input mode.
    bool show_amend_hint = false;
    if (st->focused_option >= 0 &&
        st->focused_option < static_cast<int>(options.size()))
    {
        const auto& opt = options[static_cast<size_t>(st->focused_option)];
        // Show amend hint on yes/no options that are NOT in input mode
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
        text(" "),   // small gap (matches marginTop=1)
        hbox({ text("  "), footer_hint }), // paddingX=1
    });
}

// ─── Public factory ────────────────────────────────────────────────────────

/// Create an interactive file write permission prompt component.
///
/// Keyboard shortcuts (faithful to TS FilePermissionDialog + Select):
///   ↑ / ↓ / j / k  = Navigate options
///   Enter           = Confirm selected option
///   y / Y          = Quick accept (AllowOnce)
///   n / N          = Quick reject (Deny)
///   Tab            = Toggle feedback input mode on focused option
///   Shift+Tab      = Cycle backward through input modes
///   Esc            = Abort / cancel
///   Backspace      = Delete char in input mode
///   Printable chars = Type into focused input field
///
/// Mirrors TS useFilePermissionDialog + Select keyboard handling.
[[nodiscard]] inline Component MakeFileWritePermissionPrompt(
    FileWritePermissionProps props)
{
    auto state = std::make_shared<PromptState>();
    state->props = std::move(props);

    auto emit = [state](Decision d, SessionScope scope, std::string_view feedback) {
        if (state->props.on_decide) {
            state->props.on_decide(d, scope, feedback);
        }
    };

    return Renderer([state] { return RenderFileWritePrompt(state); })
         | CatchEvent([state, emit](Event event) -> bool
    {
        auto& p = state->props;
        auto options = detail::build_options(p, p.yes_feedback, p.no_feedback);
        int count = static_cast<int>(options.size());
        int& focus = state->focused_option;

        // Clamp focus in case options changed
        if (focus >= count) focus = count - 1;
        if (focus < 0) focus = 0;

        const auto& current_opt = options[static_cast<size_t>(focus)];
        bool in_input_mode = current_opt.is_input;

        // ── Input-mode typing (printable characters) ──
        if (in_input_mode && event.is_character() &&
            event.character().size() == 1)
        {
            char c = event.character()[0];
            // Only accept printable ASCII (excluding control chars)
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
            // Don't navigate out of input mode with arrows — only j/k
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
        // TS uses Shift+Tab for cycleMode; we follow the same pattern.
        // Tab = toggle input mode on current option (if it's yes/no)
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
            // For session option, Tab cycles to next option
            // (mirrors TS behavior where Tab cycles through select options)
            focus = (focus + 1) % count;
            return true;
        }

        // ── Direct shortcuts ──
        if (event == Event::Character('y') || event == Event::Character('Y')) {
            // Select the first "yes" option and activate it
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
                // Submit with feedback
                if (current_opt.value == "yes") {
                    emit(Decision::AllowOnce, SessionScope::Default, p.yes_feedback);
                } else if (current_opt.value == "no") {
                    emit(Decision::Deny, SessionScope::Default, p.no_feedback);
                }
            } else {
                const auto& opt = options[static_cast<size_t>(focus)];
                // For accept-once / reject, pass empty feedback
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

        // ── Escape: abort ──
        if (event == Event::Escape) {
            // If in input mode, Esc exits input mode first (TS behavior)
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

/// Create a simple yes/no file write prompt for quick prototyping or tests.
/// No feedback modes, no session options — just AllowOnce / Deny.
[[nodiscard]] inline Component MakeSimpleFileWritePrompt(
    std::string file_path,
    std::string content,
    bool file_exists,
    std::string old_content,
    std::function<void(bool allowed)> on_result)
{
    FileWritePermissionProps props;
    props.file_path = file_path;
    props.content = std::move(content);
    props.file_exists = file_exists;
    props.old_content = std::move(old_content);
    props.relative_path = file_path;
    props.filename = file_path;

    // Extract basename
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

    return MakeFileWritePermissionPrompt(std::move(props));
}

} // namespace cc::ui::permissions::file_write
