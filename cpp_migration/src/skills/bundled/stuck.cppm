/// @file stuck.cppm
/// @brief /stuck slash command — diagnose OTHER Claude Code sessions on the
///        same machine that appear frozen / stuck / very slow.
///
/// Audit vs TS src/skills/bundled/stuck.ts:
///   - TS builds a 55-line structured prompt (STUCK_PROMPT) that is handed
///     to the LLM.  The prompt lists:
///       1. 5 signs of a stuck session (high CPU, D/T/Z state, high RSS,
///          stuck child).
///       2. Investigation steps (ps listing, pgrep child, re-sampling,
///          debug log tail, optional macOS `sample`).
///       3. Report instructions: only post to Slack if something is found;
///          2-message structure (1 line summary + thread dump).
///       4. Notes: diagnostic only; respect user-provided PID/symptom.
///     All of the above is ported verbatim as `kStuckPrompt`.
///   - TS also appends user args under "## User-provided context".
///     Exposed via `build_stuck_prompt(args)`.
///   - TS gates on USER_TYPE == 'ant'; `is_ant_user()` mirrors this.
///   - The pre-existing `detect_stuck_pattern` / `suggest_unstuck_action` /
///     `get_alternative_approaches` helpers (added by an earlier C++ pass,
///     not present in TS) are RETAINED as supplementary pure utilities
///     — they provide an offline fast-path the LLM may consult *before*
///     reaching for `ps`.  They are NOT a replacement for STUCK_PROMPT.
module;
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <span>

export module cc.skills.bundled.stuck;

import cc.skills.skill;
import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// ============================================================
// kStuckPrompt — verbatim port of TS `STUCK_PROMPT` (line-for-line parity).
// ============================================================
constexpr std::string_view kStuckPrompt = R"P(# /stuck — diagnose frozen/slow Claude Code sessions

The user thinks another Claude Code session on this machine is frozen,
stuck, or very slow. Investigate and post a report to
#claude-code-feedback.

## What to look for

Scan for other Claude Code processes (excluding the current one — PID is
known; for shell commands just exclude the PID you see running this
prompt). Process names are typically `claude` (installed) or `cli` (native
dev build).

Signs of a stuck session:
- **High CPU (≥90%) sustained** — likely an infinite loop. Sample twice,
  1-2s apart, to confirm it's not a transient spike.
- **Process state `D` (uninterruptible sleep)** — often an I/O hang. The
  `state` column in `ps` output; first character matters (ignore
  modifiers like `+`, `s`, `<`).
- **Process state `T` (stopped)** — user probably hit Ctrl+Z by accident.
- **Process state `Z` (zombie)** — parent isn't reaping.
- **Very high RSS (≥4GB)** — possible memory leak making the session
  sluggish.
- **Stuck child process** — a hung `git`, `node`, or shell subprocess can
  freeze the parent. Check `pgrep -lP <pid>` for each session.

## Investigation steps

1. **List all Claude Code processes** (macOS/Linux):
   ```
   ps -axo pid=,pcpu=,rss=,etime=,state=,comm=,command= \
     | grep -E '(claude|cli)' | grep -v grep
   ```
   Filter to rows where `comm` is `claude` or (`cli` AND the command path
   contains "claude").

2. **For anything suspicious**, gather more context:
   - Child processes: `pgrep -lP <pid>`
   - If high CPU: sample again after 1-2s to confirm it's sustained
   - If a child looks hung (e.g., a git command), note its full command
     line with `ps -p <child_pid> -o command=`
   - Check the session's debug log if you can infer the session ID:
     `~/.claude/debug/<session-id>.txt` (the last few hundred lines often
     show what it was doing before hanging)

3. **Consider a stack dump** for a truly frozen process (advanced,
   optional):
   - macOS: `sample <pid> 3` gives a 3-second native stack sample
   - This is big — only grab it if the process is clearly hung and you
     want to know *why*

## Report

**Only post to Slack if you actually found something stuck.** If every
session looks healthy, tell the user that directly — do not post an
all-clear to the channel.

If you did find a stuck/slow session, post to **#claude-code-feedback**
(channel ID: `C07VBSHV7EV`) using the Slack MCP tool. Use ToolSearch to
find `slack_send_message` if it's not already loaded.

**Use a two-message structure** to keep the channel scannable:

1. **Top-level message** — one short line: hostname, Claude Code version,
   and a terse symptom (e.g. "session PID 12345 pegged at 100% CPU for
   10min" or "git subprocess hung in D state"). No code blocks, no
   details.
2. **Thread reply** — the full diagnostic dump. Pass the top-level
   message's `ts` as `thread_ts`. Include:
   - PID, CPU%, RSS, state, uptime, command line, child processes
   - Your diagnosis of what's likely wrong
   - Relevant debug log tail or `sample` output if you captured it

If Slack MCP isn't available, format the report as a message the user can
copy-paste into #claude-code-feedback (and let them know to thread the
details themselves).

## Notes
- Don't kill or signal any processes — this is diagnostic only.
- If the user gave an argument (e.g., a specific PID or symptom), focus
  there first.
)P";

/// Build the full /stuck prompt, appending user-provided context exactly as
/// TS does (`STUCK_PROMPT + "\n## User-provided context\n\n${args}\n"`).
[[nodiscard]] inline std::string build_stuck_prompt(std::string_view args) {
    std::string out(kStuckPrompt);
    if (!args.empty()) {
        out += std::format("\n## User-provided context\n\n{}\n", args);
    }
    return out;
}

/// ANT user gate (matches TS: `if (process.env.USER_TYPE !== 'ant') return;`).
[[nodiscard]] inline bool is_ant_user() {
    if (const char* v = std::getenv("USER_TYPE")) {
        return std::string_view(v) == "ant";
    }
    return false;
}

// ============================================================
// Supplementary C++ helpers (NOT in TS; retained from prior pass).
// These give the embedder a cheap, offline heuristic that can be evaluated
// BEFORE the LLM is asked to run shell commands.  They are also surfaced
// in the skill "content" block as "quick checks".
// ============================================================

/// Detect if `recent_outputs` shows a stuck-loop signature (3+ identical
/// consecutive rows; 3+ consecutive "error/failed" rows; A/B/A/B oscillation
/// across 4+ rows).
bool detect_stuck_pattern(std::span<std::string> recent_outputs) {
    if (recent_outputs.size() < 3) return false;

    // Pattern 1: 3+ identical consecutive outputs.
    int consecutive_same = 0;
    for (std::size_t i = 1; i < recent_outputs.size(); ++i) {
        if (recent_outputs[i] == recent_outputs[i - 1]) {
            ++consecutive_same;
            if (consecutive_same >= 2) return true;  // 3 in a row
        } else {
            consecutive_same = 0;
        }
    }

    // Pattern 2: 3+ rows in a row mention Error/error/failed.
    int error_count = 0;
    for (const auto& o : recent_outputs) {
        const bool has_err =
            o.find("Error") != std::string::npos ||
            o.find("error") != std::string::npos ||
            o.find("failed") != std::string::npos;
        if (has_err) ++error_count;
        else         error_count  = 0;
        if (error_count >= 3) return true;
    }

    // Pattern 3: A/B/A/B oscillation (size >= 4).
    if (recent_outputs.size() >= 4) {
        bool oscillating = true;
        for (std::size_t i = 2; i < recent_outputs.size() && oscillating; ++i) {
            if (recent_outputs[i] != recent_outputs[i - 2]) oscillating = false;
        }
        if (oscillating) return true;
    }
    return false;
}

/// Map a short context string to a human-readable unstuck recommendation.
/// Extends the previous list with TS-specific pain points (429, auth,
/// encoding, subprocess).
std::string suggest_unstuck_action(std::string_view context) {
    std::string ctx(context);
    std::transform(ctx.begin(), ctx.end(), ctx.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    // ---- TS-aligned diagnostics (per spec checklist) --------------------
    // 429 rate limit
    if (ctx.find("429") != std::string::npos ||
        ctx.find("rate limit") != std::string::npos ||
        ctx.find("too many requests") != std::string::npos) {
        return "Hitting API rate limits (429).  Try: 1) Reduce request "
               "concurrency, 2) Add exponential backoff, 3) Request a higher "
               "rate tier from account settings, 4) Use streaming to stay "
               "under token-per-minute caps.";
    }
    // Auth / API key
    if (ctx.find("401") != std::string::npos ||
        ctx.find("403") != std::string::npos ||
        ctx.find("unauthorized") != std::string::npos ||
        ctx.find("invalid key") != std::string::npos ||
        ctx.find("api key") != std::string::npos ||
        ctx.find("auth") != std::string::npos) {
        return "Authentication failure.  Try: 1) Re-run /login to refresh "
               "the OAuth session, 2) Verify ANTHROPIC_API_KEY / USER_TYPE "
               "env vars, 3) Rotate the key at https://console.anthropic.com, "
               "4) Check that the key is not expired or scoped incorrectly.";
    }
    // Timeout (covers tool timeouts + child process hangs).
    if (ctx.find("timeout") != std::string::npos ||
        ctx.find("timed out") != std::string::npos) {
        return "The operation is timing out.  Try: 1) Check network "
               "connectivity (traceroute / curl to endpoint), 2) Increase "
               "the per-tool timeout via settings, 3) Try a smaller batch / "
               "simpler input, 4) If a subprocess: look for a hung child "
               "with `pgrep -lP <parent_pid>`.";
    }
    // Permission denied (filesystem).
    if (ctx.find("permission") != std::string::npos ||
        ctx.find("denied") != std::string::npos ||
        ctx.find("eacces") != std::string::npos) {
        return "Permission error.  Try: 1) Check file/directory ownership "
               "and mode with `ls -la <path>`, 2) Run with elevated perms if "
               "appropriate, 3) Make sure the target isn't inside a "
               "restricted directory (tmpfs, SIP, read-only mount).";
    }
    // File encoding.
    if (ctx.find("encoding") != std::string::npos ||
        ctx.find("utf-8") != std::string::npos ||
        ctx.find("invalid byte") != std::string::npos ||
        ctx.find("bom") != std::string::npos) {
        return "File-encoding issue.  Try: 1) Confirm encoding with `file "
               "-bi <path>`, 2) Re-save as UTF-8 without BOM, 3) Convert "
               "Latin-1 / GBK files with `iconv -f OLD -t utf-8`, 4) Exclude "
               "binary files from glob patterns.";
    }
    // Sub-agent / remote session non-response.
    if (ctx.find("sub-agent") != std::string::npos ||
        ctx.find("subagent") != std::string::npos ||
        ctx.find("agent.*no.*response") != std::string::npos) {
        return "Sub-agent appears unresponsive.  Try: 1) Reduce input size "
               "passed to the sub-agent, 2) Give it a narrower scope, 3) "
               "Check that its tools/permissions match what it needs, 4) "
               "Switch to a smaller model for the sub-task.";
    }
    // ---- Original (pre-existing) patterns retained ----------------------
    if (ctx.find("not found") != std::string::npos ||
        ctx.find("no such") != std::string::npos) {
        return "Verify the path/resource exists.  Try listing the directory "
               "or searching for the correct name.";
    }
    if (ctx.find("syntax") != std::string::npos ||
        ctx.find("parse") != std::string::npos) {
        return "Syntax/parsing error.  Try: 1) Validate the input format, "
               "2) Check for missing delimiters, 3) Simplify the input.";
    }
    if (ctx.find("import") != std::string::npos ||
        ctx.find("module") != std::string::npos) {
        return "Module/import issue.  Try: 1) Verify the dependency is "
               "installed, 2) Check import paths, 3) Clear module cache.";
    }
    // Generic.
    return "Try a different approach: 1) Break the problem into smaller "
           "steps, 2) Verify assumptions with explicit checks, 3) Search "
           "for similar patterns in the codebase.";
}

/// Return a list of 2-4 alternative strategies given a description of the
/// current (stalled) approach.  Augments the pre-existing list.
std::vector<std::string> get_alternative_approaches(
    std::string_view current_approach)
{
    std::vector<std::string> out;
    std::string a(current_approach);
    std::transform(a.begin(), a.end(), a.begin(),
        [](unsigned char c){ return std::tolower(c); });

    // Edit/modify loop.
    if (a.find("edit") != std::string::npos ||
        a.find("modify") != std::string::npos) {
        out.emplace_back("Rewrite the file from scratch instead of editing "
                         "in place");
        out.emplace_back("Create a new file with the desired content, then "
                         "replace the old one");
        out.emplace_back("Use a different editing strategy (line-based vs "
                         "block-based)");
    }
    // Search/find loop.
    if (a.find("search") != std::string::npos ||
        a.find("find") != std::string::npos) {
        out.emplace_back("Try broader search terms or patterns");
        out.emplace_back("Search in different directories or file types");
        out.emplace_back("Use semantic search instead of text matching");
    }
    // Build/compile loop.
    if (a.find("build") != std::string::npos ||
        a.find("compile") != std::string::npos) {
        out.emplace_back("Clean build artifacts and rebuild from scratch");
        out.emplace_back("Check for missing or conflicting dependencies");
        out.emplace_back("Try building individual components in isolation");
    }
    // Tool loop / bash loop.
    if (a.find("bash") != std::string::npos ||
        a.find("tool.*loop") != std::string::npos) {
        out.emplace_back("Abort the bash loop with an explicit exit status");
        out.emplace_back("Replace the script with a single focused command");
        out.emplace_back("Check the subprocess's child tree with pgrep -lP");
    }
    // Fallback.
    if (out.empty()) {
        out.emplace_back("Step back and re-read the requirements");
        out.emplace_back("Try the simplest possible implementation first");
        out.emplace_back("Look for existing code that does something similar");
        out.emplace_back("Break the task into smaller, independently testable steps");
    }
    return out;
}

// ============================================================
// SkillManifest (retained; mirrors get_stuck_skill_manifest from prior pass
// but updated to describe the *TS-aligned* behaviour).
// ============================================================
cc::skills::SkillManifest get_stuck_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "stuck",
        .description =
            "[ANT-ONLY] Investigate frozen/stuck/slow Claude Code sessions "
            "on this machine and post a diagnostic report to "
            "#claude-code-feedback.",
        .version = "1.1.0",
        .triggers = {
            "stuck", "frozen", "session hung", "100% cpu",
            "high cpu", "slow session", "not responding",
            "claudecode stuck", "/stuck",
        },
        .directory = {},
    };
}

// ============================================================
// SkillDefinition factory (TS-parity registration target).
// ============================================================
[[nodiscard]] inline cc::skills::SkillDefinition make_stuck_skill() {
    return cc::skills::SkillDefinition{
        .name = "stuck",
        .description =
            "[ANT-ONLY] Investigate frozen/stuck/slow Claude Code sessions "
            "on this machine and post a diagnostic report to "
            "#claude-code-feedback.",
        .trigger_patterns = {
            R"(/stuck)",
            R"(\bstuck\b)",
            R"(frozen.*session)",
            R"(session.*hung)",
            R"(100\s*%.*cpu)",
            R"(high\s+cpu.*(?:claude|cli))",
            R"(slow.*(?:session|loop))",
            R"(not\s+responding.*claude)",
        },
        .content = std::string(kStuckPrompt) +
R"(
## Supplementary offline checks (fast-path, pre-LLM)

If you are not sure you need to run `ps`, first evaluate these cheap
heuristics on the conversation history:

- `detect_stuck_pattern(recent_outputs)` — returns true when the last 3+
  tool outputs are identical, all contain "error", or oscillate A/B/A/B.
  Trigger: three identical consecutive outputs.
- `suggest_unstuck_action(context_keyword)` — one-line repair tip.
  Covered keywords: 429 / rate limit, 401/403 / auth, timeout, permission,
  encoding, sub-agent, not-found, syntax, import/module.
- `get_alternative_approaches(approach_description)` — 2-4 concrete
  alternative tactics for an approach that keeps failing.

If any of these fire with a confident diagnosis AND you are investigating
*your own* session (not another PID), you can give the user the repair
tip immediately without shelling out.)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.1.0",
    };
}

} // namespace cc::skills::bundled
