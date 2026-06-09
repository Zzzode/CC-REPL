/// @file verify_content.cppm
/// @brief "verify" content skill — smokescreen runtime verification: rebuild
///        the project, run it against representative inputs, compare real
///        output against the user's original claim. This is the C++ port of
///        `src/skills/bundled/verifyContent.ts` + the registration logic in
///        `src/skills/bundled/verify.ts`.
///
/// The TS `/verify` skill is intentionally narrow in scope:
///   * it runs **the project itself** (not the test suite)
///   * it focuses on CLI binaries or HTTP servers
///   * it produces a side-by-side "expected vs. observed" report
///
/// This module keeps that shape. UI components (React renderers in the TS
/// source) are intentionally omitted per the Phase 2 scope — those are a
/// Phase 4 concern.
module;

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.skills.verify_content;

import cc.skills.skill;

export namespace cc::skills::verify_content {

// ============================================================
// Pure helpers (ported from the TS skill body). These have no side-effects
// — they only format strings and classify inputs. Any real execution is
// delegated to cc.tools.bash at the call site.
// ============================================================

/// Strip HTML `<!-- comments -->` (same pipeline used in claude_api_content).
/// The TS verify SKILL.md uses comments for editorial notes that must never
/// reach the LLM prompt.
[[nodiscard]] inline std::string strip_html_comments(std::string text) {
    static const std::regex html_comment(R"(<!--[\s\S]*?-->\n?)");
    std::string prev;
    do {
        prev = text;
        text = std::regex_replace(text, html_comment, "");
    } while (text != prev);
    return text;
}

/// Detect whether the project appears to be a CLI app vs. an HTTP server
/// vs. unknown. Scans the provided list of top-level filenames / markers
/// (caller typically passes entries from `std::filesystem::directory_iterator`
/// on the project root). Classification matches TS verify/examples routing:
///   * package.json with "bin" → CLI (with a fallback: any "cli" substring
///     in entrypoint names)
///   * server.{ts,js,py,go,rb,java} or start-server script → HTTP server
///   * otherwise: Unknown, and the skill asks the user to clarify.
enum class ProjectKind { Cli, Server, Unknown };

[[nodiscard]] inline std::string_view to_string(ProjectKind k) {
    switch (k) {
        case ProjectKind::Cli:     return "cli";
        case ProjectKind::Server:  return "server";
        case ProjectKind::Unknown: return "unknown";
    }
    return "unknown";
}

struct EntryInfo {
    std::string_view filename;   // basename only
    std::string_view contents;   // optional: head of file contents (caller
                                 // may leave empty — best-effort detection
                                 // still works from filename alone)
};

[[nodiscard]] inline ProjectKind detect_project_kind(
    std::vector<EntryInfo> entries) noexcept {

    static constexpr std::array<std::string_view, 8> server_markers{
        "server.ts", "server.js", "server.py", "server.go",
        "server.rb", "server.java", "start-server", "main-server"
    };
    static constexpr std::array<std::string_view, 6> cli_markers{
        "cli.ts", "cli.js", "cli.py", "cli.go", "cli.rb", "main-cli"
    };

    // Phase 1: exact-match filenames (highest signal)
    for (const auto& e : entries) {
        if (std::ranges::find(server_markers, e.filename) !=
            server_markers.end()) return ProjectKind::Server;
        if (std::ranges::find(cli_markers, e.filename) != cli_markers.end())
            return ProjectKind::Cli;
    }

    // Phase 2: package.json with "bin" field (classic npm CLI)
    for (const auto& e : entries) {
        if (e.filename == "package.json") {
            if (e.contents.empty()) {
                // No contents supplied — conservatively assume CLI because
                // package.json often accompanies a bun/node tool.
                return ProjectKind::Cli;
            }
            if (e.contents.find("\"bin\"") != std::string_view::npos)
                return ProjectKind::Cli;
            if (e.contents.find("\"start\"") != std::string_view::npos &&
                e.contents.find("server") != std::string_view::npos)
                return ProjectKind::Server;
        }
    }

    // Phase 3: substring heuristics on entry names
    auto lower = [](std::string_view s) {
        std::string out(s.size(), '\0');
        std::ranges::transform(s, out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    for (const auto& e : entries) {
        const auto name = lower(e.filename);
        if (name.find("server") != std::string::npos &&
            (name.ends_with(".ts") || name.ends_with(".js") ||
             name.ends_with(".py") || name.ends_with(".go")))
            return ProjectKind::Server;
        if (name.find("cli") != std::string::npos) return ProjectKind::Cli;
    }

    return ProjectKind::Unknown;
}

// ============================================================
// Prompt fragments. These are ported from the two TS verify/examples/*.md
// placeholders plus the structural sections in the TS `getPromptForCommand`.
// Each fragment is a self-contained markdown block.
// ============================================================

constexpr std::string_view VERIFY_INTRO = R"raw(# verify — Runtime verification

This skill performs **runtime smoke testing** of the user's project. It is
not a linter, not a unit-test runner, and not a type-checker. Think of it
as the final "open the app and click around" step, automated.

## Core contract

Given a user-supplied claim ("my CLI prints hello world", "the server
returns 200 on /healthz") you MUST:

1. **Rebuild** the project from a clean state.
2. **Run** the binary / start the server with representative inputs.
3. **Capture** stdout / stderr / HTTP response body.
4. **Compare** observed output against the claim line-by-line.
5. **Report** a checklist where every item carries EVIDENCE (command
   output, timestamps, curl payloads) — never assert pass/fail without
   quoting the actual output.

DO NOT trust "it compiled" as proof of correctness. The user's claim is
always about runtime behaviour.
)raw";

constexpr std::string_view CLI_CHECKLIST = R"raw(
## CLI verification checklist (project type: CLI)

Run the following steps in order. For each step, paste the EXACT output
(trimmed is fine, but never summarize with words like "it succeeded"
without quoting the output that proves it).

1. **Build**
   * `$ <build-command>`
   * Expected: exit 0, no warnings that hint at the claim being broken.
   * Paste the tail of the build log.

2. **`--help` / `--version` sanity**
   * `$ <binary> --help`
   * `$ <binary> --version` (or `version` subcommand)
   * Confirm the binary starts and prints a non-empty help banner.

3. **Happy-path invocation matching the user's claim**
   * Translate the user's claim into a concrete argv. If they said "prints
     hello world", invoke `<binary> hello world` or whatever matches their
     description.
   * Capture stdout, stderr, and exit code.
   * Produce a side-by-side table:

     | Claim                              | Expected           | Observed (quoted)          | Match? |
     |------------------------------------|--------------------|----------------------------|--------|
     | Reads input file                   | file contents echo | `hello`                    | YES    |
     | Returns exit 0 on success          | 0                  | 0                          | YES    |

4. **One negative case** (if the claim implies one)
   * Missing file, bad flag, empty input — whichever makes sense.
   * Confirm graceful error handling: non-zero exit + human-readable
     message on stderr.

5. **Cleanup**
   * Kill any stray processes; close temporary files.
   * Report final PASS / FAIL with the per-item checklist above.
)raw";

constexpr std::string_view SERVER_CHECKLIST = R"raw(
## Server verification checklist (project type: HTTP server)

Run the following steps in order. Same evidence rules — always quote the
actual curl / output payload.

1. **Build**
   * `$ <build-command>`
   * Expected: exit 0. Paste the tail of the build log.

2. **Start the server in the background**
   * Pick a free port (e.g. `PORT=18080`).
   * `$ PORT=18080 <start-command> &`
   * Wait until the "listening on ..." line appears (or equivalent log).
   * Capture the PID so you can kill it in step 5.

3. **Health / readiness probe**
   * `$ curl -sS -i http://127.0.0.1:18080/healthz` or the documented route.
   * Confirm HTTP status is 2xx.

4. **Route matching the user's claim**
   * Translate the claim into a concrete curl (method, headers, body).
   * Capture full response (status + headers + body via `-i`).
   * Produce the comparison table (same format as CLI step 3).

5. **Negative / edge-case route** (if the claim implies one)
   * e.g. unknown route → 404; missing auth → 401; malformed body → 400.

6. **Cleanup**
   * `kill <PID>` (the background server).
   * Report PASS / FAIL with the per-item checklist above.
)raw";

constexpr std::string_view UNKNOWN_KIND_NOTE = R"raw(
## Project kind unknown

I couldn't auto-detect whether this project exposes a CLI binary or an
HTTP server from the top-level directory listing. Before running any
verification steps, ASK the user:

  * "Do you want me to verify a CLI command invocation, or an HTTP
    endpoint? Please paste the exact command / curl you'd use to drive
    the behaviour you care about."

Then proceed with whichever checklist matches. Do NOT guess — wrong guess
produces meaningless verification.
)raw";

constexpr std::string_view EVIDENCE_RULES = R"raw(
## Non-negotiable evidence rules (applies to ALL project kinds)

* Every PASS / FAIL line in the final report MUST link to a quoted
  command + output pair. "It worked" without evidence = FAIL by default.
* Include timestamps in the shell prompt for every run so the reader can
  verify ordering (e.g. shell `PS1` with `\t`, or run each command via
  `time` / `date; <cmd>`).
* If the project has multiple entrypoints, test the one the user
  actually cares about — ask if ambiguous.
* Do **not** substitute unit tests for this skill. `/verify` runs the
  shipped binary/server. If the user wants tests, they will invoke
  `/debug` or write tests explicitly.
* If any step crashes, hangs (> 10s), or mutates the filesystem in a
  surprising way, stop immediately and report. Do not power through
  failures — a single broken step already falsifies the claim.
)raw";

// ============================================================
// build_prompt() — assembles the fragments above based on project kind and
// optional user args. Port of TS `getPromptForCommand`:
//   SKILL_BODY.trimStart() -> if args -> append "## User Request\n\n<args>"
// ============================================================
[[nodiscard]] inline std::string build_prompt(
    ProjectKind kind,
    std::string_view args) {

    std::vector<std::string> parts;
    parts.emplace_back(VERIFY_INTRO);

    switch (kind) {
        case ProjectKind::Cli:
            parts.emplace_back(CLI_CHECKLIST);
            break;
        case ProjectKind::Server:
            parts.emplace_back(SERVER_CHECKLIST);
            break;
        case ProjectKind::Unknown:
            parts.emplace_back(UNKNOWN_KIND_NOTE);
            break;
    }

    parts.emplace_back(EVIDENCE_RULES);

    if (!args.empty()) {
        parts.push_back(std::format("## User Request\n\n{}", args));
    }

    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\n\n";
        out += parts[i];
    }
    return strip_html_comments(std::move(out));
}

// ============================================================
// SkillDefinition factory — registered via BundledSkills in bundled.cppm.
//
// Trigger intent: when the user explicitly wants to RUN the app and check
// behaviour end-to-end. This is broader than the bundled "verify"
// checklist skill (which covers build/tests/typecheck) — /verify-content
// is specifically about runtime smoke testing.
// ============================================================
[[nodiscard]] inline SkillDefinition make_verify_content_skill() {
    return SkillDefinition{
        .name = "verify-content",
        .description =
            "Runtime verification: rebuild, execute, and diff real output "
            "against the user's claim for CLI binaries and HTTP servers.\n"
            "TRIGGER when: user asks to 'run the app', 'try it', 'smoke "
            "test', 'start the server and curl', 'verify the binary does "
            "X', or uses /verify with a concrete behaviour claim.\n"
            "DO NOT TRIGGER for: unit tests, type-checking, linting, or "
            "generic 'before completion' checklists — use the 'verify' "
            "skill for those.",
        .trigger_patterns = {
            // Explicit invocation / project-run phrasing
            R"(/verify\b)",
            R"(run\s+(?:the\s+)?(?:app|binary|cli|server))",
            R"(smoke\s*test)",
            R"(start\s+(?:the\s+)?server.*curl)",
            R"(start\s+the\s+server\s+and)",
            R"(\btry\s+it\b)",

            // Runtime verification semantics
            R"(verify\s+.*(?:output|behaviour|behavior|server|binary|cli))",
            R"(does\s+(?:it|this)\s+actually\s+work)",
            R"(prove\s+(?:it|that)\s+works)",
            R"(\bend[-\s]?to[-\s]?end\s+test)",

            // Output-comparison phrasing
            R"(expected\s+vs\s+(?:actual|observed))",
            R"(check\s+(?:stdout|stderr|http\s+response|response\s+body))",

            // Build-and-run imperative
            R"(rebuild\s+and\s+run)",
            R"(build\s+and\s+launch)",
        },
        .content =
            // Default content (kind unknown, no args). Callers that can
            // detect the project kind should call build_prompt(kind, args)
            // instead to pick the right checklist.
            build_prompt(ProjectKind::Unknown, ""),
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::verify_content
