/// @file skill_keybindings.cppm
/// @brief Bundled keybindings-help skill - comprehensive customization guide.
/// Mirrors src/skills/bundled/keybindings.ts.
/// DIFFERS from root-level cc.skills.keybindings (a simple shortcut reference sheet):
///   - Root  : name="keybindings"       — simple keyboard cheat-sheet (static)
///   - Here : name="keybindings-help"  — full customization workflow with
///             file format, reserved shortcuts, /doctor validation, and
///             dynamic tables of actions/contexts.
module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <sstream>
#include <format>
#include <map>
#include <algorithm>

export module cc.skills.bundled.skill_keybindings;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// ---------------------------------------------------------------------------
// Static reference data (kept in sync with TS keybindings/schema.ts +
// keybindings/defaultBindings.ts + keybindings/reservedShortcuts.ts).
// When the C++ side gains access to the live schema, these tables can be
// generated dynamically from the source of truth.
// ---------------------------------------------------------------------------

/// All binding contexts (exact names from KEYBINDING_CONTEXTS in schema.ts)
inline const char* KNOWN_CONTEXTS[][2] = {
    {"Global",          "Applies everywhere unless overridden by a specific context"},
    {"Chat",            "Main chat input pane"},
    {"Autocomplete",    "Autocomplete / suggestion dropdown"},
    {"Confirmation",    "Yes/No permission and confirmation dialogs"},
    {"Tabs",            "Tab management bar"},
    {"Transcript",      "Message list / transcript view"},
    {"HistorySearch",   "History search modal"},
    {"Task",            "Task list panel"},
    {"ThemePicker",     "Theme picker dialog"},
    {"Help",            "Help modal"},
    {"Attachments",     "Attachment list"},
    {"Footer",          "Status / footer bar"},
    {"MessageSelector", "Multi-message selection mode"},
    {"DiffDialog",      "Diff review dialog"},
    {"ModelPicker",     "Model picker dialog"},
    {"Select",          "Generic selection lists"},
};

/// Key binding actions (prefix:context mapping for dynamic inference)
inline const char* PREFIX_TO_CONTEXT[][2] = {
    {"app",             "Global"},
    {"history",         "Global or Chat"},
    {"chat",            "Chat"},
    {"autocomplete",    "Autocomplete"},
    {"confirm",         "Confirmation"},
    {"tabs",            "Tabs"},
    {"transcript",      "Transcript"},
    {"historySearch",   "HistorySearch"},
    {"task",            "Task"},
    {"theme",           "ThemePicker"},
    {"help",            "Help"},
    {"attachments",     "Attachments"},
    {"footer",          "Footer"},
    {"messageSelector", "MessageSelector"},
    {"diff",            "DiffDialog"},
    {"modelPicker",     "ModelPicker"},
    {"select",          "Select"},
    {"permission",      "Confirmation"},
};

/// Common keyboard actions (subset of KEYBINDING_ACTIONS with plausible defaults;
/// full table populated dynamically when runtime schema is available)
struct DefaultBinding {
    const char* action;
    const char* keys;   // comma-separated
    const char* context;
};
inline DefaultBinding DEFAULT_BINDINGS[] = {
    {"app:toggleTodos",         "Ctrl+K Ctrl+T",   "Global"},
    {"app:help",                "Ctrl+/",          "Global"},
    {"app:exit",                "Ctrl+D",          "Global"},
    {"app:clearScreen",         "Ctrl+L",          "Global"},
    {"app:interrupt",           "Ctrl+C",          "Global"},
    {"chat:submit",             "Enter",           "Chat"},
    {"chat:newline",            "Alt+Enter",       "Chat"},
    {"chat:externalEditor",     "Ctrl+G",          "Chat"},
    {"chat:historyUp",          "Up",              "Chat"},
    {"chat:historyDown",        "Down",            "Chat"},
    {"historySearch:open",      "Ctrl+R",          "Global"},
    {"tabs:new",                "Ctrl+T",          "Tabs"},
    {"tabs:close",              "Ctrl+W",          "Tabs"},
    {"tabs:next",               "Ctrl+Tab",        "Tabs"},
    {"tabs:prev",               "Ctrl+Shift+Tab",  "Tabs"},
    {"confirm:accept",          "Y",               "Confirmation"},
    {"confirm:reject",          "N",               "Confirmation"},
};

/// Non-rebindable shortcuts (binding these will always raise an error)
struct ReservedShortcut { const char* key; const char* reason; };
inline ReservedShortcut NON_REBINDABLE[] = {
    {"Ctrl+C",  "Terminal interrupt (SIGINT) — cannot be rebound"},
    {"Ctrl+Z",  "Terminal suspend (SIGTSTP) — cannot be rebound"},
    {"Ctrl+D",  "EOF signal — reserved when input is empty"},
    {"Ctrl+S",  "Terminal XOFF flow control (may be unavoidable)"},
    {"Ctrl+Q",  "Terminal XON flow control"},
};
/// Shortcut reserved for terminal conventions (warned about, not blocked).
struct TerminalReservedShortcut { const char* key; const char* reason; const char* level; };
inline TerminalReservedShortcut TERMINAL_RESERVED[] = {
    {"Ctrl+B",  "tmux default prefix key",              "warning"},
    {"Ctrl+A",  "GNU screen default prefix key",        "warning"},
    {"Ctrl+R",  "Terminal reverse history search",      "warning"},
    {"Ctrl+L",  "Terminal form-feed / clear screen",    "warning"},
    {"Ctrl+V",  "Terminal literal-next (verbatim insert)", "warning"},
};
inline ReservedShortcut MACOS_RESERVED[] = {
    {"Cmd+Q",   "Quit application — macOS system"},
    {"Cmd+H",   "Hide application — macOS system"},
    {"Cmd+M",   "Minimize window — macOS system"},
    {"Cmd+W",   "Close window — macOS system"},
    {"Cmd+,",   "Preferences — macOS system"},
    {"Cmd+Space","Spotlight search — macOS system"},
};

// ---------------------------------------------------------------------------
// Dynamic table builders
// ---------------------------------------------------------------------------

namespace detail {

/// Build a markdown table from headers + rows.
inline std::string markdown_table(
    const std::vector<std::string>& headers,
    const std::vector<std::vector<std::string>>& rows) {

    std::ostringstream oss;
    auto join = [&](const std::vector<std::string>& v, const char* sep) {
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) oss << sep;
            oss << v[i];
        }
    };
    oss << "| "; join(headers, " | "); oss << " |\n";
    oss << "| ";
    for (size_t i = 0; i < headers.size(); ++i) {
        if (i) oss << " | ";
        oss << "---";
    }
    oss << " |\n";
    for (const auto& row : rows) {
        oss << "| "; join(row, " | "); oss << " |\n";
    }
    return oss.str();
}

/// Infer context name from action name prefix (mirrors TS inference logic)
inline std::string infer_context_from_action(std::string_view action) {
    auto colon = action.find(':');
    if (colon == std::string_view::npos) return "Unknown";
    std::string prefix(action.substr(0, colon));
    for (const auto& [pfx, ctx] : PREFIX_TO_CONTEXT) {
        if (prefix == pfx) return ctx;
    }
    return "Unknown";
}

} // namespace detail

/// Generate the "Available Contexts" markdown table dynamically.
inline std::string generate_contexts_table() {
    std::vector<std::string> headers = {"Context", "Description"};
    std::vector<std::vector<std::string>> rows;
    for (const auto& [ctx, desc] : KNOWN_CONTEXTS) {
        rows.push_back({std::format("`{}`", ctx), desc});
    }
    return detail::markdown_table(headers, rows);
}

/// Generate the "Available Actions" markdown table with default keys + context.
inline std::string generate_actions_table() {
    // Build lookup: action -> { keys, context }
    std::map<std::string, std::pair<std::string, std::string>> info;
    for (const auto& b : DEFAULT_BINDINGS) {
        info[b.action] = {b.keys, b.context};
    }

    std::vector<std::string> headers = {"Action", "Default Key(s)", "Context"};
    std::vector<std::vector<std::string>> rows;
    for (const auto& b : DEFAULT_BINDINGS) {
        auto keys = std::format("`{}`", b.keys);
        std::string context = b.context;
        // If missing, infer from prefix
        if (context.empty()) context = detail::infer_context_from_action(b.action);
        rows.push_back({std::format("`{}`", b.action), keys, context});
    }
    return detail::markdown_table(headers, rows);
}

/// Build the reserved-shortcuts section.
inline std::string generate_reserved_shortcuts() {
    std::ostringstream oss;

    oss << "### Non-rebindable (errors)\n";
    for (const auto& s : NON_REBINDABLE) {
        oss << "- `" << s.key << "` — " << s.reason << "\n";
    }
    oss << "\n";

    oss << "### Terminal reserved (errors/warnings)\n";
    for (const auto& s : TERMINAL_RESERVED) {
        // The 3rd column in TS stores severity; here we stored it as part of .reason
        oss << "- `" << s.key << "` — " << s.reason << "\n";
    }
    oss << "\n";

    oss << "### macOS reserved (errors)\n";
    for (const auto& s : MACOS_RESERVED) {
        oss << "- `" << s.key << "` — " << s.reason << "\n";
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Prompt sections (kept in sync with TS SECTION_* constants)
// ---------------------------------------------------------------------------

inline const char* SECTION_INTRO = R"(# Keybindings Skill

Create or modify `~/.claude/keybindings.json` to customize keyboard shortcuts.

## CRITICAL: Read Before Write

**Always read `~/.claude/keybindings.json` first** (it may not exist yet). Merge
changes with existing bindings — never replace the entire file.

- Use **Edit** tool for modifications to existing files
- Use **Write** tool only if the file does not exist yet)";

inline const char* SECTION_FILE_FORMAT = R"(## File Format

```json
{
  "$schema": "https://www.schemastore.org/claude-code-keybindings.json",
  "$docs": "https://code.claude.com/docs/en/keybindings",
  "bindings": [
    {
      "context": "Chat",
      "bindings": {
        "ctrl+e": "chat:externalEditor"
      }
    }
  ]
}
```

Always include the `$schema` and `$docs` fields.)";

inline const char* SECTION_KEYSTROKE_SYNTAX = R"(## Keystroke Syntax

**Modifiers** (combine with `+`):
- `ctrl` (alias: `control`)
- `alt` (aliases: `opt`, `option`) — note: `alt` and `meta` are identical in terminals
- `shift`
- `meta` (aliases: `cmd`, `command`)

**Special keys**: `escape`/`esc`, `enter`/`return`, `tab`, `space`, `backspace`,
`delete`, `up`, `down`, `left`, `right`

**Chords**: Space-separated keystrokes, e.g. `ctrl+k ctrl+s`
(1-second timeout between keystrokes)

**Examples**: `ctrl+shift+p`, `alt+enter`, `ctrl+k ctrl+n`)";

inline const char* SECTION_UNBINDING = R"(## Unbinding Default Shortcuts

Set a key to `null` to remove its default binding:

```json
{
  "context": "Chat",
  "bindings": {
    "ctrl+s": null
  }
}
```)";

inline const char* SECTION_INTERACTION = R"(## How User Bindings Interact with Defaults

- User bindings are **additive** — they are appended after the default bindings
- To **move** a binding to a different key: unbind the old key (`null`) AND add the new binding
- A context only needs to appear in the user's file if they want to change something in that context)";

inline const char* SECTION_COMMON_PATTERNS = R"(## Common Patterns

### Rebind a key
To change the external editor shortcut from `ctrl+g` to `ctrl+e`:

```json
{
  "context": "Chat",
  "bindings": {
    "ctrl+g": null,
    "ctrl+e": "chat:externalEditor"
  }
}
```

### Add a chord binding

```json
{
  "context": "Global",
  "bindings": {
    "ctrl+k ctrl+t": "app:toggleTodos"
  }
}
```)";

inline const char* SECTION_BEHAVIORAL_RULES = R"(## Behavioral Rules

1. Only include contexts the user wants to change (minimal overrides)
2. Validate that actions and contexts are from the known lists below
3. Warn the user proactively if they choose a key that conflicts with reserved
   shortcuts or common tools like tmux (`ctrl+b`) and screen (`ctrl+a`)
4. When adding a new binding for an existing action, the new binding is additive
   (existing default still works unless explicitly unbound)
5. To fully replace a default binding, unbind the old key AND add the new one)";

/// Return the /doctor validation section (with inline markdown table)
inline std::string section_doctor() {
    std::vector<std::string> headers = {"Issue", "Cause", "Fix"};
    std::vector<std::vector<std::string>> rows = {
        {
            "`keybindings.json must have a \"bindings\" array`",
            "Missing wrapper object",
            "Wrap bindings in `{ \"bindings\": [...] }`"
        },
        {
            "`\"bindings\" must be an array`",
            "`bindings` is not an array",
            "Set `\"bindings\"` to an array: `[{ context: ..., bindings: ... }]`"
        },
        {
            "`Unknown context \"X\"`",
            "Typo or invalid context name",
            "Use exact context names from the Available Contexts table"
        },
        {
            "`Duplicate key \"X\" in Y bindings`",
            "Same key defined twice in one context",
            "Remove the duplicate; JSON uses only the last value"
        },
        {
            "`\"X\" may not work: ...`",
            "Key conflicts with terminal/OS reserved shortcut",
            "Choose a different key (see Reserved Shortcuts section)"
        },
        {
            "`Could not parse keystroke \"X\"`",
            "Invalid key syntax",
            "Check syntax: use `+` between modifiers, valid key names"
        },
        {
            "`Invalid action for \"X\"`",
            "Action value is not a string or null",
            "Actions must be strings like `\"app:help\"` or `null` to unbind"
        },
    };

    std::ostringstream oss;
    oss << R"(## Validation with /doctor

The `/doctor` command includes a "Keybinding Configuration Issues" section that
validates `~/.claude/keybindings.json`.

### Common Issues and Fixes

)" << detail::markdown_table(headers, rows) << R"(
### Example /doctor Output

```
Keybinding Configuration Issues
Location: ~/.claude/keybindings.json
  └ [Error] Unknown context "chat"
    → Valid contexts: Global, Chat, Autocomplete, ...
  └ [Warning] "ctrl+c" may not work: Terminal interrupt (SIGINT)
```

**Errors** prevent bindings from working and must be fixed. **Warnings** indicate
potential conflicts but the binding may still work.)";
    return oss.str();
}

// ---------------------------------------------------------------------------
// Prompt assembly
// ---------------------------------------------------------------------------

/// Build the complete keybindings-help prompt.
/// If `user_args` is provided, appends a "## User Request" section.
inline std::string build_keybindings_help_prompt(
    std::optional<std::string_view> user_args = std::nullopt) {

    std::vector<std::string> sections = {
        SECTION_INTRO,
        SECTION_FILE_FORMAT,
        SECTION_KEYSTROKE_SYNTAX,
        SECTION_UNBINDING,
        SECTION_INTERACTION,
        SECTION_COMMON_PATTERNS,
        SECTION_BEHAVIORAL_RULES,
        section_doctor(),
        std::format("## Reserved Shortcuts\n\n{}", generate_reserved_shortcuts()),
        std::format("## Available Contexts\n\n{}", generate_contexts_table()),
        std::format("## Available Actions\n\n{}", generate_actions_table()),
    };
    if (user_args && !user_args->empty()) {
        sections.push_back(std::format("## User Request\n\n{}", *user_args));
    }

    std::ostringstream oss;
    for (size_t i = 0; i < sections.size(); ++i) {
        if (i) oss << "\n\n";
        oss << sections[i];
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Feature-gate check (mirrors TS isKeybindingCustomizationEnabled)
// ---------------------------------------------------------------------------

/// Returns whether keybinding customization is enabled.
/// Gate exists because some restricted environments do not permit user
/// keybindings overrides.
inline bool is_keybinding_customization_enabled() {
    // TODO(config): Read from global config when available.
    const char* val = std::getenv("CLAUDE_CODE_KEYBINDINGS_ENABLED");
    if (val && (std::string(val) == "0" || std::string(val) == "false")) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Manifest (for SkillLoader discovery)
// ---------------------------------------------------------------------------

/// Get the skill manifest for the bundled keybindings-help skill.
/// NOTE: skill name is `"keybindings-help"` (NOT `"keybindings"`).
/// The root-level `cc.skills.keybindings` module provides a simple shortcut
/// reference sheet under name `"keybindings"`.
cc::skills::SkillManifest get_keybindings_help_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "keybindings-help",
        .description =
            "Use when the user wants to customize keyboard shortcuts, rebind keys, add "
            "chord bindings, or modify ~/.claude/keybindings.json. Examples: \"rebind "
            "ctrl+s\", \"add a chord shortcut\", \"change the submit key\", "
            "\"customize keybindings\".",
        .version = "1.0.0",
        .triggers = {
            "keybinding",
            "keybindings",
            "rebind",
            "customize key",
            "keyboard shortcut",
            "keybindings.json",
            "chord binding",
            "unbind key",
            "change submit key",
            "快捷键",
            "键位绑定",
        },
        .directory = {}
    };
}

} // namespace cc::skills::bundled
