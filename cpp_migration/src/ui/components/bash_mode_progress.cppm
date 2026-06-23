/// @file bash_mode_progress.cppm
/// @brief Bash mode progress renderer — faithful port of BashModeProgress.tsx.
///
/// Top-level component for the bash-mode ("!") command input UI.
/// Combines:
///   - UserBashInputMessage (the command the user typed, wrapped in ! prefix)
///   - ShellProgressMessage (live output progress, or "Running…" state)
///     -or-
///   - BashTool.renderToolUseProgressMessage fallback (when no progress data)
///
/// TS source: src/components/BashModeProgress.tsx
/// Props:
///   - input:    string             (the raw command text, NOT wrapped in tags)
///   - progress: ShellProgress | null (live progress data, or null for initial)
///   - verbose:  boolean            (verbose mode flag)
///
/// Layout:
///   <Box flexDirection="column" marginTop={1}>
///     <UserBashInputMessage addMargin={false}
///       param={{ text: `<bash-input>${input}</bash-input>`, type: 'text' }} />
///     {progress ? <ShellProgressMessage ... />
///               : BashTool.renderToolUseProgressMessage([], { verbose, tools: [], ... })}
///   </Box>
// ────────────────────────────────────────────────────────────────────────
module;

#include <string>
#include <optional>
#include <vector>

#include <ftxui/dom/elements.hpp>

export module cc.ui.components.bash_mode_progress;

import cc.ui.components.user_bash_input_message;
import cc.ui.components.shell_progress_message;
import cc.ui.design.tokens;
import cc.ui.design.theme;

export namespace cc::ui::components::bash {

using namespace ftxui;
using cc::ui::design::theme::Theme;

// ─── ShellProgress type ────────────────────────────────────────────────
//
// Mirrors TS ShellProgress type (from src/types/tools.js).
// Used by both bash-mode and task progress rendering.
//
// TS fields (from ShellProgressMessage usage):
//   fullOutput: string
//   output: string
//   elapsedTimeSeconds?: number
//   totalLines?: number
//   totalBytes?: number
//   timeoutMs?: number
//   taskId?: string

struct ShellProgress {
    std::string full_output;         // TS: fullOutput
    std::string output;              // TS: output
    std::optional<double> elapsed_time_seconds;  // TS: elapsedTimeSeconds
    std::optional<std::uint64_t> total_lines;    // TS: totalLines
    std::optional<std::uint64_t> total_bytes;    // TS: totalBytes
    std::optional<std::uint64_t> timeout_ms;     // TS: timeoutMs
    std::optional<std::string> task_id;          // TS: taskId
};

[[nodiscard]] inline ShellProgress make_shell_progress() {
    return ShellProgress{};
}

// ─── Props ──────────────────────────────────────────────────────────────

struct BashModeProgressProps {
    std::string input;                // TS: input: string (raw command text)
    std::optional<ShellProgress> progress;  // TS: progress: ShellProgress | null
    bool verbose = false;             // TS: verbose: boolean
};

[[nodiscard]] inline BashModeProgressProps make_bash_mode_progress_props() {
    return BashModeProgressProps{};
}

// ─── Render ─────────────────────────────────────────────────────────────
//
// Faithful port of BashModeProgress.tsx:
//
//   <Box flexDirection="column" marginTop={1}>
//     <UserBashInputMessage
//       addMargin={false}
//       param={{ text: `<bash-input>${input}</bash-input>`, type: "text" }}
//     />
//     {progress
//       ? <ShellProgressMessage
//           fullOutput={progress.fullOutput}
//           output={progress.output}
//           elapsedTimeSeconds={progress.elapsedTimeSeconds}
//           totalLines={progress.totalLines}
//           verbose={verbose}
//         />
//       : BashTool.renderToolUseProgressMessage?.([], {
//           verbose,
//           tools: [],
//           terminalSize: undefined,
//         })
//     }
//   </Box>
//
// Fallback (no progress): BashTool.renderToolUseProgressMessage with empty
// progress messages returns <MessageResponse height={1}>"Running…"</MessageResponse>
// (see BashTool/UI.tsx renderToolUseProgressMessage).

[[nodiscard]] inline Element render_bash_mode_progress(
    const BashModeProgressProps& props,
    const Theme& theme)
{
    // Build UserBashInputMessage input: wrap in <bash-input> tags
    std::string wrapped_input = "<bash-input>" + props.input + "</bash-input>";

    UserBashInputProps input_props;
    input_props.add_margin = false;
    input_props.text = std::move(wrapped_input);
    auto input_el = render_user_bash_input_message(input_props, theme);

    // Build progress / fallback element
    Element progress_el = text("");

    if (props.progress.has_value()) {
        const auto& p = *props.progress;
        shell::ShellProgressMessageProps msg_props;
        msg_props.output = p.output;
        msg_props.full_output = p.full_output;
        msg_props.elapsed_time_seconds = p.elapsed_time_seconds;
        msg_props.total_lines = p.total_lines;
        msg_props.total_bytes = p.total_bytes;
        msg_props.timeout_ms = p.timeout_ms;
        msg_props.task_id = p.task_id;
        msg_props.verbose = props.verbose;
        progress_el = shell::render_shell_progress_message(msg_props, theme);
    } else {
        // Fallback: BashTool.renderToolUseProgressMessage with no progress
        // Returns <MessageResponse height={1}><Text dimColor>Running…</Text></MessageResponse>
        auto prefix = text("  \xe2\x8f\xbf  ") | dim | color(theme.palette->muted);
        auto running_text = text("Running\xe2\x80\xa6") | dim | color(theme.palette->muted);
        // height=1 → fixed single line
        progress_el = hbox({ prefix, running_text }) | size(HEIGHT, EQUAL, 1);
    }

    // <Box flexDirection="column" marginTop={1}>
    // marginTop={1} → one blank line at the top.
    // Use text(" ") since empty text has zero height in FTXUI.
    return vbox({
        text(" "),       // marginTop={1}
        input_el,
        progress_el,
    });
}

} // namespace cc::ui::components::bash
