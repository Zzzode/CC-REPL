// Implementation unit for cc.tools.agent_runtime — built-in agent prompt
// builders and their constants (builtin_detail), plugin component
// discovery/qualification, directory/settings/flag/policy agent loading,
// the get_all/find definition union, and teammate-identity prompt assembly.
// This is the only unit that imports cc.utils.team_helpers.
module;

#include <cstdlib>

module cc.tools.agent_runtime;

import std;

import cc.utils.json;
import cc.utils.team_helpers;
import cc.utils.yaml;

namespace cc::tools::agent_runtime {

// --- built-in agent system prompt and configuration constants -------------
// Migrated from src/tools/AgentTool/built-in/*.ts (Agent 1 migration).
//
// Inline implementations live here (rather than in built_in_agents.cppm) to
// avoid a circular module import: built_in_agents.cppm already imports
// agent_runtime for the AgentDefinition type. Callers outside agent_runtime
// should use cc::tools::built_in_agents::get_built_in_agents() which returns
// equivalent definitions.


namespace builtin_detail {

// Tool-name constants aligned with the TS prompt strings.
inline constexpr std::string_view kBash = "Bash";
inline constexpr std::string_view kRead = "Read";
inline constexpr std::string_view kEdit = "Edit";
inline constexpr std::string_view kWrite = "Write";
inline constexpr std::string_view kGlob = "Glob";
inline constexpr std::string_view kGrep = "Grep";
inline constexpr std::string_view kNotebookEdit = "NotebookEdit";
inline constexpr std::string_view kExitPlanMode = "ExitPlanMode";
inline constexpr std::string_view kAgent = "Agent";
inline constexpr std::string_view kWebFetch = "WebFetch";
inline constexpr std::string_view kWebSearch = "WebSearch";
inline constexpr std::string_view kSendMessage = "SendMessage";

// For the open-source C++ port we use the dedicated Glob/Grep tool path (the
// ant-native embedded-search branch uses find/grep aliases via Bash).
inline constexpr bool kEmbeddedSearch = false;

[[nodiscard]] inline bool is_ant() {
    const char* v = std::getenv("USER_TYPE");
    return v && std::string_view(v) == "ant";
}

// ---- general-purpose ----
inline constexpr std::string_view kGpPrefix =
    R"(You are an agent for Loom, a personal AI coding assistant. Given the user's message, you should use the tools available to complete the task. Complete the task fully—don't gold-plate, but don't leave it half-done.)";
inline constexpr std::string_view kGpGuidelines =
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

[[nodiscard]] inline std::string gp_prompt() {
    return std::format(
        "{} When you complete the task, respond with a concise report covering what was done and any key findings — the caller will relay this to the user, so it only needs the essentials.\n\n{}",
        kGpPrefix, kGpGuidelines
    );
}

inline constexpr std::string_view kGpWhen =
    "General-purpose agent for researching complex questions, searching for code, and executing multi-step tasks. When you are searching for a keyword or file and are not confident that you will find the right match in the first few tries use this agent to perform the search for you.";

// ---- explore ----
[[nodiscard]] inline std::string explore_prompt() {
    const bool emb = kEmbeddedSearch;
    const auto glob_s = emb ? std::format("- Use `find` via {} for broad file pattern matching", kBash)
                            : std::format("- Use {} for broad file pattern matching", kGlob);
    const auto grep_s = emb ? std::format("- Use `grep` via {} for searching file contents with regex", kBash)
                            : std::format("- Use {} for searching file contents with regex", kGrep);
    const std::string bash_tail = emb ? ", grep" : "";
    return std::format(
        R"(You are a file search specialist for Loom, a personal AI coding assistant. You excel at thoroughly navigating and exploring codebases.

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
        glob_s, grep_s, kRead, kBash, bash_tail, kBash
    );
}

inline constexpr std::string_view kExploreWhen =
    "Fast agent specialized for exploring codebases. Use this when you need to quickly find files by patterns (eg. \"src/components/**/*.tsx\"), search code for keywords (eg. \"API endpoints\"), or answer questions about the codebase (eg. \"how do API endpoints work?\"). When calling this agent, specify the desired thoroughness level: \"quick\" for basic searches, \"medium\" for moderate exploration, or \"very thorough\" for comprehensive analysis across multiple locations and naming conventions.";

// ---- plan ----
[[nodiscard]] inline std::string plan_prompt() {
    const auto search = kEmbeddedSearch
        ? std::format("`find`, `grep`, and {}", kRead)
        : std::format("{}, {}, and {}", kGlob, kGrep, kRead);
    const std::string bash_tail = kEmbeddedSearch ? ", grep" : "";
    return std::format(
        R"(You are a software architect and planning specialist for Loom. Your role is to explore the codebase and design implementation plans.

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
        search, kBash, bash_tail, kBash
    );
}

inline constexpr std::string_view kPlanWhen =
    "Software architect agent for designing implementation plans. Use this when you need to plan the implementation strategy for a task. Returns step-by-step plans, identifies critical files, and considers architectural trade-offs.";

// ---- statusline-setup ----
inline constexpr std::string_view kStatuslinePrompt =
    R"SYS_1(You are a status line setup agent for Loom. Your job is to create or update the statusLine command in the user's Loom settings.

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

   To display context remaining percentage:
   - input=$(cat); remaining=$(echo "$input" | jq -r '.context_window.remaining_percentage // empty'); [ -n "$remaining" ] && echo "Context: $remaining% remaining"

2. For longer commands, save a new file in ~/.loom, e.g. ~/.loom/statusline-command.sh, and reference it in settings.

3. Update the user's ~/.loom/settings.json with:
   { "statusLine": { "type": "command", "command": "your_command_here" } }

4. If ~/.loom/settings.json is a symlink, update the target file instead.

Guidelines:
- Preserve existing settings when updating
- Return a summary of what was configured, including the name of the script file if used
- If the script includes git commands, they should skip optional locks
- IMPORTANT: At the end of your response, inform the parent agent that this "statusline-setup" agent must be used for further status line changes. Also ensure that the user is informed that they can ask Loom to continue to make changes to the status line.)SYS_1";

inline constexpr std::string_view kStatuslineWhen =
    "Use this agent to configure the user's Loom status line setting.";

// ---- verification ----
[[nodiscard]] inline std::string verification_prompt() {
    return std::format(
        R"(You are a verification specialist. Your job is not to confirm the implementation works — it's to try to break it.

You have two documented failure patterns. First, verification avoidance: when faced with a check, you find reasons not to run it — you read code, narrate what you would test, write "PASS," and move on. Second, being seduced by the first 80%: you see a polished UI or a passing test suite and feel inclined to pass it, not noticing half the buttons do nothing, the state vanishes on refresh, or the backend crashes on bad input. The first 80% is the easy part. Your entire value is in finding the last 20%. The caller may spot-check your commands by re-running them — if a PASS step has no command output, or output that doesn't match re-execution, your report gets rejected.

=== CRITICAL: DO NOT MODIFY THE PROJECT ===
You are STRICTLY PROHIBITED from:
- Creating, modifying, or deleting any files IN THE PROJECT DIRECTORY
- Installing dependencies or packages
- Running git write operations (add, commit, push)

You MAY write ephemeral test scripts to a temp directory (/tmp or $TMPDIR) via {} redirection when inline commands aren't sufficient. Clean up after yourself.

Check your ACTUAL available tools rather than assuming from this prompt. You may have browser automation (mcp__claude-in-chrome__*, mcp__playwright__*), {}, or other MCP tools depending on the session — do not skip capabilities you didn't think to check for.

=== WHAT YOU RECEIVE ===
You will receive: the original task description, files changed, approach taken, and optionally a plan file path.

=== VERIFICATION STRATEGY ===
Adapt your strategy based on what was changed:

**Frontend changes**: Start dev server → use browser automation tools to navigate, screenshot, click, and read console → curl sample subresources → run frontend tests
**Backend/API changes**: Start server → curl/fetch endpoints → verify response shapes → test error handling → check edge cases
**CLI/script changes**: Run with representative inputs → verify stdout/stderr/exit codes → test edge inputs → verify --help output
**Infrastructure/config changes**: Validate syntax → dry-run where possible → check env vars / secrets are referenced
**Library/package changes**: Build → full test suite → import from fresh context and exercise public API
**Bug fixes**: Reproduce the original bug → verify fix → run regression tests → check side effects
**Mobile (iOS/Android)**: Clean build → install on simulator/emulator → dump accessibility/UI tree → tap → kill/relaunch for persistence → check crash logs
**Data/ML pipeline**: Run with sample input → verify output shape/schema/types → test empty/NaN/null handling → check for silent data loss
**Database migrations**: Run migration up → verify schema → run migration down → test against existing data
**Refactoring (no behavior change)**: Test suite MUST pass unchanged → diff public API surface → spot-check observable behavior
**Other change types**: The pattern is always the same — (a) exercise the change directly, (b) check outputs against expectations, (c) try to break it.

=== REQUIRED STEPS (universal baseline) ===
1. Read the project's LOOM.md / README for build/test commands and conventions.
2. Run the build (if applicable). A broken build is an automatic FAIL.
3. Run the project's test suite (if it has one). Failing tests are an automatic FAIL.
4. Run linters/type-checkers if configured.
5. Check for regressions in related code.

=== RECOGNIZE YOUR OWN RATIONALIZATIONS ===
- "The code looks correct based on my reading" → reading is not verification. Run it.
- "The implementer's tests already pass" → verify independently.
- "This is probably fine" → probably is not verified. Run it.
- "Let me start the server and check the code" → start the server and hit the endpoint.
- "I don't have a browser" → check for MCP browser tools first; use the fallback.
- "This would take too long" → not your call.

=== ADVERSARIAL PROBES (adapt to change type) ===
- **Concurrency**: parallel requests to create-if-not-exists paths
- **Boundary values**: 0, -1, empty string, very long strings, unicode, MAX_INT
- **Idempotency**: same mutating request twice
- **Orphan operations**: delete/reference IDs that don't exist

=== BEFORE ISSUING PASS ===
Your report must include at least one adversarial probe and its result.

=== OUTPUT FORMAT (REQUIRED) ===
Every check MUST follow this structure:

```
### Check: [what you're verifying]
**Command run:**
  [exact command]
**Output observed:**
  [actual terminal output]
**Result: PASS** (or FAIL — Expected vs Actual)
```

End with exactly this line (parsed by caller):

VERDICT: PASS / VERDICT: FAIL / VERDICT: PARTIAL

Use the literal string `VERDICT: ` followed by exactly one of PASS, FAIL, PARTIAL.
- **FAIL**: include what failed, exact error output, reproduction steps.
- **PARTIAL**: environmental limitations only.)",
        kBash, kWebFetch
    );
}

inline constexpr std::string_view kVerificationWhen =
    "Use this agent to verify that implementation work is correct before reporting completion. Invoke after non-trivial tasks (3+ file edits, backend/API changes, infrastructure changes). Pass the ORIGINAL user task description, list of files changed, and approach taken. The agent runs builds, tests, linters, and checks to produce a PASS/FAIL/PARTIAL verdict with evidence.";

inline constexpr std::string_view kVerificationReminder =
    "CRITICAL: This is a VERIFICATION-ONLY task. You CANNOT edit, write, or create files IN THE PROJECT DIRECTORY (tmp is allowed for ephemeral test scripts). You MUST end with VERDICT: PASS, VERDICT: FAIL, or VERDICT: PARTIAL.";

// ---- loom-guide ----
// Empty: no documentation host is shipped (see cc.constants.prompts).
inline constexpr std::string_view kCcdocsMap = "";
inline constexpr std::string_view kCdpDocsMap = "";

[[nodiscard]] inline std::string guide_base_prompt() {
    const auto local = kEmbeddedSearch
        ? std::format("{}, `find`, and `grep`", kRead)
        : std::format("{}, {}, and {}", kRead, kGlob, kGrep);
    return std::format(
        R"(You are the Loom guide agent. Your primary responsibility is helping users understand and use Loom, the Loom Agent SDK, and the Loom API (formerly the Anthropic API) effectively.

**Your expertise spans three domains:**

1. **Loom** (the CLI tool): Installation, configuration, hooks, skills, MCP servers, keyboard shortcuts, IDE integrations, settings, and workflows.
2. **Loom Agent SDK**: Framework for building custom AI agents. Node.js/TypeScript and Python.
3. **Loom API**: Direct model interaction, tool use, and integrations.

**Documentation sources:**

- **Loom docs** ({}): Install/setup, hooks, skills, MCP, IDE integrations, settings, shortcuts, subagents, plugins, sandboxing.
- **Loom Agent SDK docs** ({}): SDK overview, agent config + custom tools, session management, permissions, MCP integration, hosting, cost tracking.
- **Loom API docs** ({}): Messages API + streaming, tool use (computer use, code execution, web search, bash, programmatic tool calling, tool search, context editing, Files API, structured outputs), vision, PDF, citations, extended thinking, MCP connector, cloud providers (Bedrock, Vertex, Foundry).

**Approach:**
1. Determine domain
2. Use {} to fetch the docs map
3. Identify relevant URLs
4. Fetch specific pages
5. Provide clear, actionable guidance
6. Use {} if docs don't cover the topic
7. Reference local project files (LOOM.md, .loom/) using {}

**Guidelines:**
- Prioritize official documentation
- Keep responses concise and actionable
- Include specific examples / code snippets when helpful
- Reference exact URLs
- Proactively suggest related commands, shortcuts, capabilities

Complete the user's request with accurate, documentation-based guidance.)",
        kCcdocsMap, kCdpDocsMap, kCdpDocsMap,
        kWebFetch, kWebSearch, local
    );
}

inline constexpr std::string_view kGuideWhen =
    "Use this agent when the user asks questions (\"Can Loom...\", \"Does Loom...\", \"How do I...\") about: (1) Loom CLI tool - features, hooks, slash commands, MCP servers, settings, IDE integrations, keyboard shortcuts; (2) Loom Agent SDK - building custom agents; (3) Loom API - API usage, tool use, SDK usage. **IMPORTANT:** Before spawning a new agent, check if there is already a running or recently completed loom-guide agent that you can continue via SendMessage.";

} // namespace builtin_detail


[[nodiscard]] bool are_explore_plan_agents_enabled() {
#if defined(ANT_NATIVE_BUILD)
    return env_truthy("LOOM_ENABLE_EXPLORE_PLAN_AGENTS") ||
           env_truthy("BUILTIN_EXPLORE_PLAN_AGENTS");
#else
    return env_truthy("LOOM_ENABLE_EXPLORE_PLAN_AGENTS") ||
           env_truthy("BUILTIN_EXPLORE_PLAN_AGENTS");
#endif
}
[[nodiscard]] bool is_verification_agent_enabled() {
    return env_truthy("LOOM_ENABLE_VERIFICATION_AGENT") ||
           env_truthy("VERIFICATION_AGENT");
}
[[nodiscard]] std::vector<AgentDefinition> built_in_agent_definitions() {
    using namespace builtin_detail;

    if (env_truthy("LOOM_AGENT_SDK_DISABLE_BUILTIN_AGENTS") && is_sdk_entrypoint()) return {};

    std::vector<AgentDefinition> agents;
    agents.reserve(6);

    // --- general-purpose ---
    agents.push_back(AgentDefinition{
        .agent_type = "general-purpose",
        .when_to_use = std::string{kGpWhen},
        .model = "",  // uses default subagent model
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = gp_prompt(),
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
        .hooks = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = std::nullopt,
        .omit_loom_md = false,
        .critical_system_reminder = std::nullopt,
    });

    // --- statusline-setup ---
    agents.push_back(AgentDefinition{
        .agent_type = "statusline-setup",
        .when_to_use = std::string{kStatuslineWhen},
        .model = "sonnet",
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = std::string{kStatuslinePrompt},
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
        .hooks = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = "orange",
        .omit_loom_md = false,
        .critical_system_reminder = std::nullopt,
    });

    // --- Explore + Plan (feature-gated) ---
    if (are_explore_plan_agents_enabled()) {
        agents.push_back(AgentDefinition{
            .agent_type = "Explore",
            .when_to_use = std::string{kExploreWhen},
            .model = is_ant() ? "inherit" : "haiku",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = explore_prompt(),
            .tools = {"Read", "Glob", "Grep"},
            .disallowed_tools = {
                std::string{kAgent},
                std::string{kExitPlanMode},
                std::string{kEdit},
                std::string{kWrite},
                std::string{kNotebookEdit},
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
            .hooks = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = std::nullopt,
            .omit_loom_md = true,
            .critical_system_reminder = std::nullopt,
        });
        agents.push_back(AgentDefinition{
            .agent_type = "Plan",
            .when_to_use = std::string{kPlanWhen},
            .model = "inherit",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = plan_prompt(),
            .tools = {"Read", "Glob", "Grep"},
            .disallowed_tools = {
                std::string{kAgent},
                std::string{kExitPlanMode},
                std::string{kEdit},
                std::string{kWrite},
                std::string{kNotebookEdit},
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
            .hooks = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = std::nullopt,
            .omit_loom_md = true,
            .critical_system_reminder = std::nullopt,
        });
    }

    // --- loom-guide (suppressed for SDK entrypoints) ---
    if (!is_sdk_entrypoint()) {
        const std::vector<std::string> guide_tools = kEmbeddedSearch
            ? std::vector<std::string>{
                  std::string{kBash}, std::string{kRead},
                  std::string{kWebFetch}, std::string{kWebSearch}}
            : std::vector<std::string>{
                  std::string{kGlob}, std::string{kGrep}, std::string{kRead},
                  std::string{kWebFetch}, std::string{kWebSearch}};
        const std::string feedback =
            "- When you cannot find an answer or the feature doesn't exist, direct the user to use /feedback to report a feature request or bug";
        const std::string system_prompt = guide_base_prompt() + "\n" + feedback;
        agents.push_back(AgentDefinition{
            .agent_type = "loom-guide",
            .when_to_use = std::string{kGuideWhen},
            .model = "haiku",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = system_prompt,
            .tools = guide_tools,
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
            .hooks = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = std::nullopt,
            .omit_loom_md = false,
            .critical_system_reminder = std::nullopt,
        });
    }

    // --- verification (feature-gated, A/B default off) ---
    if (is_verification_agent_enabled()) {
        agents.push_back(AgentDefinition{
            .agent_type = "verification",
            .when_to_use = std::string{kVerificationWhen},
            .model = "inherit",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = verification_prompt(),
            .tools = {"Read", "Glob", "Grep", "Bash"},
            .disallowed_tools = {
                std::string{kAgent},
                std::string{kExitPlanMode},
                std::string{kEdit},
                std::string{kWrite},
                std::string{kNotebookEdit},
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
            .hooks = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = "red",
            .omit_loom_md = false,
            .critical_system_reminder = std::string{kVerificationReminder},
        });
    }

    return agents;
}
void append_existing_plugin_component_path(
    cc::utils::json::JsonVal value,
    const fs::path& plugin_dir,
    std::vector<fs::path>& out
) {
    auto append_one = [&](std::string_view raw) {
        if (raw.empty()) return;
        fs::path path{std::string(raw)};
        if (path.is_relative()) path = plugin_dir / path;
        std::error_code ec;
        if (fs::exists(path, ec)) out.push_back(std::move(path));
    };

    if (value.is_str()) {
        append_one(value.as_str());
    } else if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) append_one(item.as_str());
        });
    }
}
[[nodiscard]] std::optional<PluginComponentPaths> read_plugin_component_paths(
    const fs::path& plugin_dir
) {
    const auto manifest_path = plugin_dir / "plugin.json";
    std::ifstream input(manifest_path);
    if (!input) return std::nullopt;

    std::stringstream buffer;
    buffer << input.rdbuf();
    auto doc = cc::utils::json::parse(buffer.str());
    if (!doc) return std::nullopt;

    auto root = doc->root();
    auto name = root.get("name");
    if (!root.is_obj() || !name.is_str() || name.as_str().empty()) return std::nullopt;

    PluginComponentPaths paths{
        .plugin_name = std::string(name.as_str()),
        .plugin_dir = plugin_dir,
        .agents_paths = {},
        .skills_paths = {},
    };

    const auto default_agents = plugin_dir / "agents";
    std::error_code ec;
    if (fs::exists(default_agents, ec)) paths.agents_paths.push_back(default_agents);
    if (auto agents = root.get("agents"); agents.valid()) {
        append_existing_plugin_component_path(agents, plugin_dir, paths.agents_paths);
    }

    const auto default_skills = plugin_dir / "skills";
    if (fs::exists(default_skills, ec)) paths.skills_paths.push_back(default_skills);
    if (auto skills = root.get("skills"); skills.valid()) {
        append_existing_plugin_component_path(skills, plugin_dir, paths.skills_paths);
    }

    if (paths.agents_paths.empty() && paths.skills_paths.empty()) return std::nullopt;
    return paths;
}
[[nodiscard]] std::vector<PluginComponentPaths> discover_plugin_component_paths() {
    std::vector<PluginComponentPaths> discovered;
    std::vector<fs::path> roots;
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(fs::path{home} / ".loom" / "plugins");
    }
    roots.push_back(fs::current_path() / ".loom" / "plugins");

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            if (auto paths = read_plugin_component_paths(entry.path())) {
                discovered.push_back(std::move(*paths));
            }
        }
    }

    std::ranges::sort(discovered, {}, &PluginComponentPaths::plugin_name);
    return discovered;
}
[[nodiscard]] LoadAgentDefinitionsResult load_agent_definitions_from_dir_ex(
    const fs::path& dir,
    std::string source
) {
    LoadAgentDefinitionsResult result;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return result;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == ".md") {
            if (auto parsed = parse_agent_markdown(entry.path(), source)) {
                result.agents.push_back(std::move(*parsed));
                continue;
            }
            // Re-parse frontmatter to determine whether the file looked like
            // an agent definition attempt (has `name:`) and thus deserves a
            // diagnostic entry. Matches TS "skip non-agent reference docs
            // silently; report parse errors for files that look like agents".
            {
                std::ifstream input(entry.path());
                if (input) {
                    std::string text(
                        (std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
                    std::string_view sv(text);
                    if (sv.starts_with("---\n") || sv.starts_with("---\r\n")) {
                        auto first_nl = sv.find('\n');
                        auto fm_end = sv.find("\n---", first_nl + 1);
                        if (fm_end != std::string_view::npos) {
                            auto fm_sv = sv.substr(first_nl + 1, fm_end - first_nl - 1);
                            auto fm = cc::utils::parse_yaml(fm_sv);
                            if (const auto* fields =
                                    std::get_if<cc::utils::YamlMap>(&fm.data);
                                fields) {
                                const auto name = yaml_string_field(*fields, "name");
                                if (name && !name->empty()) {
                                    result.failed.push_back(FailedAgentFile{
                                        .path = entry.path().string(),
                                        .error = get_parse_error(*fields),
                                    });
                                }
                            }
                        }
                    }
                }
            }
        } else if (entry.path().extension() == ".json") {
            auto parsed = parse_agents_json_file(entry.path(), source);
            // migrated edge case: TS logs JSON parse failures via logForDebugging;
            // we surface the individual files that produced zero agents as a
            // coarse "failed" bucket — if the file contained valid entries we
            // always accept them (same semantics: zod schema rejects bad
            // individual objects, only errors prevent the whole file).
            if (parsed.empty()) {
                std::ifstream probe(entry.path());
                if (probe) {
                    std::stringstream buf;
                    buf << probe.rdbuf();
                    auto doc = cc::utils::json::parse(buf.str());
                    if (!doc || !doc->root().is_obj()) {
                        result.failed.push_back(FailedAgentFile{
                            .path = entry.path().string(),
                            .error = "Invalid JSON agent file: not a JSON object",
                        });
                    }
                }
            } else {
                result.agents.insert(
                    result.agents.end(),
                    std::make_move_iterator(parsed.begin()),
                    std::make_move_iterator(parsed.end()));
            }
        }
    }
    std::ranges::sort(result.agents, {}, &AgentDefinition::agent_type);
    return result;
}
[[nodiscard]] std::vector<AgentDefinition> load_agent_definitions_from_dir(
    const fs::path& dir,
    std::string source
) {
    return load_agent_definitions_from_dir_ex(dir, std::move(source)).agents;
}
[[nodiscard]] std::string qualify_plugin_component_name(
    std::string_view plugin_name,
    std::string value
) {
    if (value.empty() || value.starts_with("plugin:")) return value;
    return std::format("plugin:{}:{}", plugin_name, value);
}
void qualify_plugin_mcp_names(AgentDefinition& agent, std::string_view plugin_name) {
    for (auto& server : agent.required_mcp_servers) {
        server = qualify_plugin_component_name(plugin_name, std::move(server));
    }
    for (auto& server : agent.mcp_servers) {
        server = qualify_plugin_component_name(plugin_name, std::move(server));
    }
    for (auto& server : agent.inline_mcp_servers) {
        server.name = qualify_plugin_component_name(plugin_name, std::move(server.name));
    }
}
void append_plugin_agent_definition(
    std::vector<AgentDefinition>& agents,
    AgentDefinition agent,
    std::string_view plugin_name,
    const std::vector<std::string>& namespace_parts
) {
    std::string qualified = std::string(plugin_name);
    for (const auto& part : namespace_parts) {
        if (!part.empty()) qualified += ":" + part;
    }
    qualified += ":" + agent.agent_type;
    agent.agent_type = std::move(qualified);
    agent.source = "plugin";
    qualify_plugin_mcp_names(agent, plugin_name);
    agents.push_back(std::move(agent));
}
void load_plugin_agents_from_path(
    std::vector<AgentDefinition>& agents,
    const fs::path& path,
    std::string_view plugin_name,
    std::vector<std::string> namespace_parts
) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return;

    if (fs::is_regular_file(path, ec)) {
        if (path.extension() == ".md") {
            if (auto parsed = parse_agent_markdown(path, "plugin")) {
                append_plugin_agent_definition(agents, std::move(*parsed), plugin_name, namespace_parts);
            }
        } else if (path.extension() == ".json") {
            for (auto& parsed : parse_agents_json_file(path, "plugin")) {
                append_plugin_agent_definition(agents, std::move(parsed), plugin_name, namespace_parts);
            }
        }
        return;
    }

    if (!fs::is_directory(path, ec)) return;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            if (entry.path().extension() == ".md") {
                if (auto parsed = parse_agent_markdown(entry.path(), "plugin")) {
                    append_plugin_agent_definition(agents, std::move(*parsed), plugin_name, namespace_parts);
                }
            } else if (entry.path().extension() == ".json") {
                for (auto& parsed : parse_agents_json_file(entry.path(), "plugin")) {
                    append_plugin_agent_definition(agents, std::move(parsed), plugin_name, namespace_parts);
                }
            }
        } else if (entry.is_directory(ec)) {
            auto nested = namespace_parts;
            nested.push_back(entry.path().filename().string());
            load_plugin_agents_from_path(agents, entry.path(), plugin_name, std::move(nested));
        }
    }
}
[[nodiscard]] std::vector<AgentDefinition> load_plugin_agent_definitions() {
    std::vector<AgentDefinition> agents;
    for (const auto& plugin : discover_plugin_component_paths()) {
        for (const auto& path : plugin.agents_paths) {
            load_plugin_agents_from_path(agents, path, plugin.plugin_name);
        }
    }
    std::ranges::sort(agents, {}, &AgentDefinition::agent_type);
    return agents;
}
[[nodiscard]] std::vector<AgentDefinition> get_all_agent_definitions(
    std::optional<fs::path> cwd
) {
    std::map<std::string, AgentDefinition> by_type;
    auto active_agents = [&]() {
        std::vector<AgentDefinition> active;
        active.reserve(by_type.size());
        for (auto& [_, agent] : by_type) active.push_back(std::move(agent));
        return active;
    };

    for (auto& agent : built_in_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }
    if (env_truthy("LOOM_SIMPLE")) return active_agents();

    for (auto& agent : load_plugin_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }

    if (auto* home = std::getenv("HOME")) {
        const auto user_settings = fs::path(home) / ".loom" / "settings.json";
        for (auto& agent : load_agent_definitions_from_dir(
            fs::path(home) / ".loom" / "agents", "userSettings")) {
            by_type[agent.agent_type] = std::move(agent);
        }
        for (auto& agent : load_agent_definitions_from_settings_file(user_settings, "userSettings")) {
            by_type[agent.agent_type] = std::move(agent);
        }
    }

    const auto project_cwd = cwd.value_or(fs::current_path());
    for (auto& agent : load_agent_definitions_from_dir(
        project_cwd / ".loom" / "agents", "projectSettings")) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_agent_definitions_from_settings_file(
        project_cwd / ".loom" / "settings.json", "projectSettings")) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_agent_definitions_from_settings_file(
        project_cwd / ".loom" / "settings.local.json", "localSettings")) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_flag_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_policy_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }

    return active_agents();
}
[[nodiscard]] std::optional<AgentDefinition> find_agent_definition(
    std::string_view requested_type,
    std::optional<fs::path> cwd
) {
    auto agents = get_all_agent_definitions(std::move(cwd));
    auto resolved = resolve_requested_agent_type(requested_type, agents);
    if (!resolved) return std::nullopt;
    for (const auto& agent : agents) {
        if (agent.agent_type == *resolved) return agent;
    }
    return std::nullopt;
}
[[nodiscard]] bool has_teammate_identity() {
    auto agent_id = cc::utils::get_agent_id();
    auto agent_name = cc::utils::get_agent_name();
    auto team_name = cc::utils::get_team_name();
    return agent_id && !agent_id->empty() &&
        agent_name && !agent_name->empty() &&
        team_name && !team_name->empty();
}
[[nodiscard]] std::optional<std::string> build_teammate_append_system_prompt(
    std::optional<std::string> existing_append_prompt,
    std::optional<fs::path> cwd
) {
    if (!has_teammate_identity()) return existing_append_prompt;

    std::string append_prompt = existing_append_prompt.value_or("");
    append_prompt_section(append_prompt, teammate_system_prompt_addendum);

    if (auto agent_type = cc::utils::get_agent_type(); agent_type && !agent_type->empty()) {
        auto agent = find_agent_definition(*agent_type, std::move(cwd));
        if (agent && agent->source != "built-in" && !agent->system_prompt.empty()) {
            append_prompt_section(
                append_prompt,
                std::format("# Custom Agent Instructions\n{}", agent->system_prompt));
        }
    }

    if (append_prompt.empty()) return std::nullopt;
    return append_prompt;
}
std::expected<std::vector<std::string>, std::string> load_agents_from_dir(std::string_view dir_path) {
    std::vector<std::string> names;
    for (const auto& agent : load_agent_definitions_from_dir(fs::path(dir_path), "custom")) {
        names.push_back(agent.agent_type);
    }
    return names;
}
[[nodiscard]] LoadAgentDefinitionsResult load_agents_dir(
    const fs::path& dir,
    std::string_view source
) {
    return load_agent_definitions_from_dir_ex(dir, std::string(source));
}
} // namespace cc::tools::agent_runtime
