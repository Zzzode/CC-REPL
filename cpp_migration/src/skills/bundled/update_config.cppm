/// @file update_config.cppm
/// @brief Bundled Update Config skill with full settings schema, hooks docs,
/// and verification workflow. Delegates all config I/O to cc.config.config
/// and cc.config.feature_flags — NEVER reads/writes JSON files directly.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <format>
#include <filesystem>

export module cc.skills.bundled.update_config;

import cc.skills.skill;
import cc.config.config;
import cc.config.feature_flags;
import cc.config.settings;

export namespace cc::skills::bundled {

// ============================================================
// Settings Examples Documentation (mirrors TS SETTINGS_EXAMPLES_DOCS)
// ============================================================

constexpr std::string_view SETTINGS_EXAMPLES_DOCS = R"(## Settings File Locations

Choose the appropriate file based on scope:

| File | Scope | Git | Use For |
|------|-------|-----|---------|
| `~/.claude/settings.json` | Global | N/A | Personal preferences for all projects |
| `.claude/settings.json` | Project | Commit | Team-wide hooks, permissions, plugins |
| `.claude/settings.local.json` | Project | Gitignore | Personal overrides for this project |

Settings load in order: user -> project -> local (later overrides earlier).

## Settings Schema Reference

### Permissions
```json
{
  "permissions": {
    "allow": ["Bash(npm:*)", "Edit(.claude)", "Read"],
    "deny": ["Bash(rm -rf:*)"],
    "ask": ["Write(/etc/*)"],
    "defaultMode": "default" | "plan" | "acceptEdits" | "dontAsk",
    "additionalDirectories": ["/extra/dir"]
  }
}
```

**Permission Rule Syntax:**
- Exact match: `"Bash(npm run test)"`
- Prefix wildcard: `"Bash(git:*)"` — matches `git status`, `git commit`, etc.
- Tool only: `"Read"` — allows all Read operations

### Environment Variables
```json
{
  "env": {
    "DEBUG": "true",
    "MY_API_KEY": "value"
  }
}
```

### Model & Agent
```json
{
  "model": "sonnet",
  "agent": "agent-name",
  "alwaysThinkingEnabled": true
}
```

### Attribution (Commits & PRs)
```json
{
  "attribution": {
    "commit": "Custom commit trailer text",
    "pr": "Custom PR description text"
  }
}
```
Set `commit` or `pr` to empty string `""` to hide that attribution.

### MCP Server Management
```json
{
  "enableAllProjectMcpServers": true,
  "enabledMcpjsonServers": ["server1", "server2"],
  "disabledMcpjsonServers": ["blocked-server"]
}
```

### Plugins
```json
{
  "enabledPlugins": {
    "formatter@anthropic-tools": true
  }
}
```
Plugin syntax: `plugin-name@source` where source is `claude-code-marketplace`,
`claude-plugins-official`, or `builtin`.

### Other Settings
- `language`: Preferred response language (e.g. "japanese")
- `cleanupPeriodDays`: Days to keep transcripts (default: 30; 0 disables persistence)
- `respectGitignore`: Whether to respect .gitignore (default: true)
- `spinnerTipsEnabled`: Show tips in spinner
- `spinnerVerbs`: Customize spinner verbs (`{ "mode": "append" | "replace", "verbs": [...] }`)
- `spinnerTipsOverride`: Override spinner tips (`{ "excludeDefault": true, "tips": [...] }`)
- `syntaxHighlightingDisabled`: Disable diff highlighting
)";

// ============================================================
// Hooks Documentation (mirrors TS HOOKS_DOCS)
// ============================================================

constexpr std::string_view HOOKS_DOCS = R"(## Hooks Configuration

Hooks run commands at specific points in the harness lifecycle.

### Hook Structure
```json
{
  "hooks": {
    "EVENT_NAME": [
      {
        "matcher": "ToolName|OtherTool",
        "hooks": [
          {
            "type": "command",
            "command": "your-command-here",
            "timeout": 60,
            "statusMessage": "Running..."
          }
        ]
      }
    ]
  }
}
```

### Hook Events

| Event | Matcher | Purpose |
|-------|---------|---------|
| PermissionRequest | Tool name | Run before permission prompt |
| PreToolUse | Tool name | Run before tool, can block |
| PostToolUse | Tool name | Run after successful tool |
| PostToolUseFailure | Tool name | Run after tool fails |
| Notification | Notification type | Run on notifications |
| Stop | - | Run when Claude stops (clear, resume, compact) |
| PreCompact | "manual"/"auto" | Before compaction |
| PostCompact | "manual"/"auto" | After compaction (receives summary) |
| UserPromptSubmit | - | When user submits |
| SessionStart | - | When session starts |

**Common tool matchers:** `Bash`, `Write`, `Edit`, `Read`, `Glob`, `Grep`

### Hook Types

**1. Command Hook** - Runs a shell command:
```json
{ "type": "command", "command": "prettier --write $FILE", "timeout": 30 }
```

**2. Prompt Hook** - Evaluates a condition with LLM:
```json
{ "type": "prompt", "prompt": "Is this safe? $ARGUMENTS" }
```
Only available for tool events: PreToolUse, PostToolUse, PermissionRequest.

**3. Agent Hook** - Runs an agent with tools:
```json
{ "type": "agent", "prompt": "Verify tests pass: $ARGUMENTS" }
```
Only available for tool events: PreToolUse, PostToolUse, PermissionRequest.

### Hook Input (stdin JSON)
```json
{
  "session_id": "abc123",
  "tool_name": "Write",
  "tool_input": { "file_path": "/path/to/file.txt", "content": "..." },
  "tool_response": { "success": true }
}
```

### Hook JSON Output
Hooks return JSON to control behavior:

```json
{
  "systemMessage": "Warning shown to user in UI",
  "continue": false,
  "stopReason": "Message shown when blocking",
  "suppressOutput": false,
  "decision": "block",
  "reason": "Explanation for decision",
  "hookSpecificOutput": {
    "hookEventName": "PostToolUse",
    "additionalContext": "Context injected back to model"
  }
}
```

### Common Patterns

**Auto-format after writes:**
```json
{
  "hooks": {
    "PostToolUse": [{
      "matcher": "Write|Edit",
      "hooks": [{
        "type": "command",
        "command": "jq -r '.tool_response.filePath // .tool_input.file_path' | { read -r f; prettier --write \"$f\"; } 2>/dev/null || true"
      }]
    }]
  }
}
```

**Log all bash commands:**
```json
{
  "hooks": {
    "PreToolUse": [{
      "matcher": "Bash",
      "hooks": [{
        "type": "command",
        "command": "jq -r '.tool_input.command' >> ~/.claude/bash-log.txt"
      }]
    }]
  }
}
```

**Run tests after code changes:**
```json
{
  "hooks": {
    "PostToolUse": [{
      "matcher": "Write|Edit",
      "hooks": [{
        "type": "command",
        "command": "jq -r '.tool_input.file_path // .tool_response.filePath' | grep -E '\\.(ts|js)$' && npm test || true"
      }]
    }]
  }
}
```
)";

// ============================================================
// Hook Verification Flow (mirrors TS HOOK_VERIFICATION_FLOW)
// ============================================================

constexpr std::string_view HOOK_VERIFICATION_FLOW = R"(## Constructing a Hook (with verification)

Given an event, matcher, target file, and desired behavior, follow this flow.
Each step catches a different failure class — a hook that silently does
nothing is worse than no hook.

1. **Dedup check.** Read the target file. If a hook already exists on the same
   event+matcher, show the existing command and ask: keep it, replace it, or
   add alongside.

2. **Construct the command for THIS project — don't assume.** The hook receives
   JSON on stdin. Build a command that:
   - Extracts payload safely — use `jq -r` into a quoted variable or
     `{ read -r f; ... "$f"; }`, NOT unquoted `| xargs` (splits on spaces)
   - Invokes the underlying tool the way this project runs it
     (npx/bunx/yarn/pnpm? Makefile target? globally-installed?)
   - Skips inputs the tool doesn't handle
     (formatters often have `--ignore-unknown`)
   - Stays RAW for now — no `|| true`, no stderr suppression. Wrap after the
     pipe-test passes.

3. **Pipe-test the raw command.** Synthesize the stdin payload the hook will
   receive and pipe it directly:
   - `Pre|PostToolUse` on `Write|Edit`:
     `echo '{"tool_name":"Edit","tool_input":{"file_path":"<a real file>"}}' | <cmd>`
   - `Pre|PostToolUse` on `Bash`:
     `echo '{"tool_name":"Bash","tool_input":{"command":"ls"}}' | <cmd>`
   - `Stop`/`UserPromptSubmit`/`SessionStart`:
     `echo '{}' | <cmd>`

   Check exit code AND side effect. Fix and retest. Once it works, wrap with
   `2>/dev/null || true` (unless the user wants a blocking check).

4. **Write the JSON.** Merge into the target file. If this creates
   `.claude/settings.local.json` for the first time, add it to `.gitignore`.

5. **Validate syntax + schema in one shot:**

   ```
   jq -e '.hooks.<event>[] | select(.matcher == "<matcher>") | .hooks[] | select(.type == "command") | .command' <target-file>
   ```

   Exit 0 + prints your command = correct. Exit 4 = no match. Exit 5 = malformed.
   A broken settings.json silently disables ALL settings from that file.

6. **Prove the hook fires** (only for Pre|PostToolUse on matchers you can
   trigger in-turn):
   - For a formatter: introduce a detectable violation via Edit, re-read,
     confirm the hook fixed it. Then revert.
   - For anything else: temporarily prefix the command in settings.json with
     `echo "$(date) hook fired" >> /tmp/claude-hook-check.txt; `, trigger the
     matching tool, read the sentinel file. Then strip the prefix.

   **Always clean up** whether proof passed or failed.

7. **Handoff.** Tell user the hook is live (or needs `/hooks`/restart per
   watcher caveat). Point them at `/hooks` to review/edit/disable later.
)";

// ============================================================
// Main Update Config Prompt (mirrors TS UPDATE_CONFIG_PROMPT)
// ============================================================

constexpr std::string_view UPDATE_CONFIG_PROMPT_TEMPLATE = R"(## Update Config Skill

Modify the harness configuration by updating settings.json files.

## When Hooks Are Required (Not Memory)

If the user wants something automatic in response to an EVENT, they need a
**hook** in settings.json. Memory/preferences cannot trigger automation.

**These require hooks:**
- "Before compacting, ask me what to preserve" -> PreCompact hook
- "After writing files, run prettier" -> PostToolUse hook with Write|Edit matcher
- "When I run bash commands, log them" -> PreToolUse hook with Bash matcher
- "Always run tests after code changes" -> PostToolUse hook

**Hook events:** PreToolUse, PostToolUse, PreCompact, PostCompact, Stop,
Notification, SessionStart

## CRITICAL: Read Before Write

**Always read the existing settings file before making changes.** Merge new
settings with existing ones - NEVER replace the entire file.

## CRITICAL: Use AskUserQuestion for Ambiguity

When ambiguous, use AskUserQuestion to clarify:
- Which settings file to modify (user/project/local)
- Whether to add to existing arrays or replace them
- Specific values when multiple options exist

## Decision: Config Tool vs Direct Edit

**Use the Config tool** for these simple settings:
- `theme`, `editorMode`, `verbose`, `model`
- `language`, `alwaysThinkingEnabled`
- `permissions.defaultMode`

**Edit settings.json directly** for:
- Hooks (PreToolUse, PostToolUse, etc.)
- Complex permission rules (allow/deny arrays)
- Environment variables
- MCP server configuration
- Plugin configuration

## Workflow

1. **Clarify intent** - Ask if ambiguous
2. **Read existing file** - Use Read tool on the target settings file
3. **Merge carefully** - Preserve existing settings, especially arrays
4. **Edit file** - Use Edit tool (ask user first if file doesn't exist)
5. **Confirm** - Tell user what was changed

## Merging Arrays (Important!)

When adding to permission or hook arrays, **merge with existing**, don't replace:

**WRONG** (replaces existing permissions):
```json
{ "permissions": { "allow": ["Bash(npm:*)"] } }
```

**RIGHT** (preserves existing + adds new):
```json
{
  "permissions": {
    "allow": [
      "Bash(git:*)",
      "Edit(.claude)",
      "Bash(npm:*)"
    ]
  }
}
```
)";

// ============================================================
// Bundled Update Config Factory
// ============================================================

/// Full bundled update-config skill with settings schema, hooks docs,
/// and structured hook verification flow. Delegates config I/O entirely
/// to cc.config.config::ConfigManager and cc.config.feature_flags.
[[nodiscard]] inline SkillDefinition make_bundled_update_config_skill() {
    std::string full_content;
    full_content.reserve(
        std::string_view(UPDATE_CONFIG_PROMPT_TEMPLATE).size() +
        std::string_view(SETTINGS_EXAMPLES_DOCS).size() +
        std::string_view(HOOKS_DOCS).size() +
        std::string_view(HOOK_VERIFICATION_FLOW).size() +
        500 // for examples section, workflow, and common mistakes
    );

    full_content += std::string(UPDATE_CONFIG_PROMPT_TEMPLATE);
    full_content += "\n";
    full_content += std::string(SETTINGS_EXAMPLES_DOCS);
    full_content += "\n";
    full_content += std::string(HOOKS_DOCS);
    full_content += "\n";
    full_content += std::string(HOOK_VERIFICATION_FLOW);

    // Example workflows + common mistakes + troubleshooting
    full_content += R"(
## Example Workflows

### Adding a Hook
1. Clarify which formatter, linter, or action
2. Read `.claude/settings.json` (or create if missing)
3. Merge into existing hooks, don't replace
4. Follow the Hook Verification Flow above

### Adding Permissions
1. Read existing permissions
2. Add new rule(s) to allow/deny/ask arrays
3. Result: combined with existing rules

### Environment Variables
1. Decide: User settings (global) or project settings?
2. Read target file
3. Merge: add to env object

## Common Mistakes to Avoid
1. **Replacing instead of merging** - Always preserve existing settings
2. **Wrong file** - Ask user if scope is unclear
3. **Invalid JSON** - Validate syntax after changes
4. **Forgetting to read first** - Always read before write
5. **Missing .gitignore** for settings.local.json

## Troubleshooting Hooks
If a hook isn't running:
1. Check settings file content (read it)
2. Verify JSON syntax - invalid JSON silently fails
3. Check the matcher - does it match the tool name?
4. Check hook type: "command", "prompt", or "agent"?
5. Pipe-test the command manually with synthesized stdin
6. Run with debug mode to see hook execution logs
)";

    return SkillDefinition{
        .name = "update-config",
        .description =
            "Configure the harness via settings.json. Automated behaviors "
            "(\"from now on when X\", \"before/after X\") require hooks in "
            "settings.json — memory/preferences cannot execute code. Also use "
            "for: permissions, env vars, hook troubleshooting, plugin/MCP config. "
            "Use the Config tool for simple settings (theme, model, verbose).",
        .trigger_patterns = {
            R"(update.*config)",
            R"(change.*(?:setting|configuration))",
            R"(modify.*(?:config|settings))",
            R"(set.*(?:option|preference|config))",
            R"(configure\s+\w+)",
            R"(allow\s+(?:permission|command|tool))",
            R"(add\s+(?:a\s+)?hook)",
            R"(set\s+env(?:ironment)?\s+var)",
            R"(settings\.json)",
            R"(from\s+now\s+on\s+when)",
            R"(before\s+(?:each|every)\s+\w+)",
            R"(after\s+(?:each|every|writing|editing))",
            R"(whenever\s+(?:i\s+)?(?:you|claude|it)\s+)",
        },
        .content = std::move(full_content),
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

// ============================================================
// Config Delegation Helpers (100% delegate to cc.config modules)
// ============================================================

/// Get the correct settings path for a scope. Delegate to settings.cppm.
[[nodiscard]] inline std::filesystem::path resolve_settings_path(
    cc::config::SettingsScope scope,
    const std::filesystem::path& home_dir,
    const std::filesystem::path& project_root
) {
    return cc::config::get_settings_path(scope, home_dir, project_root);
}

/// Set a feature flag by name. Delegate to feature_flags.cppm.
/// Returns true if the flag name was recognized and set.
inline bool set_feature_flag(std::string_view name, bool enabled) {
    return cc::core::flags::global_flags().set_by_name(name, enabled);
}

/// Check if a feature flag is enabled.
[[nodiscard]] inline bool is_feature_flag_enabled(std::string_view name) {
    auto feature = cc::core::flags::FeatureFlagManager::find_by_name(name);
    return feature.has_value() && cc::core::flags::is_enabled(*feature);
}

/// Get a summary of all enabled features for display.
[[nodiscard]] inline std::string enabled_features_summary() {
    return cc::core::flags::global_flags().enabled_summary();
}

} // namespace cc::skills::bundled
