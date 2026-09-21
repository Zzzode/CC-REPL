module;
#include <string>
#include <string_view>
#include <map>
#include <sstream>

export module cc.tools.tool_prompts;

import cc.tools.file_edit_prompt;   // get_edit_tool_description

export namespace cc::tools {


inline auto get_tool_prompt(std::string_view tool_name) -> std::string {
    static const std::map<std::string, std::string, std::less<>> prompts = {
        {"Read", R"(Read file contents from the filesystem. Provide an absolute path.)"},
        {"Write", R"(Write content to a file. Overwrites existing content. Read the file first if it exists.)"},
        // migrated: agent9 — Edit prompt now delegates to file_edit_prompt.cppm
        // which mirrors TS getEditToolDescription() exactly.
        {"Edit", cc::tools::file_edit::get_edit_tool_description()},
        {"Glob", R"(Find files matching a glob pattern. Returns paths sorted by modification time.)"},
        {"Grep", R"(Search file contents with regex. Supports context lines and file type filters.)"},
        {"RunCommand", R"(Execute a terminal command. Use for git, build, test, and other shell operations.)"},
        {"SearchCodebase", R"(Semantic code search. Find code by meaning, not exact text.)"},
        {"WebSearch", R"(Search the web for real-time information.)"},
        {"WebFetch", R"(Fetch and parse content from a URL.)"},
        {"Agent", R"(Spawn a sub-agent for independent task execution.)"},
        {"TaskCreate", R"(Create a new tracked task.)"},
        {"TaskUpdate", R"(Update status or details of an existing task.)"},
        // migrated: BashTool prompt (from BashTool/prompt.ts getSimplePrompt).
        // Some runtime-derived sections (sandbox allowlists, attribution,
        // undercover instructions) are omitted until the underlying services
        // are ported to C++ — the static skeleton below is feature-complete
        // for the tool's own instructions.
        {"Bash", R"(Executes a given bash command and returns its output.

The working directory persists between commands, but shell state does not.
The shell environment is initialized from the user's profile (bash or zsh).

IMPORTANT: Avoid using this tool to run `find`, `grep`, `cat`, `head`, `tail`,
`sed`, `awk`, or `echo` commands, unless explicitly instructed or after you have
verified that a dedicated tool cannot accomplish your task. Instead, use the
appropriate dedicated tool:
- File search: Use Glob (NOT find or ls)
- Content search: Use Grep (NOT grep or rg)
- Read files: Use Read (NOT cat/head/tail)
- Edit files: Use Edit (NOT sed/awk)
- Write files: Use Write (NOT echo >/cat <<EOF)
- Communication: Output text directly (NOT echo/printf)

While the Bash tool can do similar things, it's better to use the built-in
tools as they provide a better user experience and make it easier to review
tool calls and give permission.

# Instructions

- If your command will create new directories or files, first use this tool
  to run `ls` to verify the parent directory exists and is the correct location.
- Always quote file paths that contain spaces with double quotes in your
  command (e.g., cd "path with spaces/file.txt").
- Try to maintain your current working directory throughout the session by
  using absolute paths and avoiding usage of `cd`. You may use `cd` if the
  User explicitly requests it.
- You may specify an optional timeout in milliseconds. By default, your
  command will timeout after 120 seconds.
- You can use the `run_in_background` parameter to run the command in the
  background. Only use this if you don't need the result immediately and
  are OK being notified when the command completes later. You do not need
  to use '&' at the end of the command when using this parameter.
- When issuing multiple commands:
  - If the commands are independent and can run in parallel, make multiple
    Bash tool calls in a single message. Example: if you need to run
    "git status" and "git diff", send two Bash tool calls in parallel.
  - If the commands depend on each other and must run sequentially, use a
    single Bash call with '&&' to chain them together.
  - Use ';' only when you need to run commands sequentially but don't care
    if earlier commands fail.
  - DO NOT use newlines to separate commands (newlines are ok in quoted
    strings).
- For git commands:
  - Prefer to create a new commit rather than amending an existing commit.
  - Before running destructive operations (e.g., git reset --hard,
    git push --force, git checkout --), consider whether there is a safer
    alternative that achieves the same goal. Only use destructive operations
    when they are truly the best approach.
  - Never skip hooks (--no-verify) or bypass signing (--no-gpg-sign,
    -c commit.gpgsign=false) unless the user has explicitly asked for it.
    If a hook fails, investigate and fix the underlying issue.
- Avoid unnecessary `sleep` commands:
  - Do not sleep between commands that can run immediately — just run them.
  - If your command is long running and you would like to be notified when
    it finishes — use `run_in_background`. No sleep needed.
  - Do not retry failing commands in a sleep loop — diagnose the root cause.
  - If waiting for a background task you started with `run_in_background`,
    you will be notified when it completes — do not poll.
  - If you must poll an external process, use a check command
    (e.g. `gh run view`) rather than sleeping first.
  - If you must sleep, keep the duration short (1-5 seconds) to avoid
    blocking the user.

# Committing changes with git

Only create commits when requested by the user. If unclear, ask first.

Git Safety Protocol:
- NEVER update the git config
- NEVER run destructive git commands (push --force, reset --hard,
  checkout ., restore ., clean -f, branch -D) unless the user explicitly
  requests these actions.
- NEVER skip hooks (--no-verify, --no-gpg-sign, etc) unless the user
  explicitly requests it.
- NEVER run force push to main/master, warn the user if they request it.
- CRITICAL: Always create NEW commits rather than amending, unless the user
  explicitly requests a git amend. When a pre-commit hook fails, the commit
  did NOT happen — so --amend would modify the PREVIOUS commit, which may
  destroy work or lose previous changes. Instead, fix the issue, re-stage,
  and create a NEW commit.
- When staging files, prefer adding specific files by name rather than using
  "git add -A" or "git add .", which can accidentally include sensitive
  files (.env, credentials) or large binaries.
- NEVER commit changes unless the user explicitly asks you to.

1. Run in parallel: `git status`, `git diff`, `git log`.
2. Analyze all staged changes and draft a concise commit message.
3. Stage relevant files, create the commit, then run `git status` to verify.
4. If the commit fails due to a pre-commit hook: fix the issue and create a
   NEW commit.

Use the gh command via the Bash tool for ALL GitHub-related tasks including
issues, pull requests, checks, and releases. If given a Github URL use the
gh command to get the information needed.)"},
    };

    auto it = prompts.find(tool_name);
    if (it != prompts.end()) {
        return it->second;
    }
    return std::string("No prompt available for tool: ") + std::string(tool_name);
}


inline auto get_all_tool_prompts() -> std::map<std::string, std::string> {
    return {
        {"Read", get_tool_prompt("Read")},
        {"Write", get_tool_prompt("Write")},
        {"Edit", get_tool_prompt("Edit")},
        {"Glob", get_tool_prompt("Glob")},
        {"Grep", get_tool_prompt("Grep")},
        {"RunCommand", get_tool_prompt("RunCommand")},
        {"SearchCodebase", get_tool_prompt("SearchCodebase")},
        {"WebSearch", get_tool_prompt("WebSearch")},
        {"WebFetch", get_tool_prompt("WebFetch")},
        {"Agent", get_tool_prompt("Agent")},
        {"TaskCreate", get_tool_prompt("TaskCreate")},
        {"TaskUpdate", get_tool_prompt("TaskUpdate")},
        {"Bash", get_tool_prompt("Bash")},
    };
}


inline auto format_tool_use_result(
    std::string_view tool_name,
    std::string_view result,
    bool is_error
) -> std::string {
    std::ostringstream oss;

    if (is_error) {
        oss << "Tool `" << tool_name << "` returned an error:\n";
        oss << result;
    } else {
        oss << result;
    }


    if (result.size() > 50000) {
        oss << "\n\n[Note: Output was truncated. "
            << result.size() << " total characters.]";
    }

    return oss.str();
}


inline auto get_tool_description(std::string_view tool_name) -> std::string {
    static const std::map<std::string, std::string, std::less<>> descriptions = {
        {"Read", "Reads file contents from the local filesystem"},
        {"Write", "Writes content to a file on the local filesystem"},
        {"Edit", "Performs exact string replacements in files"},
        {"Glob", "Fast file pattern matching with glob syntax"},
        {"Grep", "Searches file contents with regular expressions"},
        {"RunCommand", "Executes terminal commands"},
        {"SearchCodebase", "Semantic search over the codebase"},
        {"WebSearch", "Searches the web for information"},
        {"WebFetch", "Fetches content from a URL"},
        {"Agent", "Spawns a sub-agent for delegated tasks"},
        {"TaskCreate", "Creates a new tracked task"},
        {"TaskUpdate", "Updates an existing task"},
        {"LS", "Lists directory contents"},
        {"DeleteFile", "Deletes files from the filesystem"},
        {"Bash", "Executes bash commands with sandboxing, timeout, and background task support"},
    };

    auto it = descriptions.find(tool_name);
    if (it != descriptions.end()) {
        return it->second;
    }
    return std::string("Tool: ") + std::string(tool_name);
}

} // namespace cc::tools
