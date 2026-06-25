/// @file permission_bash.cppm
/// @brief Faithful C++/FTXUI port of TS BashPermissionRequest.tsx
///
/// MODULE:   cc.ui.permissions.permission_bash
/// LICENCE:  Exported.  Imported by permission routing & test code.
///
/// TS REFERENCE: src/components/permissions/BashPermissionRequest/BashPermissionRequest.tsx
///               + bashToolUseOptions.tsx
///               + PermissionDialog.tsx
///
/// LAYOUT (faithful to TS):
///   ┌─ Bash command ───────────────────────────────────────┐
///   │ {subtitle: classifier status / auto-approved / ...}  │
///   │                                                      │
///   │  $ ls -la src/                                       │
///   │  {description, dimmed}                               │
///   │  {PermissionExplainer, ctrl+e toggled}               │
///   │                                                      │
///   │  {PermissionRuleExplanation — "why this rule"}       │
///   │  {destructive-warning banner (yellow, if applicable)}│
///   │  Do you want to proceed?                             │
///   │  ❯ Yes                                               │
///   │    Yes, and don't ask again for ls:*                 │
///   │    No                                                │
///   │                                                      │
///   │  Esc to cancel · Tab to amend · ctrl+e to explain    │
///   └──────────────────────────────────────────────────────┘
///
/// INTERACTIONS (faithful to TS):
///   - ArrowUp / ArrowDown / j / k : move selection
///   - Enter / y : accept selected option
///   - n         : reject (select "No" and activate)
///   - a         : toggle "always" / select the prefix option
///   - Esc       : cancel / abort
///   - Tab       : toggle input/feedback mode for focused option
///   - Ctrl+E    : toggle permission explainer
///   - Ctrl+D    : toggle debug info (when debug enabled)
///
/// Engine logic is 100% delegated to cc.tools.bash_permissions and
/// cc.utils.permissions_engine — this file only renders the UI and
/// routes user input.
module;

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

export module cc.ui.permissions.permission_bash;

import cc.ui.permissions.components;
import cc.ui.permissions.permission_shell_helpers;

export namespace cc::ui::permissions::bash_prompt {

using namespace ftxui;
namespace pc = cc::ui::permissions::components;

// ============================================================
// Types
// ============================================================

/// Decision returned when the user makes a choice.
enum class Decision : std::uint8_t {
    AllowOnce,          ///< Allow only this invocation  ("yes")
    AllowWithPrefix,    ///< Allow + add a prefix rule   ("yes-prefix-edited" / "yes-apply-suggestions")
    Deny,               ///< Deny this invocation        ("no")
    Abort,              ///< Escape / abort the whole operation
};

/// A single selectable option in the dialog.
struct Option {
    std::string value;       ///< Machine-readable key (e.g. "yes", "no", "yes-prefix-edited")
    std::string label;       ///< Display label
    std::optional<std::string> description;  ///< Inline description shown under the label
    bool is_input = false;   ///< True if this option has an editable text input
    std::string input_value; ///< Current value of the input (when is_input=true)
    std::string input_placeholder;  ///< Placeholder for the input
};

/// Props for constructing a bash permission prompt.
struct BashPromptProps {
    // -- Content --
    std::string command;                 ///< The bash command to run
    std::optional<std::string> description;  ///< Tool description (from toolUseConfirm)
    std::optional<std::string> working_dir;  ///< Working directory

    // -- Rule / risk context --
    std::string rule_explanation;        ///< Why permission is being asked (may be empty)
    std::vector<std::string> matched_rules;  ///< Matched permission rule labels
    bool is_destructive = false;         ///< Whether command is destructive
    std::string destructive_reason;      ///< Reason for destructive flag

    // -- Sandbox context --
    bool sandboxing_enabled = false;     ///< Whether sandboxing is available at all
    bool is_sandboxed = false;           ///< Whether this specific command would be sandboxed

    // -- Classifier / auto-approve status (optional) --
    enum class ClassifierState {
        None,           ///< Classifier not running / not available
        Checking,       ///< Classifier check in progress
        AutoApproved,   ///< Classifier auto-approved
        ManualRequired, ///< Classifier ran but requires manual approval
    };
    ClassifierState classifier_state = ClassifierState::None;
    std::string classifier_matched_rule;  ///< Rule that matched (when AutoApproved)

    // -- Suggestions / always-allow --
    bool show_always_allow = true;        ///< Whether to show "don't ask again" options
    std::string editable_prefix;          ///< Prefix for the editable "always allow" option
    bool has_suggestions = false;         ///< Whether backend suggestions exist
    std::string suggestions_label;        ///< Label for the suggestions option

    // -- Feedback / input modes --
    bool yes_feedback_mode = false;       ///< Whether "Yes" has a feedback input
    bool no_feedback_mode = false;        ///< Whether "No" has a feedback input
    std::string yes_feedback_text;        ///< Current yes-feedback text
    std::string no_feedback_text;         ///< Current no-feedback text

    // -- Explainer --
    bool explainer_enabled = false;       ///< Whether explainer is available
    bool explainer_visible = false;       ///< Whether explainer is currently shown
    std::string explainer_text;           ///< Explainer content (may be loading)

    // -- Debug --
    bool debug_mode = false;              ///< Whether debug info is available
    bool debug_visible = false;           ///< Whether debug panel is shown
    std::string debug_text;               ///< Debug info text

    // -- Worker badge (optional) --
    bool has_worker_badge = false;
    std::string worker_name;
    std::string worker_color;

    // -- Callbacks --
    std::function<void(Decision, std::string_view feedback, std::string_view prefix_rule)> on_decide;
    std::function<void()> on_abort;
    std::function<void(bool visible)> on_explainer_toggle;
    std::function<void(std::string_view new_prefix)> on_prefix_change;
};

// ============================================================
// Internal state
// ============================================================

namespace detail {

/// Mutable state shared between Renderer and CatchEvent.
struct PromptState {
    BashPromptProps props;
    int selected_index = 0;        ///< Currently selected option index
    bool explainer_visible = false;
    bool debug_visible = false;
    std::string editable_prefix;   ///< Live-edited prefix (mirrors prop until edited)
    std::string yes_feedback;      ///< Live yes-feedback text
    std::string no_feedback;       ///< Live no-feedback text
};

// ============================================================
// Options list (mirrors bashToolUseOptions.tsx)
// ============================================================

/// Build the list of selectable options from props.
/// Faithful to bashToolUseOptions() in bashToolUseOptions.tsx.
[[nodiscard]] inline std::vector<Option> BuildOptions(const PromptState& st) {
    std::vector<Option> opts;
    const auto& p = st.props;

    // --- Yes option ---
    Option yes_opt;
    yes_opt.value = "yes";
    yes_opt.label = "Yes";
    yes_opt.is_input = p.yes_feedback_mode;
    yes_opt.input_value = st.yes_feedback;
    yes_opt.input_placeholder = "and tell Claude what to do next";
    opts.push_back(std::move(yes_opt));

    // --- Always-allow options (when enabled) ---
    if (p.show_always_allow) {
        if (!p.editable_prefix.empty()) {
            // Editable prefix option (like TS "yes-prefix-edited")
            Option prefix_opt;
            prefix_opt.value = "yes-prefix-edited";
            prefix_opt.label = "Yes, and don't ask again for";
            prefix_opt.is_input = true;
            prefix_opt.input_value = st.editable_prefix;
            prefix_opt.input_placeholder = "command prefix (e.g., npm run:*)";
            opts.push_back(std::move(prefix_opt));
        } else if (p.has_suggestions && !p.suggestions_label.empty()) {
            // Suggestions-based option (like TS "yes-apply-suggestions")
            Option suggest_opt;
            suggest_opt.value = "yes-apply-suggestions";
            suggest_opt.label = p.suggestions_label;
            opts.push_back(std::move(suggest_opt));
        }
    }

    // --- No option ---
    Option no_opt;
    no_opt.value = "no";
    no_opt.label = "No";
    no_opt.is_input = p.no_feedback_mode;
    no_opt.input_value = st.no_feedback;
    no_opt.input_placeholder = "and tell Claude what to do differently";
    opts.push_back(std::move(no_opt));

    return opts;
}

// ============================================================
// Rendering helpers
// ============================================================

/// Render the title bar — "Bash command" or "Bash command (unsandboxed)"
/// Faithful to TS PermissionDialog title logic.
[[nodiscard]] inline Element RenderTitle(const BashPromptProps& p) {
    std::string title = "Bash command";
    if (p.sandboxing_enabled && !p.is_sandboxed) {
        title += " (unsandboxed)";
    }
    return text(title) | bold;
}

/// Render the subtitle line — classifier status / auto-approved / etc.
/// Faithful to TS classifierSubtitle logic.
[[nodiscard]] inline Element RenderSubtitle(const BashPromptProps& p) {
    switch (p.classifier_state) {
        case BashPromptProps::ClassifierState::AutoApproved: {
            Elements parts;
            parts.push_back(hbox({
                text("✓ ") | color(Color::Green) | bold,
                text("Auto-approved") | color(Color::Green),
            }));
            if (!p.classifier_matched_rule.empty()) {
                parts.push_back(text("  "));
                parts.push_back(hbox({
                    text("· matched \"") | dim,
                    text(p.classifier_matched_rule) | dim,
                    text("\"") | dim,
                }));
            }
            return hbox(parts);
        }
        case BashPromptProps::ClassifierState::Checking:
            return hbox({
                text("…") | color(Color::Yellow),
                text(" Attempting to auto-approve") | dim,
            });
        case BashPromptProps::ClassifierState::ManualRequired:
            return text("Requires manual approval") | dim;
        case BashPromptProps::ClassifierState::None:
            return text("");
    }
    return text("");
}

/// Render the command preview with "$ " prefix.
/// Faithful to TS BashTool.renderToolUseMessage for display.
[[nodiscard]] inline Element RenderCommand(const BashPromptProps& p, bool dimmed) {
    const auto display_cmd = truncate_command(p.command, 200);
    auto cmd_el = hbox({
        text("$ ") | bold | color(Color::Yellow),
        text(display_cmd) | color(Color::Yellow),
    });
    if (dimmed) {
        cmd_el = cmd_el | dim;
    }
    return cmd_el;
}

/// Render the description line.
/// Faithful to TS {toolUseConfirm.description} display.
[[nodiscard]] inline Element RenderDescription(const BashPromptProps& p) {
    if (!p.description || p.description->empty()) return text("");
    return text(*p.description) | dim;
}

/// Render the permission explainer content.
/// Faithful to TS PermissionExplainerContent.
[[nodiscard]] inline Element RenderExplainer(const BashPromptProps& p, bool visible) {
    if (!visible) return text("");
    return vbox({
        text("") | size(HEIGHT, EQUAL, 1),
        paragraph(p.explainer_text.empty() ? "Loading explanation…" : p.explainer_text)
            | color(Color::Cyan) | dim,
    });
}

/// Render the rule explanation section.
/// Faithful to TS PermissionRuleExplanation.
[[nodiscard]] inline Element RenderRuleExplanation(const BashPromptProps& p) {
    Elements els;
    if (!p.rule_explanation.empty()) {
        els.push_back(text(p.rule_explanation) | dim);
    }
    if (!p.matched_rules.empty()) {
        for (const auto& r : p.matched_rules) {
            els.push_back(hbox({
                text("  • ") | dim,
                text(r) | dim | color(Color::Cyan),
            }));
        }
    }
    if (els.empty()) return text("");
    return vbox(els);
}

/// Render the destructive warning banner.
/// Faithful to TS destructive command warning display.
[[nodiscard]] inline Element RenderDestructiveWarning(const BashPromptProps& p) {
    if (!p.is_destructive) return text("");
    const auto& reason = p.destructive_reason.empty()
        ? std::string{"This command may permanently delete or overwrite files."}
        : p.destructive_reason;
    return pc::DestructiveWarningBanner(reason);
}

/// Render a single option row.
/// Faithful to TS Select option rendering.
[[nodiscard]] inline Element RenderOptionRow(const Option& opt, bool selected) {
    auto marker = selected
        ? text("❯ ") | color(Color::Cyan) | bold
        : text("  ");

    auto label_text = selected
        ? text(opt.label) | bold
        : text(opt.label);

    if (opt.is_input && !opt.input_value.empty()) {
        // Show label + current value (like TS showLabelWithValue)
        auto value_el = selected
            ? text(opt.input_value) | color(Color::Green)
            : text(opt.input_value) | dim;
        auto row = hbox({
            marker,
            label_text,
            text(": "),
            value_el,
        });
        if (selected) row = row | color(Color::Cyan);
        return row;
    }
    if (opt.is_input && selected) {
        // Input mode, empty value — show placeholder
        auto row = hbox({
            marker,
            label_text,
            text(": "),
            text(opt.input_placeholder) | dim,
            text("▌") | color(Color::Cyan), // cursor
        });
        return row;
    }

    auto row = hbox({ marker, label_text });
    if (selected) row = row | color(Color::Cyan);

    // Inline description (if any and not an input)
    if (opt.description && !opt.is_input) {
        row = vbox({
            row,
            hbox({ text("  "), text(*opt.description) | dim }),
        });
    }

    return row;
}

/// Render the full options list.
[[nodiscard]] inline Element RenderOptionsList(const std::vector<Option>& opts, int selected) {
    Elements rows;
    for (std::size_t i = 0; i < opts.size(); ++i) {
        rows.push_back(RenderOptionRow(opts[i], static_cast<int>(i) == selected));
    }
    return vbox(rows);
}

/// Render the bottom keyboard hint row.
/// Faithful to TS bottom hint: "Esc to cancel · Tab to amend · ctrl+e to explain"
[[nodiscard]] inline Element RenderKeyboardHints(
    const BashPromptProps& p,
    bool explainer_visible,
    int selected_index,
    const std::vector<Option>& opts)
{
    std::vector<pc::KeyHint> hints;

    hints.push_back({"Esc", "cancel"});

    // Tab hint — shown when on yes/no and not already in input mode
    if (selected_index >= 0 && static_cast<std::size_t>(selected_index) < opts.size()) {
        const auto& opt = opts[selected_index];
        bool on_yes_no = (opt.value == "yes" || opt.value == "no");
        if (on_yes_no && !opt.is_input) {
            hints.push_back({"Tab", "amend"});
        }
    }

    // Explainer hint
    if (p.explainer_enabled) {
        hints.push_back({"Ctrl+E", explainer_visible ? "hide explainer" : "explain"});
    }

    // Debug hint
    if (p.debug_mode) {
        hints.push_back({"Ctrl+D", "debug"});
    }

    return pc::KeyboardHintRow(hints);
}

/// Render debug info panel.
[[nodiscard]] inline Element RenderDebugInfo(const BashPromptProps& p, bool visible) {
    if (!visible || p.debug_text.empty()) return text("");
    return vbox({
        text("") | size(HEIGHT, EQUAL, 1),
        text("Debug info:") | dim | bold,
        paragraph(p.debug_text) | dim | color(Color::GrayLight),
    });
}

} // namespace detail

// ============================================================
// Full component
// ============================================================

/// Render the complete bash permission prompt.
[[nodiscard]] inline Element RenderBashPrompt(std::shared_ptr<detail::PromptState> st) {
    const auto& p = st->props;
    auto options = detail::BuildOptions(*st);

    // --- Header ---
    Elements header_els;
    header_els.push_back(detail::RenderTitle(p));
    // Only add subtitle if it has content
    if (p.classifier_state != BashPromptProps::ClassifierState::None) {
        header_els.push_back(detail::RenderSubtitle(p));
    }
    auto header = vbox(header_els);

    // --- Body content ---
    Elements body_els;

    // Command + description
    body_els.push_back(detail::RenderCommand(p, st->explainer_visible));
    if (!st->explainer_visible) {
        body_els.push_back(detail::RenderDescription(p));
    }

    // Explainer
    if (p.explainer_enabled) {
        body_els.push_back(detail::RenderExplainer(p, st->explainer_visible));
    }

    // Debug info (replaces main content when visible, like TS)
    if (st->debug_visible && p.debug_mode) {
        body_els.push_back(detail::RenderDebugInfo(p, true));
    } else {
        // Rule explanation
        auto rule_el = detail::RenderRuleExplanation(p);
        if (p.rule_explanation.size() > 0 || !p.matched_rules.empty()) {
            body_els.push_back(text(""));
            body_els.push_back(rule_el);
        }

        // Destructive warning
        if (p.is_destructive) {
            body_els.push_back(text(""));
            body_els.push_back(detail::RenderDestructiveWarning(p));
        }

        // "Do you want to proceed?"
        body_els.push_back(text(""));
        body_els.push_back(text("Do you want to proceed?") | dim);

        // Options list
        body_els.push_back(text(""));
        body_els.push_back(detail::RenderOptionsList(options, st->selected_index));
    }

    // Keyboard hints (always at bottom)
    body_els.push_back(text(""));
    body_els.push_back(detail::RenderKeyboardHints(p, st->explainer_visible, st->selected_index, options));

    auto body = vbox(body_els);

    // --- Assemble dialog ---
    // Faithful to TS PermissionDialog: round border, top-only border style
    auto dialog = window(
        hbox({
            text(" ") | bold,
            text("⚡") | color(Color::Yellow),
            text(" Permission ") | bold | color(Color::Yellow),
        }),
        vbox({
            header | color(Color::White),
            pc::ThinDivider(),
            text("") | size(HEIGHT, EQUAL, 1),
            body | xflex,
        }) | xflex
    ) | color(Color::Yellow);

    // Apply size constraints
    return dialog | size(WIDTH, LESS_THAN, 90) | size(HEIGHT, LESS_THAN, 40);
}

// ============================================================
// Public factory
// ============================================================

/// Create an interactive Bash permission prompt component.
/// Faithful to TS BashPermissionRequest component behaviour.
[[nodiscard]] inline Component MakeBashPermissionPrompt(BashPromptProps props) {
    auto state = std::make_shared<detail::PromptState>();
    state->props = std::move(props);
    state->editable_prefix = state->props.editable_prefix;
    state->yes_feedback = state->props.yes_feedback_text;
    state->no_feedback = state->props.no_feedback_text;
    state->explainer_visible = state->props.explainer_visible;
    state->debug_visible = state->props.debug_visible;

    // Helper to get current options
    auto current_options = [state]() { return detail::BuildOptions(*state); };

    auto emit_decision = [state](Decision decision, std::string_view feedback, std::string_view prefix) {
        if (state->props.on_decide) {
            state->props.on_decide(decision, feedback, prefix);
        }
    };

    return Renderer([state] { return RenderBashPrompt(state); })
         | CatchEvent([state, emit_decision, current_options](Event event) -> bool {

        auto opts = current_options();
        int count = static_cast<int>(opts.size());

        // --- Movement ---
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index = (state->selected_index - 1 + count) % count;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index = (state->selected_index + 1) % count;
            return true;
        }

        // --- Direct shortcuts ---
        if (event == Event::Character('y')) {
            // Find "yes" option and activate it
            for (int i = 0; i < count; ++i) {
                if (opts[i].value == "yes") {
                    state->selected_index = i;
                    break;
                }
            }
            emit_decision(Decision::AllowOnce, state->yes_feedback, "");
            return true;
        }
        if (event == Event::Character('n')) {
            // Find "no" option and activate it
            for (int i = 0; i < count; ++i) {
                if (opts[i].value == "no") {
                    state->selected_index = i;
                    break;
                }
            }
            emit_decision(Decision::Deny, state->no_feedback, "");
            return true;
        }

        // --- Tab: toggle feedback/input mode for focused option ---
        if (event == Event::Tab) {
            if (state->selected_index >= 0 && static_cast<std::size_t>(state->selected_index) < opts.size()) {
                const auto& opt = opts[state->selected_index];
                if (opt.value == "yes") {
                    state->props.yes_feedback_mode = !state->props.yes_feedback_mode;
                    return true;
                }
                if (opt.value == "no") {
                    state->props.no_feedback_mode = !state->props.no_feedback_mode;
                    return true;
                }
            }
            return true; // Tab always handled, even if no-op on non-yes/no
        }

        // --- Ctrl+E: toggle explainer ---
        // In FTXUI, Ctrl+E is character 0x05 (Ctrl+A=0x01, ..., Ctrl+E=0x05)
        if (event.is_character() && event.character().size() == 1 &&
            event.character()[0] == '\x05') {
            if (state->props.explainer_enabled) {
                state->explainer_visible = !state->explainer_visible;
                if (state->props.on_explainer_toggle) {
                    state->props.on_explainer_toggle(state->explainer_visible);
                }
            }
            return true;
        }

        // --- Ctrl+D: toggle debug info ---
        // Ctrl+D is character 0x04
        if (event.is_character() && event.character().size() == 1 &&
            event.character()[0] == '\x04') {
            if (state->props.debug_mode) {
                state->debug_visible = !state->debug_visible;
            }
            return true;
        }

        // --- Enter: activate selected option ---
        if (event == Event::Return) {
            if (state->selected_index >= 0 && static_cast<std::size_t>(state->selected_index) < opts.size()) {
                const auto& opt = opts[state->selected_index];
                if (opt.value == "yes") {
                    emit_decision(Decision::AllowOnce, state->yes_feedback, "");
                } else if (opt.value == "yes-prefix-edited") {
                    emit_decision(Decision::AllowWithPrefix, "", state->editable_prefix);
                } else if (opt.value == "yes-apply-suggestions") {
                    emit_decision(Decision::AllowWithPrefix, "", "");
                } else if (opt.value == "no") {
                    emit_decision(Decision::Deny, state->no_feedback, "");
                }
            }
            return true;
        }

        // --- Escape: abort ---
        if (event == Event::Escape) {
            if (state->props.on_abort) {
                state->props.on_abort();
            }
            emit_decision(Decision::Abort, "", "");
            return true;
        }

        // --- Text input for editable options ---
        // When the selected option is an input, handle character input
        if (state->selected_index >= 0 && static_cast<std::size_t>(state->selected_index) < opts.size()) {
            const auto& opt = opts[state->selected_index];
            if (opt.is_input && event.is_character()) {
                char c = event.character()[0];
                // Only handle printable ASCII for now
                if (c >= 32 && c < 127) {
                    if (opt.value == "yes") {
                        state->yes_feedback += c;
                    } else if (opt.value == "no") {
                        state->no_feedback += c;
                    } else if (opt.value == "yes-prefix-edited") {
                        state->editable_prefix += c;
                        if (state->props.on_prefix_change) {
                            state->props.on_prefix_change(state->editable_prefix);
                        }
                    }
                    return true;
                }
            }
            // Backspace for input fields
            if (opt.is_input && event == Event::Backspace) {
                if (opt.value == "yes" && !state->yes_feedback.empty()) {
                    state->yes_feedback.pop_back();
                    return true;
                }
                if (opt.value == "no" && !state->no_feedback.empty()) {
                    state->no_feedback.pop_back();
                    return true;
                }
                if (opt.value == "yes-prefix-edited" && !state->editable_prefix.empty()) {
                    state->editable_prefix.pop_back();
                    if (state->props.on_prefix_change) {
                        state->props.on_prefix_change(state->editable_prefix);
                    }
                    return true;
                }
            }
        }

        return false;
    });
}

// ============================================================
// Convenience: simple yes/no prompt (most common basic case)
// ============================================================

/// Create a simple bash permission prompt with just Yes/No options.
/// This is the minimal version — use MakeBashPermissionPrompt for full features.
[[nodiscard]] inline Component MakeSimpleBashPrompt(
    std::string command,
    std::optional<std::string> working_dir,
    std::function<void(bool allowed)> on_result)
{
    BashPromptProps p;
    p.command = std::move(command);
    p.working_dir = std::move(working_dir);
    p.show_always_allow = false;
    p.on_decide = [on_result = std::move(on_result)]
        (Decision d, std::string_view /*feedback*/, std::string_view /*prefix*/) {
        if (d == Decision::AllowOnce) {
            on_result(true);
        } else {
            on_result(false);
        }
    };
    return MakeBashPermissionPrompt(std::move(p));
}

// ─── Convenience: static render for golden snapshot tests ──────────────────

/// Render a static bash permission prompt for golden snapshot tests.
/// Builds the props internally and returns the rendered Element.
[[nodiscard]] inline Element RenderBashPermissionPromptForTest(
    std::string_view command,
    std::string_view description = "",
    bool is_destructive = false,
    bool show_always_allow = true,
    int selected = 0)
{
    BashPromptProps props;
    props.command = std::string{command};
    if (!description.empty()) {
        props.description = std::string{description};
    }
    props.working_dir = "/home/user/project";
    props.is_destructive = is_destructive;
    if (is_destructive) {
        props.destructive_reason = "This command modifies files in place";
    }
    props.sandboxing_enabled = true;
    props.is_sandboxed = !is_destructive;
    props.classifier_state = is_destructive
        ? BashPromptProps::ClassifierState::ManualRequired
        : BashPromptProps::ClassifierState::AutoApproved;
    props.classifier_matched_rule = is_destructive ? "" : "read-only commands";
    props.show_always_allow = show_always_allow;
    props.rule_explanation = is_destructive
        ? "Command may modify files.  Review before approving."
        : "Command only reads files and is safe to run.";
    props.matched_rules = is_destructive
        ? std::vector<std::string>{"write-destructive"}
        : std::vector<std::string>{"read-only-auto"};

    auto state = std::make_shared<detail::PromptState>();
    state->props = std::move(props);
    state->selected_index = selected;
    state->editable_prefix = std::string{command.substr(0, command.find(' '))} + ":*";

    return RenderBashPrompt(state);
}

} // namespace cc::ui::permissions::bash_prompt
