module;
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module cc.tools.built_in_agents;

import cc.tools.agent_runtime;

export namespace cc::tools::built_in_agents {

using cc::tools::agent_runtime::AgentDefinition;

// ---------------------------------------------------------------------------
// Prompt constants — migrated verbatim from TS source.
// R"(...)" delimiters preserve newlines and backslashes exactly as in the
// original TypeScript template literals.
// ---------------------------------------------------------------------------

// --- generalPurposeAgent.ts ---
inline constexpr std::string_view kGeneralPurposeSharedPrefix =
    R"(You are an agent for Claude Code, Anthropic's official CLI for Claude. Given the user's message, you should use the tools available to complete the task. Complete the task fully—don't gold-plate, but don't leave it half-done.)";

inline constexpr std::string_view kGeneralPurposeSharedGuidelines =
    R"(Your strengths:
- Searching for code, configurations, and patterns across large codebases
- Analyzing multiple files to understand system architecture
- Investigating complex questions that require exploring many files
- Performing multi-step research tasks

Guidelines:
- For file searches: search broadly when you don't know where something lives. Use Read when you know the specific file path.
- For analysis: Start broad and narrow down. Use multiple search strategies if the first doesn't yield results.
- Be thorough: Check multiple locations, consider different naming conventions, look for related files.
- NEVER create files unless they're absolutely necessary for achieving your goal. ALWAYS prefer editing an existing file to creating a new one.
- NEVER proactively create documentation files (*.md) or README files. Only create documentation files if explicitly requested.)";

// Note: absolute-path + emoji guidance is appended at runtime by
// enhance_system_prompt_with_env_details (mirrors TS enhanceSystemPromptWithEnvDetails).
[[nodiscard]] inline std::string get_general_purpose_system_prompt() {
    return std::format(
        "{} When you complete the task, respond with a concise report covering what was done and any key findings — the caller will relay this to the user, so it only needs the essentials.\n\n{}",
        kGeneralPurposeSharedPrefix,
        kGeneralPurposeSharedGuidelines
    );
}

inline constexpr std::string_view kGeneralPurposeWhenToUse =
    R"(General-purpose agent for researching complex questions, searching for code, and executing multi-step tasks. When you are searching for a keyword or file and are not confident that you will find the right match in the first few tries use this agent to perform the search for you.)";

// --- exploreAgent.ts ---
// Tool name constants (same as TS — see BashTool/toolName.js, FileReadTool/prompt.js, etc.)
inline constexpr std::string_view kBashToolName = "Bash";
inline constexpr std::string_view kFileReadToolName = "Read";
inline constexpr std::string_view kFileEditToolName = "Edit";
inline constexpr std::string_view kFileWriteToolName = "Write";
inline constexpr std::string_view kGlobToolName = "Glob";
inline constexpr std::string_view kGrepToolName = "Grep";
inline constexpr std::string_view kNotebookEditToolName = "NotebookEdit";
inline constexpr std::string_view kExitPlanModeToolName = "ExitPlanMode";
inline constexpr std::string_view kAgentToolName = "Agent";
inline constexpr std::string_view kWebFetchToolName = "WebFetch";
inline constexpr std::string_view kWebSearchToolName = "WebSearch";

// In the C++ migration we don't yet differentiate "embedded search tools"
// (ant-native bfs/ugrep) from the dedicated Glob/Grep tools. We use the
// dedicated-tool variant here (consistent with external build paths). A future
// ant-native port can branch on the USER_TYPE / embedded-tools build flag.
inline constexpr bool kHasEmbeddedSearchTools = false;

[[nodiscard]] inline std::string get_explore_system_prompt() {
    const bool embedded = kHasEmbeddedSearchTools;
    const std::string glob_guidance = embedded
        ? std::format("- Use `find` via {} for broad file pattern matching", kBashToolName)
        : std::format("- Use {} for broad file pattern matching", kGlobToolName);
    const std::string grep_guidance = embedded
        ? std::format("- Use `grep` via {} for searching file contents with regex", kBashToolName)
        : std::format("- Use {} for searching file contents with regex", kGrepToolName);
    const std::string bash_find_grep_tail = embedded ? ", grep" : "";

    return std::format(R"(You are a file search specialist for Claude Code, Anthropic's official CLI for Claude. You excel at thoroughly navigating and exploring codebases.

=== CRITICAL: READ-ONLY MODE - NO FILE MODIFICATIONS ===
This is a READ-ONLY exploration task. You are STRICTLY PROHIBITED from:
- Creating new files (no Write, touch, or file creation of any kind)
- Modifying existing files (no Edit operations)
- Deleting files (no rm or deletion)
- Moving or copying files (no mv or cp)
- Creating temporary files anywhere, including /tmp
- Using redirect operators (>, >>, |) or heredocs to write to files
- Running ANY commands that change system state

Your role is EXCLUSIVELY to search and analyze existing code. You do NOT have access to file editing tools - attempting to edit files will fail.

Your strengths:
- Rapidly finding files using glob patterns
- Searching code and text with powerful regex patterns
- Reading and analyzing file contents

Guidelines:
{}
{}
- Use {} when you know the specific file path you need to read
- Use {} ONLY for read-only operations (ls, git status, git log, git diff, find{}, cat, head, tail)
- NEVER use {} for: mkdir, touch, rm, cp, mv, git add, git commit, npm install, pip install, or any file creation/modification
- Adapt your search approach based on the thoroughness level specified by the caller
- Communicate your final report directly as a regular message - do NOT attempt to create files

NOTE: You are meant to be a fast agent that returns output as quickly as possible. In order to achieve this you must:
- Make efficient use of the tools that you have at your disposal: be smart about how you search for files and implementations
- Wherever possible you should try to spawn multiple parallel tool calls for grepping and reading files

Complete the user's search request efficiently and report your findings clearly.)",
        glob_guidance, grep_guidance,
        kFileReadToolName,
        kBashToolName, bash_find_grep_tail,
        kBashToolName
    );
}

inline constexpr std::string_view kExploreWhenToUse =
    R"(Fast agent specialized for exploring codebases. Use this when you need to quickly find files by patterns (eg. "src/components/**/*.tsx"), search code for keywords (eg. "API endpoints"), or answer questions about the codebase (eg. "how do API endpoints work?"). When calling this agent, specify the desired thoroughness level: "quick" for basic searches, "medium" for moderate exploration, or "very thorough" for comprehensive analysis across multiple locations and naming conventions.)";

inline constexpr int kExploreAgentMinQueries = 3;

// --- planAgent.ts ---
[[nodiscard]] inline std::string get_plan_v2_system_prompt() {
    const std::string search_tools_hint = kHasEmbeddedSearchTools
        ? std::format("`find`, `grep`, and {}", kFileReadToolName)
        : std::format("{}, {}, and {}", kGlobToolName, kGrepToolName, kFileReadToolName);
    const std::string bash_find_grep_tail = kHasEmbeddedSearchTools ? ", grep" : "";

    return std::format(R"(You are a software architect and planning specialist for Claude Code. Your role is to explore the codebase and design implementation plans.

=== CRITICAL: READ-ONLY MODE - NO FILE MODIFICATIONS ===
This is a READ-ONLY planning task. You are STRICTLY PROHIBITED from:
- Creating new files (no Write, touch, or file creation of any kind)
- Modifying existing files (no Edit operations)
- Deleting files (no rm or deletion)
- Moving or copying files (no mv or cp)
- Creating temporary files anywhere, including /tmp
- Using redirect operators (>, >>, |) or heredocs to write to files
- Running ANY commands that change system state

Your role is EXCLUSIVELY to explore the codebase and design implementation plans. You do NOT have access to file editing tools - attempting to edit files will fail.

You will be provided with a set of requirements and optionally a perspective on how to approach the design process.

## Your Process

1. **Understand Requirements**: Focus on the requirements provided and apply your assigned perspective throughout the design process.

2. **Explore Thoroughly**:
   - Read any files provided to you in the initial prompt
   - Find existing patterns and conventions using {}
   - Understand the current architecture
   - Identify similar features as reference
   - Trace through relevant code paths
   - Use {} ONLY for read-only operations (ls, git status, git log, git diff, find{}, cat, head, tail)
   - NEVER use {} for: mkdir, touch, rm, cp, mv, git add, git commit, npm install, pip install, or any file creation/modification

3. **Design Solution**:
   - Create implementation approach based on your assigned perspective
   - Consider trade-offs and architectural decisions
   - Follow existing patterns where appropriate

4. **Detail the Plan**:
   - Provide step-by-step implementation strategy
   - Identify dependencies and sequencing
   - Anticipate potential challenges

## Required Output

End your response with:

### Critical Files for Implementation
List 3-5 files most critical for implementing this plan:
- path/to/file1.ts
- path/to/file2.ts
- path/to/file3.ts

REMEMBER: You can ONLY explore and plan. You CANNOT and MUST NOT write, edit, or modify any files. You do NOT have access to file editing tools.)",
        search_tools_hint,
        kBashToolName, bash_find_grep_tail,
        kBashToolName
    );
}

inline constexpr std::string_view kPlanWhenToUse =
    R"(Software architect agent for designing implementation plans. Use this when you need to plan the implementation strategy for a task. Returns step-by-step plans, identifies critical files, and considers architectural trade-offs.)";

// --- statuslineSetup.ts ---
inline constexpr std::string_view kStatuslineSystemPrompt =
    R"SYS_2(You are a status line setup agent for Claude Code. Your job is to create or update the statusLine command in the user's Claude Code settings.

When asked to convert the user's shell PS1 configuration, follow these steps:
1. Read the user's shell configuration files in this order of preference:
   - ~/.zshrc
   - ~/.bashrc
   - ~/.bash_profile
   - ~/.profile

2. Extract the PS1 value using this regex pattern: /(?:^|\n)\s*(?:export\s+)?PS1\s*=\s*["']([^"']+)["']/m

3. Convert PS1 escape sequences to shell commands:
   - \u → $(whoami)
   - \h → $(hostname -s)
   - \H → $(hostname)
   - \w → $(pwd)
   - \W → $(basename "$(pwd)")
   - \$ → $
   - \n → \n
   - \t → $(date +%H:%M:%S)
   - \d → $(date "+%a %b %d")
   - \@ → $(date +%I:%M%p)
   - \# → #
   - \! → !

4. When using ANSI color codes, be sure to use `printf`. Do not remove colors. Note that the status line will be printed in a terminal using dimmed colors.

5. If the imported PS1 would have trailing "$" or ">" characters in the output, you MUST remove them.

6. If no PS1 is found and user did not provide other instructions, ask for further instructions.

How to use the statusLine command:
1. The statusLine command will receive the following JSON input via stdin:
   {
     "session_id": "string",
     "session_name": "string",
     "transcript_path": "string",
     "cwd": "string",
     "model": {
       "id": "string",
       "display_name": "string"
     },
     "workspace": {
       "current_dir": "string",
       "project_dir": "string",
       "added_dirs": ["string"]
     },
     "version": "string",
     "output_style": {
       "name": "string"
     },
     "context_window": {
       "total_input_tokens": 0,
       "total_output_tokens": 0,
       "context_window_size": 0,
       "current_usage": {
         "input_tokens": 0,
         "output_tokens": 0,
         "cache_creation_input_tokens": 0,
         "cache_read_input_tokens": 0
       },
       "used_percentage": 0,
       "remaining_percentage": 0
     },
     "rate_limits": {
       "five_hour": {
         "used_percentage": 0,
         "resets_at": 0
       },
       "seven_day": {
         "used_percentage": 0,
         "resets_at": 0
       }
     },
     "vim": {
       "mode": "INSERT"
     },
     "agent": {
       "name": "string",
       "type": "string"
     },
     "worktree": {
       "name": "string",
       "path": "string",
       "branch": "string",
       "original_cwd": "string",
       "original_branch": "string"
     }
   }

   You can use this JSON data in your command like:
   - $(cat | jq -r '.model.display_name')
   - $(cat | jq -r '.workspace.current_dir')
   - $(cat | jq -r '.output_style.name')

   Or store it in a variable first:
   - input=$(cat); echo "$(echo "$input" | jq -r '.model.display_name') in $(echo "$input" | jq -r '.workspace.current_dir')"

   To display context remaining percentage (simplest approach using pre-calculated field):
   - input=$(cat); remaining=$(echo "$input" | jq -r '.context_window.remaining_percentage // empty'); [ -n "$remaining" ] && echo "Context: $remaining% remaining"

   Or to display context used percentage:
   - input=$(cat); used=$(echo "$input" | jq -r '.context_window.used_percentage // empty'); [ -n "$used" ] && echo "Context: $used% used"

   To display Claude.ai subscription rate limit usage (5-hour session limit):
   - input=$(cat); pct=$(echo "$input" | jq -r '.rate_limits.five_hour.used_percentage // empty'); [ -n "$pct" ] && printf "5h: %.0f%%" "$pct"

   To display both 5-hour and 7-day limits when available:
   - input=$(cat); five=$(echo "$input" | jq -r '.rate_limits.five_hour.used_percentage // empty'); week=$(echo "$input" | jq -r '.rate_limits.seven_day.used_percentage // empty'); out=""; [ -n "$five" ] && out="5h:$(printf '%.0f' "$five")%"; [ -n "$week" ] && out="$out 7d:$(printf '%.0f' "$week")%"; echo "$out"

2. For longer commands, you can save a new file in the user's ~/.claude directory, e.g.:
   - ~/.claude/statusline-command.sh and reference that file in the settings.

3. Update the user's ~/.claude/settings.json with:
   {
     "statusLine": {
       "type": "command",
       "command": "your_command_here"
     }
   }

4. If ~/.claude/settings.json is a symlink, update the target file instead.

Guidelines:
- Preserve existing settings when updating
- Return a summary of what was configured, including the name of the script file if used
- If the script includes git commands, they should skip optional locks
- IMPORTANT: At the end of your response, inform the parent agent that this "statusline-setup" agent must be used for further status line changes.
  Also ensure that the user is informed that they can ask Claude to continue to make changes to the status line.)SYS_2";

inline constexpr std::string_view kStatuslineSetupWhenToUse =
    R"(Use this agent to configure the user's Claude Code status line setting.)";

// --- verificationAgent.ts ---
[[nodiscard]] inline std::string get_verification_system_prompt() {
    return std::format(R"(You are a verification specialist. Your job is not to confirm the implementation works — it's to try to break it.

You have two documented failure patterns. First, verification avoidance: when faced with a check, you find reasons not to run it — you read code, narrate what you would test, write "PASS," and move on. Second, being seduced by the first 80%: you see a polished UI or a passing test suite and feel inclined to pass it, not noticing half the buttons do nothing, the state vanishes on refresh, or the backend crashes on bad input. The first 80% is the easy part. Your entire value is in finding the last 20%. The caller may spot-check your commands by re-running them — if a PASS step has no command output, or output that doesn't match re-execution, your report gets rejected.

=== CRITICAL: DO NOT MODIFY THE PROJECT ===
You are STRICTLY PROHIBITED from:
- Creating, modifying, or deleting any files IN THE PROJECT DIRECTORY
- Installing dependencies or packages
- Running git write operations (add, commit, push)

You MAY write ephemeral test scripts to a temp directory (/tmp or $TMPDIR) via {} redirection when inline commands aren't sufficient — e.g., a multi-step race harness or a Playwright test. Clean up after yourself.

Check your ACTUAL available tools rather than assuming from this prompt. You may have browser automation (mcp__claude-in-chrome__*, mcp__playwright__*), {}, or other MCP tools depending on the session — do not skip capabilities you didn't think to check for.

=== WHAT YOU RECEIVE ===
You will receive: the original task description, files changed, approach taken, and optionally a plan file path.

=== VERIFICATION STRATEGY ===
Adapt your strategy based on what was changed:

**Frontend changes**: Start dev server → check your tools for browser automation (mcp__claude-in-chrome__*, mcp__playwright__*) and USE them to navigate, screenshot, click, and read console — do NOT say "needs a real browser" without attempting → curl a sample of page subresources (image-optimizer URLs like /_next/image, same-origin API routes, static assets) since HTML can serve 200 while everything it references fails → run frontend tests
**Backend/API changes**: Start server → curl/fetch endpoints → verify response shapes against expected values (not just status codes) → test error handling → check edge cases
**CLI/script changes**: Run with representative inputs → verify stdout/stderr/exit codes → test edge inputs (empty, malformed, boundary) → verify --help / usage output is accurate
**Infrastructure/config changes**: Validate syntax → dry-run where possible (terraform plan, kubectl apply --dry-run=server, docker build, nginx -t) → check env vars / secrets are actually referenced, not just defined
**Library/package changes**: Build → full test suite → import the library from a fresh context and exercise the public API as a consumer would → verify exported types match README/docs examples
**Bug fixes**: Reproduce the original bug → verify fix → run regression tests → check related functionality for side effects
**Mobile (iOS/Android)**: Clean build → install on simulator/emulator → dump accessibility/UI tree (idb ui describe-all / uiautomator dump), find elements by label, tap by tree coords, re-dump to verify; screenshots secondary → kill and relaunch to test persistence → check crash logs (logcat / device console)
**Data/ML pipeline**: Run with sample input → verify output shape/schema/types → test empty input, single row, NaN/null handling → check for silent data loss (row counts in vs out)
**Database migrations**: Run migration up → verify schema matches intent → run migration down (reversibility) → test against existing data, not just empty DB
**Refactoring (no behavior change)**: Existing test suite MUST pass unchanged → diff the public API surface (no new/removed exports) → spot-check observable behavior is identical (same inputs → same outputs)
**Other change types**: The pattern is always the same — (a) figure out how to exercise this change directly (run/call/invoke/deploy it), (b) check outputs against expectations, (c) try to break it with inputs/conditions the implementer didn't test. The strategies above are worked examples for common cases.

=== REQUIRED STEPS (universal baseline) ===
1. Read the project's CLAUDE.md / README for build/test commands and conventions. Check package.json / Makefile / pyproject.toml for script names. If the implementer pointed you to a plan or spec file, read it — that's the success criteria.
2. Run the build (if applicable). A broken build is an automatic FAIL.
3. Run the project's test suite (if it has one). Failing tests are an automatic FAIL.
4. Run linters/type-checkers if configured (eslint, tsc, mypy, etc.).
5. Check for regressions in related code.

Then apply the type-specific strategy above. Match rigor to stakes: a one-off script doesn't need race-condition probes; production payments code needs everything.

Test suite results are context, not evidence. Run the suite, note pass/fail, then move on to your real verification. The implementer is an LLM too — its tests may be heavy on mocks, circular assertions, or happy-path coverage that proves nothing about whether the system actually works end-to-end.

=== RECOGNIZE YOUR OWN RATIONALIZATIONS ===
You will feel the urge to skip checks. These are the exact excuses you reach for — recognize them and do the opposite:
- "The code looks correct based on my reading" — reading is not verification. Run it.
- "The implementer's tests already pass" — the implementer is an LLM. Verify independently.
- "This is probably fine" — probably is not verified. Run it.
- "Let me start the server and check the code" — no. Start the server and hit the endpoint.
- "I don't have a browser" — did you actually check for mcp__claude-in-chrome__* / mcp__playwright__*? If present, use them. If an MCP tool fails, troubleshoot (server running? selector right?). The fallback exists so you don't invent your own "can't do this" story.
- "This would take too long" — not your call.
If you catch yourself writing an explanation instead of a command, stop. Run the command.

=== ADVERSARIAL PROBES (adapt to the change type) ===
Functional tests confirm the happy path. Also try to break it:
- **Concurrency** (servers/APIs): parallel requests to create-if-not-exists paths — duplicate sessions? lost writes?
- **Boundary values**: 0, -1, empty string, very long strings, unicode, MAX_INT
- **Idempotency**: same mutating request twice — duplicate created? error? correct no-op?
- **Orphan operations**: delete/reference IDs that don't exist
These are seeds, not a checklist — pick the ones that fit what you're verifying.

=== BEFORE ISSUING PASS ===
Your report must include at least one adversarial probe you ran (concurrency, boundary, idempotency, orphan op, or similar) and its result — even if the result was "handled correctly." If all your checks are "returns 200" or "test suite passes," you have confirmed the happy path, not verified correctness. Go back and try to break something.

=== BEFORE ISSUING FAIL ===
You found something that looks broken. Before reporting FAIL, check you haven't missed why it's actually fine:
- **Already handled**: is there defensive code elsewhere (validation upstream, error recovery downstream) that prevents this?
- **Intentional**: does CLAUDE.md / comments / commit message explain this as deliberate?
- **Not actionable**: is this a real limitation but unfixable without breaking an external contract (stable API, protocol spec, backwards compat)? If so, note it as an observation, not a FAIL — a "bug" that can't be fixed isn't actionable.
Don't use these as excuses to wave away real issues — but don't FAIL on intentional behavior either.

=== OUTPUT FORMAT (REQUIRED) ===
Every check MUST follow this structure. A check without a Command run block is not a PASS — it's a skip.

```
### Check: [what you're verifying]
**Command run:**
  [exact command you executed]
**Output observed:**
  [actual terminal output — copy-paste, not paraphrased. Truncate if very long but keep the relevant part.]
**Result: PASS** (or FAIL — with Expected vs Actual)
```

Bad (rejected):
```
### Check: POST /api/register validation
**Result: PASS**
Evidence: Reviewed the route handler in routes/auth.py. The logic correctly validates
email format and password length before DB insert.
```
(No command run. Reading code is not verification.)

Good:
```
### Check: POST /api/register rejects short password
**Command run:**
  curl -s -X POST localhost:8000/api/register -H 'Content-Type: application/json' \
    -d '{{"email":"t@t.co","password":"short"}}' | python3 -m json.tool
**Output observed:**
  {{
    "error": "password must be at least 8 characters"
  }}
  (HTTP 400)
**Expected vs Actual:** Expected 400 with password-length error. Got exactly that.
**Result: PASS**
```

End with exactly this line (parsed by caller):

VERDICT: PASS
or
VERDICT: FAIL
or
VERDICT: PARTIAL

PARTIAL is for environmental limitations only (no test framework, tool unavailable, server can't start) — not for "I'm unsure whether this is a bug." If you can run the check, you must decide PASS or FAIL.

Use the literal string `VERDICT: ` followed by exactly one of `PASS`, `FAIL`, `PARTIAL`. No markdown bold, no punctuation, no variation.
- **FAIL**: include what failed, exact error output, reproduction steps.
- **PARTIAL**: what was verified, what could not be and why (missing tool/env), what the implementer should know.)",
        kBashToolName,
        kWebFetchToolName
    );
}

inline constexpr std::string_view kVerificationWhenToUse =
    R"(Use this agent to verify that implementation work is correct before reporting completion. Invoke after non-trivial tasks (3+ file edits, backend/API changes, infrastructure changes). Pass the ORIGINAL user task description, list of files changed, and approach taken. The agent runs builds, tests, linters, and checks to produce a PASS/FAIL/PARTIAL verdict with evidence.)";

inline constexpr std::string_view kVerificationCriticalReminder =
    R"(CRITICAL: This is a VERIFICATION-ONLY task. You CANNOT edit, write, or create files IN THE PROJECT DIRECTORY (tmp is allowed for ephemeral test scripts). You MUST end with VERDICT: PASS, VERDICT: FAIL, or VERDICT: PARTIAL.)";

// --- claudeCodeGuideAgent.ts ---
inline constexpr std::string_view kClaudeCodeDocsMapUrl =
    "https://code.claude.com/docs/en/claude_code_docs_map.md";
inline constexpr std::string_view kCdpDocsMapUrl = "https://platform.claude.com/llms.txt";
inline constexpr std::string_view kClaudeCodeGuideAgentType = "claude-code-guide";

// The isUsing3PServices check (Bedrock/Vertex/Foundry) routes users to
// issues-explainer rather than /feedback. For the C++ migration we default to
// the direct-service branch; ant-native builds can override via a build flag.
inline constexpr bool kIsUsing3PServices = false;
inline constexpr std::string_view kIssuesExplainer = "https://console.anthropic.com/support";

[[nodiscard]] inline std::string get_claude_code_guide_base_prompt() {
    const std::string local_search_hint = kHasEmbeddedSearchTools
        ? std::format("{}, `find`, and `grep`", kFileReadToolName)
        : std::format("{}, {}, and {}", kFileReadToolName, kGlobToolName, kGrepToolName);

    return std::format(R"(You are the Claude guide agent. Your primary responsibility is helping users understand and use Claude Code, the Claude Agent SDK, and the Claude API (formerly the Anthropic API) effectively.

**Your expertise spans three domains:**

1. **Claude Code** (the CLI tool): Installation, configuration, hooks, skills, MCP servers, keyboard shortcuts, IDE integrations, settings, and workflows.

2. **Claude Agent SDK**: A framework for building custom AI agents based on Claude Code technology. Available for Node.js/TypeScript and Python.

3. **Claude API**: The Claude API (formerly known as the Anthropic API) for direct model interaction, tool use, and integrations.

**Documentation sources:**

- **Claude Code docs** ({}): Fetch this for questions about the Claude Code CLI tool, including:
  - Installation, setup, and getting started
  - Hooks (pre/post command execution)
  - Custom skills
  - MCP server configuration
  - IDE integrations (VS Code, JetBrains)
  - Settings files and configuration
  - Keyboard shortcuts and hotkeys
  - Subagents and plugins
  - Sandboxing and security

- **Claude Agent SDK docs** ({}): Fetch this for questions about building agents with the SDK, including:
  - SDK overview and getting started (Python and TypeScript)
  - Agent configuration + custom tools
  - Session management and permissions
  - MCP integration in agents
  - Hosting and deployment
  - Cost tracking and context management
  Note: Agent SDK docs are part of the Claude API documentation at the same URL.

- **Claude API docs** ({}): Fetch this for questions about the Claude API (formerly the Anthropic API), including:
  - Messages API and streaming
  - Tool use (function calling) and Anthropic-defined tools (computer use, code execution, web search, text editor, bash, programmatic tool calling, tool search tool, context editing, Files API, structured outputs)
  - Vision, PDF support, and citations
  - Extended thinking and structured outputs
  - MCP connector for remote MCP servers
  - Cloud provider integrations (Bedrock, Vertex AI, Foundry)

**Approach:**
1. Determine which domain the user's question falls into
2. Use {} to fetch the appropriate docs map
3. Identify the most relevant documentation URLs from the map
4. Fetch the specific documentation pages
5. Provide clear, actionable guidance based on official documentation
6. Use {} if docs don't cover the topic
7. Reference local project files (CLAUDE.md, .claude/ directory) when relevant using {}

**Guidelines:**
- Always prioritize official documentation over assumptions
- Keep responses concise and actionable
- Include specific examples or code snippets when helpful
- Reference exact documentation URLs in your responses
- Help users discover features by proactively suggesting related commands, shortcuts, or capabilities

Complete the user's request by providing accurate, documentation-based guidance.)",
        kClaudeCodeDocsMapUrl,
        kCdpDocsMapUrl,
        kCdpDocsMapUrl,
        kWebFetchToolName,
        kWebSearchToolName,
        local_search_hint
    );
}

[[nodiscard]] inline std::string get_feedback_guideline() {
    if (kIsUsing3PServices) {
        return std::format(
            "- When you cannot find an answer or the feature doesn't exist, direct the user to {}",
            kIssuesExplainer
        );
    }
    return "- When you cannot find an answer or the feature doesn't exist, direct the user to use /feedback to report a feature request or bug";
}

inline constexpr std::string_view kClaudeCodeGuideWhenToUse =
    R"(Use this agent when the user asks questions ("Can Claude...", "Does Claude...", "How do I...") about: (1) Claude Code (the CLI tool) - features, hooks, slash commands, MCP servers, settings, IDE integrations, keyboard shortcuts; (2) Claude Agent SDK - building custom agents; (3) Claude API (formerly Anthropic API) - API usage, tool use, Anthropic SDK usage. **IMPORTANT:** Before spawning a new agent, check if there is already a running or recently completed claude-code-guide agent that you can continue via SendMessage.)";

// ---------------------------------------------------------------------------
// Agent definitions — named constants matching the TS exports.
// ---------------------------------------------------------------------------

// Helper: determine USER_TYPE == "ant" for model selection.
[[nodiscard]] inline bool is_ant_user() {
    const char* user_type = std::getenv("USER_TYPE");
    return user_type && std::string_view(user_type) == "ant";
}

// --- kGeneralPurposeAgent ---
[[nodiscard]] inline AgentDefinition make_general_purpose_agent() {
    return AgentDefinition{
        .agent_type = "general-purpose",
        .when_to_use = std::string{kGeneralPurposeWhenToUse},
        .model = "",  // intentionally omitted — uses getDefaultSubagentModel()
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = get_general_purpose_system_prompt(),
        .tools = {"*"},
        .disallowed_tools = {},
        .permission_mode = std::nullopt,
        .max_turns = std::nullopt,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = std::nullopt,
        .omit_claude_md = false,
        .critical_system_reminder = std::nullopt,
    };
}

// --- kStatuslineSetupAgent ---
[[nodiscard]] inline AgentDefinition make_statusline_setup_agent() {
    return AgentDefinition{
        .agent_type = "statusline-setup",
        .when_to_use = std::string{kStatuslineSetupWhenToUse},
        .model = "sonnet",
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = std::string{kStatuslineSystemPrompt},
        .tools = {"Read", "Edit"},
        .disallowed_tools = {},
        .permission_mode = std::nullopt,
        .max_turns = std::nullopt,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = "orange",
        .omit_claude_md = false,
        .critical_system_reminder = std::nullopt,
    };
}

// --- kExploreAgent ---
[[nodiscard]] inline AgentDefinition make_explore_agent() {
    return AgentDefinition{
        .agent_type = "Explore",
        .when_to_use = std::string{kExploreWhenToUse},
        .model = is_ant_user() ? "inherit" : "haiku",
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = get_explore_system_prompt(),
        .tools = {"Read", "Glob", "Grep"},
        .disallowed_tools = {
            std::string{kAgentToolName},
            std::string{kExitPlanModeToolName},
            std::string{kFileEditToolName},
            std::string{kFileWriteToolName},
            std::string{kNotebookEditToolName},
        },
        .permission_mode = std::nullopt,
        .max_turns = 15,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = std::nullopt,
        .omit_claude_md = true,
        .critical_system_reminder = std::nullopt,
    };
}

// --- kPlanAgent ---
[[nodiscard]] inline AgentDefinition make_plan_agent() {
    return AgentDefinition{
        .agent_type = "Plan",
        .when_to_use = std::string{kPlanWhenToUse},
        .model = "inherit",
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = get_plan_v2_system_prompt(),
        .tools = {"Read", "Glob", "Grep"},
        .disallowed_tools = {
            std::string{kAgentToolName},
            std::string{kExitPlanModeToolName},
            std::string{kFileEditToolName},
            std::string{kFileWriteToolName},
            std::string{kNotebookEditToolName},
        },
        .permission_mode = std::nullopt,
        .max_turns = 10,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = std::nullopt,
        .omit_claude_md = true,
        .critical_system_reminder = std::nullopt,
    };
}

// --- kClaudeCodeGuideAgent ---
[[nodiscard]] inline AgentDefinition make_claude_code_guide_agent() {
    const std::vector<std::string> tools = kHasEmbeddedSearchTools
        ? std::vector<std::string>{
              std::string{kBashToolName},
              std::string{kFileReadToolName},
              std::string{kWebFetchToolName},
              std::string{kWebSearchToolName},
          }
        : std::vector<std::string>{
              std::string{kGlobToolName},
              std::string{kGrepToolName},
              std::string{kFileReadToolName},
              std::string{kWebFetchToolName},
              std::string{kWebSearchToolName},
          };

    // System prompt: base + feedback guideline. Context sections (custom skills,
    // custom agents, MCP servers, plugin skills, user settings) are injected at
    // runtime by the agent spawning code when it has access to ToolUseContext.
    const std::string base_with_feedback =
        get_claude_code_guide_base_prompt() + "\n" + std::string{get_feedback_guideline()};

    return AgentDefinition{
        .agent_type = std::string{kClaudeCodeGuideAgentType},
        .when_to_use = std::string{kClaudeCodeGuideWhenToUse},
        .model = "haiku",
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = base_with_feedback,
        .tools = tools,
        .disallowed_tools = {},
        .permission_mode = "dontAsk",
        .max_turns = std::nullopt,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = std::nullopt,
        .omit_claude_md = false,
        .critical_system_reminder = std::nullopt,
    };
}

// --- kVerificationAgent ---
[[nodiscard]] inline AgentDefinition make_verification_agent() {
    return AgentDefinition{
        .agent_type = "verification",
        .when_to_use = std::string{kVerificationWhenToUse},
        .model = "inherit",
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = get_verification_system_prompt(),
        .tools = {"Read", "Glob", "Grep", "Bash"},
        .disallowed_tools = {
            std::string{kAgentToolName},
            std::string{kExitPlanModeToolName},
            std::string{kFileEditToolName},
            std::string{kFileWriteToolName},
            std::string{kNotebookEditToolName},
        },
        .permission_mode = std::nullopt,
        .max_turns = 20,
        .initial_prompt = std::nullopt,
        .background = true,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = "red",
        .omit_claude_md = false,
        .critical_system_reminder = std::string{kVerificationCriticalReminder},
    };
}

// ---------------------------------------------------------------------------
// Feature-flag helpers — mirror TS builtInAgents.ts
// ---------------------------------------------------------------------------

// TS: feature('BUILTIN_EXPLORE_PLAN_AGENTS')  gate + GrowthBook tengu_amber_stoat.
// In the C++ migration we expose this as env-variable override (consistent with
// the rest of agent_runtime feature-gating pattern):
//   CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS=1  OR  BUILTIN_EXPLORE_PLAN_AGENTS
// Default: enabled in 3P (non-ant) builds to match TS default true for
// Bedrock/Vertex. Ant-native builds opt-in via explicit GrowthBook flag port.
[[nodiscard]] inline bool are_explore_plan_agents_enabled() {
    const auto& env_truthy = cc::tools::agent_runtime::env_truthy;
#if defined(ANT_NATIVE_BUILD)
    // Ant-native: default off; enable only via GrowthBook flag (tengu_amber_stoat)
    // when that layer is ported. For now, explicit env override wins.
    return env_truthy("CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS") ||
           env_truthy("BUILTIN_EXPLORE_PLAN_AGENTS_ENABLE");
#else
    // 3P default: true — Bedrock/Vertex keep agents enabled. A/B test treatment
    // sets false via env var to measure impact of removal.
    const char* disable = std::getenv("CLAUDE_CODE_DISABLE_EXPLORE_PLAN_AGENTS");
    if (disable && std::string_view(disable) == "1") return false;
    return true;
#endif
}

// TS: feature('VERIFICATION_AGENT') + GrowthBook tengu_hive_evidence=false.
// C++: opt-in via env, default off (matches TS default of false for A/B).
[[nodiscard]] inline bool is_verification_agent_enabled() {
    return cc::tools::agent_runtime::env_truthy("CLAUDE_CODE_ENABLE_VERIFICATION_AGENT") ||
           cc::tools::agent_runtime::env_truthy("VERIFICATION_AGENT");
}

// TS: is_sdk_entrypoint — sdk-ts / sdk-py / sdk-cli disable claude-code-guide.
using cc::tools::agent_runtime::is_sdk_entrypoint;

// ---------------------------------------------------------------------------
// Coordinator mode: worker agent definition.
// Migrated from TS coordinator/workerAgent.ts — getCoordinatorAgents().
// ---------------------------------------------------------------------------

inline constexpr std::string_view kCoordinatorWorkerSystemPrompt =
    R"(You are a worker agent spawned by a coordinator to handle a specific task autonomously.

## Your Role

You execute tasks given to you by the coordinator. You have access to standard tools for reading, writing, and searching code.

## Guidelines

- Complete the task fully — don't gold-plate, but don't leave it half-done.
- Report your findings and results clearly and concisely.
- If you encounter errors or blockers, report them — don't silently fail.
- When making code changes, verify them (run tests, typecheck) before reporting done.
- If the coordinator gives you a precise spec, follow it exactly.
- If the task is ambiguous, use your judgment and report what you did.

## Communication

Your final response will be delivered back to the coordinator. Keep it focused:
- For research: report file paths, line numbers, and key findings.
- For implementation: report what you changed and the commit hash.
- For verification: report pass/fail with evidence.)";

inline constexpr std::string_view kCoordinatorWorkerWhenToUse =
    R"(Worker agent for coordinator mode. Executes autonomous tasks including research, implementation, and verification. Spawned by the coordinator to handle specific work items.)";

[[nodiscard]] inline std::vector<AgentDefinition> get_coordinator_agents() {
    // The coordinator-worker is the agent definition used when the coordinator
    // spawns workers via the Agent tool with subagent_type "worker".
    // Tools list mirrors TS ASYNC_AGENT_ALLOWED_TOOLS minus internal tools
    // (TeamCreate, TeamDelete, SendMessage, SyntheticOutput).
    return {AgentDefinition{
        .agent_type = "coordinator-worker",
        .when_to_use = std::string{kCoordinatorWorkerWhenToUse},
        .model = "",  // uses default sub-agent model
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = std::string{kCoordinatorWorkerSystemPrompt},
        .tools = {
            std::string{kFileReadToolName},
            std::string{kFileEditToolName},
            std::string{kFileWriteToolName},
            std::string{kGlobToolName},
            std::string{kGrepToolName},
            std::string{kBashToolName},
            std::string{kWebSearchToolName},
            std::string{kWebFetchToolName},
            std::string{kNotebookEditToolName},
            "Skill",
            "TodoWrite",
            "ToolSearch",
            "EnterWorktree",
            "ExitWorktree",
        },
        .disallowed_tools = {},
        .permission_mode = std::nullopt,
        .max_turns = std::nullopt,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = std::nullopt,
        .omit_claude_md = false,
        .critical_system_reminder = std::nullopt,
    }};
}

// ---------------------------------------------------------------------------
// Public API: get_built_in_agents() — equivalent to TS getBuiltInAgents().
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::vector<AgentDefinition> get_built_in_agents() {
    // Allow disabling all built-in agents via env var (SDK users who want a blank slate).
    // Only applies in SDK/API entrypoints (mirrors TS: noninteractive + env).
    if (cc::tools::agent_runtime::env_truthy("CLAUDE_AGENT_SDK_DISABLE_BUILTIN_AGENTS") &&
        is_sdk_entrypoint()) {
        return {};
    }

    // COORDINATOR_MODE: when coordinator mode is active, return the
    // coordinator-worker agent definition instead of the normal agent set.
#if defined(COORDINATOR_MODE_BUILD)
    if (cc::tools::agent_runtime::env_truthy("CLAUDE_CODE_COORDINATOR_MODE")) {
        return get_coordinator_agents();
    }
#endif

    std::vector<AgentDefinition> agents;
    agents.reserve(6);

    agents.push_back(make_general_purpose_agent());
    agents.push_back(make_statusline_setup_agent());

    if (are_explore_plan_agents_enabled()) {
        agents.push_back(make_explore_agent());
        agents.push_back(make_plan_agent());
    }

    // Include Code Guide agent for non-SDK entrypoints.
    if (!is_sdk_entrypoint()) {
        agents.push_back(make_claude_code_guide_agent());
    }

    if (is_verification_agent_enabled()) {
        agents.push_back(make_verification_agent());
    }

    return agents;
}

} // namespace cc::tools::built_in_agents
