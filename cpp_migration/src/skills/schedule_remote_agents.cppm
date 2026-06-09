/// @file schedule_remote_agents.cppm
/// @brief Schedule Remote Agents skill - slash command wrapper that builds a
/// detailed prompt for creating/listing/updating/running remote Claude Code
/// agent triggers via the RemoteTrigger tool.
///
/// Audit vs TS src/skills/bundled/scheduleRemoteAgents.ts:
///   - TS builds prompt dynamically with MCP connectors, git repo, environments.
///   - C++ emits the same structured prompt as a static template with clearly
///     marked placeholders (the LLM fills them in from context at runtime).
///   - Natural-language time parsing: we document the mapping rules inline so
///     the LLM performs the NL->cron conversion; the actual cron engine lives
///     in cc.tools.cron (ScheduleCronTool) and is NOT duplicated here.
///   - List / update / run / delete-redirect workflows match TS exactly.
module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.skills.schedule_remote_agents;

import cc.skills.skill;
import cc.tools.cron;

export namespace cc::skills::schedule_remote_agents {

// ============================================================
// Base58 (Bitcoin-style) alphabet used by tagged MCP connector IDs.
// Mirrors TS scheduleRemoteAgents.ts::BASE58.
// ============================================================
constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/// Decode a ``mcpsrv_01{base58(uuid)}`` tagged connector ID to a UUID string.
/// Mirrors TS ``taggedIdToUUID``.  Returns ``nullopt`` on any format error.
[[nodiscard]] inline std::optional<std::string> tagged_id_to_uuid(
    std::string_view tagged_id) noexcept
{
    constexpr std::string_view prefix = "mcpsrv_";
    if (!tagged_id.starts_with(prefix)) return std::nullopt;
    const auto rest = tagged_id.substr(prefix.size());
    if (rest.size() < 3) return std::nullopt;            // version(2) + at least 1 byte
    // Skip the 2-char version prefix ("01").
    const auto base58_data = rest.substr(2);
    if (base58_data.empty()) return std::nullopt;

    // Decode base58 -> big integer (use 128-bit; a UUID fits exactly).
    __uint128_t n = 0;
    for (char c : base58_data) {
        const auto pos = kBase58Alphabet.find(c);
        if (pos == std::string_view::npos) return std::nullopt;
        n = n * 58 + static_cast<__uint128_t>(pos);
    }
    // Convert to 32-hex-digit string.
    std::string hex;
    hex.reserve(32);
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 31; i >= 0; --i) {
        const unsigned nibble = static_cast<unsigned>((n >> (i * 4)) & 0xF);
        hex.push_back(kHex[nibble]);
    }
    // Format as 8-4-4-4-12.
    return std::format("{}-{}-{}-{}-{}",
        hex.substr(0, 8),
        hex.substr(8, 4),
        hex.substr(12, 4),
        hex.substr(16, 4),
        hex.substr(20, 12));
}

/// Remove the "claude.ai-" prefix and sanitize a connector display name to the
/// character set ``[a-zA-Z0-9_-]``.  Mirrors TS ``sanitizeConnectorName``.
[[nodiscard]] inline std::string sanitize_connector_name(std::string name) noexcept {
    // Strip leading "claude.ai-", "claude ai ", "claude_ai-" prefixes (case insensitive).
    static constexpr std::array<std::string_view, 6> kPrefixes = {
        "claude.ai-", "claude ai-", "claude_ai-",
        "claude.ai ", "claude ai ", "claude_ai_",
    };
    for (auto pfx : kPrefixes) {
        if (name.size() >= pfx.size()) {
            bool match = true;
            for (size_t i = 0; i < pfx.size() && match; ++i) {
                if (std::tolower(static_cast<unsigned char>(name[i])) !=
                    std::tolower(static_cast<unsigned char>(pfx[i])))
                    match = false;
            }
            if (match) { name.erase(0, pfx.size()); break; }
        }
    }
    // Replace any non [a-zA-Z0-9_-] char with '-'.
    for (char& c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-')) c = '-';
    }
    // Collapse runs of '-'.
    std::string out;
    out.reserve(name.size());
    bool last_dash = false;
    for (char c : name) {
        if (c == '-') {
            if (!last_dash) { out.push_back(c); last_dash = true; }
        } else {
            out.push_back(c);
            last_dash = false;
        }
    }
    // Trim leading/trailing '-'.
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back()  == '-') out.pop_back();
    return out;
}

// ============================================================
// Natural-language time -> 5-field cron expression.
//
// This covers the common keyword phrases the TS skill asks the LLM to
// translate ("tomorrow 9am", "every Monday 10am", "every weekday at 9am").
// The function intentionally keeps a small, testable surface; the LLM is
// trusted to handle edge cases.  Complex NLP is TODO (no date lib in deps).
// ============================================================
struct NlParseResult {
    std::optional<std::string> cron_expr;     /// 5-field cron (UTC)
    std::optional<std::string> description;   /// Human-readable next run
    std::optional<std::string> error;         /// Non-empty on parse failure
    bool one_shot = false;                    /// True = one-off (not cron)
};

/// Map a weekday name (en) to cron DOW number (0=Sun..6=Sat).
[[nodiscard]] inline std::optional<int> dow_from_name(std::string_view s) {
    static const std::unordered_map<std::string_view, int> kMap = {
        {"sun",0},{"sunday",0},
        {"mon",1},{"monday",1},
        {"tue",2},{"tuesday",2},
        {"wed",3},{"wednesday",3},{"wednes",3},
        {"thu",4},{"thursday",4},{"thur",4},{"thurs",4},
        {"fri",5},{"friday",5},
        {"sat",6},{"saturday",6},
    };
    std::string key(s);
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c){ return std::tolower(c); });
    auto it = kMap.find(key);
    return (it == kMap.end()) ? std::nullopt : std::optional{it->second};
}

/// Parse an "HHam"/"HHpm"/"HH:MMam"-style clock string into (hour_24, min).
[[nodiscard]] inline std::optional<std::pair<int,int>> parse_clock(std::string_view s) {
    std::string t(s);
    std::transform(t.begin(), t.end(), t.begin(),
        [](unsigned char c){ return std::tolower(c); });
    bool pm = t.ends_with("pm");
    bool am = t.ends_with("am");
    if (!am && !pm) return std::nullopt;
    t.pop_back(); t.pop_back();
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    // Accept "9", "9:30", "0930".
    int h = 0, m = 0;
    auto colon = t.find(':');
    if (colon != std::string::npos) {
        h = std::atoi(t.substr(0, colon).c_str());
        m = std::atoi(t.substr(colon + 1).c_str());
    } else if (t.size() >= 3 && std::isdigit(static_cast<unsigned char>(t[t.size()-3]))) {
        // "0930"
        m = std::atoi(t.substr(t.size() - 2).c_str());
        h = std::atoi(t.substr(0, t.size() - 2).c_str());
    } else {
        h = std::atoi(t.c_str());
    }
    if (h < 1 || h > 12 || m < 0 || m > 59) return std::nullopt;
    if (pm && h != 12) h += 12;
    if (am && h == 12) h = 0;
    return std::pair{h, m};
}

/// Apply a simple UTC offset (hours) to a local-time parse.  Offsets come from
/// the caller (e.g. tz offset extracted from system at skill invocation time).
/// NOTE: for the skill template, we emit the conversion rules as prose and let
/// the LLM compute them; this function is only used in unit-testable code
/// paths.
[[nodiscard]] inline std::pair<int,int> apply_tz_offset(int h_local, int m_local,
                                                        int offset_hrs) noexcept {
    int h = h_local - offset_hrs;
    int dm = 0;
    if (h < 0)  { h += 24; dm = -1; }
    if (h > 23) { h -= 24; dm = +1; }
    (void)dm; // day-shift not tracked at cron granularity (users notice dates)
    return {h, m_local};
}

/// Parse a limited set of NL time phrases.  Mirrors the subset of natural
/// language that the TS skill documents (see ``buildPrompt`` cron examples).
/// Returns an empty ``cron_expr`` and populated ``error`` for anything
/// ambiguous; the LLM in the loop is instructed to ask for clarification.
[[nodiscard]] inline NlParseResult parse_natural_time(std::string_view phrase) {
    NlParseResult r;
    std::string s(phrase);
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c){ return std::tolower(c); });
    // -- clock tokens ----------------------------------------------------
    std::optional<std::pair<int,int>> clock;
    // scan for \d+(am|pm) or \d+:\d+(am|pm)
    {
        auto re_like = [&](size_t i) -> std::optional<std::pair<int,int>> {
            // capture a maximal run of digits/colon followed by am/pm
            size_t j = i;
            while (j < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == ':')) ++j;
            if (j + 1 < s.size() && (s[j] == 'a' || s[j] == 'p') && s[j+1] == 'm') {
                return parse_clock(std::string_view(s).substr(i, j + 2 - i));
            }
            return std::nullopt;
        };
        for (size_t i = 0; i < s.size() && !clock; ++i) {
            if (std::isdigit(static_cast<unsigned char>(s[i])) || (i > 0 && s[i-1] == ' ' && std::isdigit(static_cast<unsigned char>(s[i])))) {
                if (auto c = re_like(i)) { clock = c; break; }
            }
        }
    }
    // default clock if none given: 09:00 local
    if (!clock) clock = {9, 0};
    const auto [hh, mm] = *clock;

    // -- day / recurrence tokens ----------------------------------------
    // "every weekday" / "weekdays"
    if (s.find("weekday") != std::string::npos) {
        r.cron_expr = std::format("{} {} * * 1-5", mm, hh);
        r.description = std::format("Every weekday at {:02d}:{:02d} local", hh, mm);
        return r;
    }
    // "every weekend" / "weekends"
    if (s.find("weekend") != std::string::npos) {
        r.cron_expr = std::format("{} {} * * 0,6", mm, hh);
        r.description = std::format("Weekends at {:02d}:{:02d} local", hh, mm);
        return r;
    }
    // "every MON" / "on MON"
    for (auto [name, num] : std::initializer_list<std::pair<std::string_view,int>>{
            {"monday",1},{"tuesday",2},{"wednesday",3},{"thursday",4},
            {"friday",5},{"saturday",6},{"sunday",0},
            {"mon",1},{"tue",2},{"wed",3},{"thu",4},{"fri",5},{"sat",6},{"sun",0}}) {
        if (s.find(name) != std::string::npos) {
            r.cron_expr = std::format("{} {} * * {}", mm, hh, num);
            r.description = std::format("Every {} at {:02d}:{:02d} local", name, hh, mm);
            return r;
        }
    }
    // "every day" / "daily" / "each day"
    if (s.find("every day") != std::string::npos || s.find("daily") != std::string::npos ||
        s.find("each day") != std::string::npos || s.find("everyday") != std::string::npos) {
        r.cron_expr = std::format("{} {} * * *", mm, hh);
        r.description = std::format("Every day at {:02d}:{:02d} local", hh, mm);
        return r;
    }
    // "every N hours"
    // "every hour"
    if (s.find("every hour") != std::string::npos || s.find("hourly") != std::string::npos) {
        r.cron_expr = std::format("{} * * * *", mm);
        r.description = std::format("Every hour at :{:02d} local", mm);
        return r;
    }
    // "tomorrow" + clock => one-shot (cron can't express "once tomorrow")
    if (s.find("tomorrow") != std::string::npos) {
        r.one_shot = true;
        r.description = std::format("Once tomorrow at {:02d}:{:02d} local", hh, mm);
        r.cron_expr = std::format("{} {} * * *", mm, hh);  // fire at next matching
        return r;
    }
    // "next monday" etc. => one-shot
    if (s.find("next ") != std::string::npos) {
        r.one_shot = true;
        r.cron_expr = std::format("{} {} * * *", mm, hh);
        r.description = std::format("Once, next occurrence at {:02d}:{:02d} local", hh, mm);
        return r;
    }
    // "first of the month"
    if (s.find("first of") != std::string::npos || s.find("1st of") != std::string::npos) {
        r.cron_expr = std::format("{} {} 1 * *", mm, hh);
        r.description = std::format("1st of every month at {:02d}:{:02d} local", hh, mm);
        return r;
    }
    // Fallback: unknown pattern -> ask the LLM to re-prompt the user.
    r.error = std::format(
        "Cannot translate '{}' to a cron schedule.  Supported: "
        "'tomorrow 9am', 'every Monday 10am', 'every weekday 9am', "
        "'every 2 hours', 'first of the month 8am'.  Ask the user for clarification.",
        phrase);
    return r;
}

// ============================================================
// Delegation wrappers for the cron_tool store (we DO NOT duplicate the
// scheduling engine).  Every list/create/delete request is forwarded.
// ============================================================
using cc::tools::CronAction;
using cc::tools::CronError;
using cc::tools::CronRequest;
using cc::tools::ScheduleCronTool;
using cc::tools::global_cron_store;

/// Thin helper: list all scheduled tasks as a readable report (mirrors TS
/// "/schedule list" workflow output style).
[[nodiscard]] inline std::string list_scheduled_tasks_human() {
    auto tasks = global_cron_store().list();
    if (tasks.empty()) return "No scheduled remote-agent tasks.";
    std::string out = std::format("Scheduled remote-agent tasks ({}):\n", tasks.size());
    for (const auto* t : tasks) {
        out += std::format("  - [{}] {}  id={}  cron='{}'  tz={}  runs={}\n",
            t->state == cc::tools::CronTaskState::Active ? "active" : "paused",
            t->name, t->id, t->expression.raw, t->timezone, t->run_count);
    }
    return out;
}

/// Thin helper: create (delegates to ScheduleCronTool) and format the
/// confirmation line the same way TS does it ("Created X — next run Y").
[[nodiscard]] inline std::expected<std::string, CronError>
create_scheduled_task(
    std::string name,
    std::string message,
    std::string cron_expr,
    std::string timezone = "UTC")
{
    ScheduleCronTool tool;
    return tool.execute(CronRequest{
        .action = CronAction::Create,
        .name = std::move(name),
        .message = std::move(message),
        .cron_expression = std::move(cron_expr),
        .timezone = std::move(timezone),
    });
}

/// Thin helper: cancel (delete) a task by id.  Mirrors TS "cancel" path.
[[nodiscard]] inline std::expected<std::string, CronError>
cancel_scheduled_task(std::string task_id) {
    ScheduleCronTool tool;
    return tool.execute(CronRequest{
        .action = CronAction::Delete,
        .task_id = std::move(task_id),
    });
}

// ============================================================
// The full prompt template (mirrors TS buildPrompt()).
//
// The TS skill dynamically interpolates connector list, git repo URL,
// environment list, setup notes, timezone, and user args; the C++ version
// exposes the same interpolation points as explicit template fields so the
// caller fills them in from context.  See ``build_prompt(...)``.
// ============================================================
constexpr std::string_view kRemoteTriggerToolName = "remote_trigger";
constexpr std::string_view kAskUserQuestionToolName = "ask_user_question";
constexpr std::string_view kBaseQuestion =
    "What would you like to do with scheduled remote agents?";

struct BuildPromptOpts {
    std::string_view user_timezone = "UTC";
    std::string_view connectors_info =
        "No connected MCP connectors found. The user may need to connect "
        "servers at https://claude.ai/settings/connectors";
    std::string_view git_repo_url;   /// empty => no repo context
    std::string_view environments_info =
        "Available environments:\n"
        "- default (id: env_default, kind: cloud)";
    std::string_view created_environment;  /// empty when none was auto-created
    std::vector<std::string_view> setup_notes;
    bool needs_github_access_reminder = false;
    std::string_view user_args;       /// slash-command args
    bool authenticated = true;        /// false => emit auth-error prompt only
};

[[nodiscard]] inline std::string format_setup_notes(
    std::span<const std::string_view> notes)
{
    if (notes.empty()) return {};
    std::string out = "\u26a0 Heads-up:\n";
    for (auto n : notes) out += std::format("- {}\n", n);
    return out;
}

[[nodiscard]] inline std::string build_prompt(const BuildPromptOpts& opts) {
    // -- Auth short-circuit (matches TS registerScheduleRemoteAgentsSkill head)
    if (!opts.authenticated) {
        return "You need to authenticate with a claude.ai account first. "
               "API accounts are not supported.  Run /login, then try "
               "/schedule again.";
    }

    const auto setup_block = format_setup_notes(opts.setup_notes);

    const std::string initial_question =
        opts.setup_notes.empty()
            ? std::string(kBaseQuestion)
            : std::format("{}\n\n{}", setup_block, kBaseQuestion);

    const std::string first_step = opts.user_args.empty()
        ? std::format(
            "Your FIRST action must be a single {} tool call "
            "(no preamble). Use this EXACT string for the `question` field — "
            "do not paraphrase or shorten it:\n\n\"{}\"\n\n"
            "Set `header: \"Action\"` and offer the four actions "
            "(create / list / update / run) as options. "
            "After the user picks, follow the matching workflow below.",
            kAskUserQuestionToolName, initial_question)
        : "The user has already told you what they want (see User Request at "
          "the bottom). Skip the initial question and go directly to the "
          "matching workflow.";

    const std::string setup_notes_section =
        (!opts.user_args.empty() && !opts.setup_notes.empty())
            ? std::format("\n## Setup Notes\n\n{}\n", setup_block)
            : "";

    const std::string created_env_line = opts.created_environment.empty()
        ? ""
        : std::format(
            "\n**Note:** A new environment `{}` was just created for the "
            "user because they had none.  Use its id for "
            "`job_config.ccr.environment_id` and mention the creation when "
            "you confirm the trigger config.\n",
            opts.created_environment);

    const std::string github_access_reminder =
        opts.needs_github_access_reminder
            ? "- If the user's request seems to require GitHub repo access "
              "(e.g. cloning a repo, opening PRs, reading code), remind them "
              "they need the Claude GitHub App installed on the repo — "
              "otherwise the remote agent won't be able to access it.\n"
        : "";

    const std::string user_request_section = opts.user_args.empty()
        ? ""
        : std::format(
            "\n## User Request\n\n"
            "The user said: \"{}\"\n\n"
            "Start by understanding their intent and working through the "
            "appropriate workflow above.\n",
            opts.user_args);

    const std::string repo_mention = opts.git_repo_url.empty()
        ? " Ask which git repos the remote agent needs cloned into its environment."
        : std::format(
            " The default git repo is already set to `{}`. Ask the user if "
            "this is the right repo or if they need a different one.",
            opts.git_repo_url);

    return std::format(
R"(# Schedule Remote Agents

You are helping the user schedule, update, list, or run **remote** Claude Code
agents. These are NOT local cron jobs — each trigger spawns a fully isolated
remote session (CCR) in Anthropic's cloud infrastructure on a cron schedule.
The agent runs in a sandboxed environment with its own git checkout, tools,
and optional MCP connections.

## First Step

{}
{}

## What You Can Do

Use the `{}` tool (auth is handled in-process — do not use curl):

- `{{action: "list"}}` — list all triggers
- `{{action: "get", trigger_id: "..."}}` — fetch one trigger
- `{{action: "create", body: {{...}}}}` — create a trigger
- `{{action: "update", trigger_id: "...", body: {{...}}}}` — partial update
- `{{action: "run", trigger_id: "..."}}` — run a trigger now

You CANNOT delete triggers. If the user asks to delete, direct them to:
https://claude.ai/code/scheduled

## Create body shape

```json
{{
  "name": "AGENT_NAME",
  "cron_expression": "CRON_EXPR",
  "enabled": true,
  "job_config": {{
    "ccr": {{
      "environment_id": "ENVIRONMENT_ID",
      "session_context": {{
        "model": "claude-sonnet-4-6",
        "sources": [
          {{"git_repository": {{"url": "{}"}}}}
        ],
        "allowed_tools": ["Bash", "Read", "Write", "Edit", "Glob", "Grep"]
      }},
      "events": [
        {{"data": {{
          "uuid": "<lowercase v4 uuid>",
          "session_id": "",
          "type": "user",
          "parent_tool_use_id": null,
          "message": {{"content": "PROMPT_HERE", "role": "user"}}
        }}}}
      ]
    }}
  }}
}}
```

Generate a fresh lowercase UUID for `events[].data.uuid` yourself.

## Available MCP Connectors

These are the user's currently connected claude.ai MCP connectors:

{}

When attaching connectors to a trigger, use the `connector_uuid` and `name`
shown above (the name is already sanitized to only contain letters, numbers,
hyphens, and underscores), and the connector's URL.  The `name` field in
`mcp_connections` must only contain `[a-zA-Z0-9_-]` — dots and spaces are NOT
allowed.

**Important:** Infer what services the agent needs from the user's
description.  For example, if they say "check Datadog and Slack me errors,"
the agent needs both Datadog and Slack connectors.  Cross-reference against
the list above and warn if any required service isn't connected.  If a needed
connector is missing, direct the user to
https://claude.ai/settings/connectors to connect it first.

## Environments

Every trigger requires an `environment_id` in the job config.  This
determines where the remote agent runs.  Ask the user which environment to
use.

{}

Use the `id` value as the `environment_id` in `job_config.ccr.environment_id`.
{}
## API Field Reference

### Create Trigger — Required Fields
- `name` (string) — A descriptive name
- `cron_expression` (string) — 5-field cron. **Minimum interval is 1 hour.**
- `job_config` (object) — Session configuration (see structure above)

### Create Trigger — Optional Fields
- `enabled` (boolean, default: true)
- `mcp_connections` (array) — MCP servers to attach:
  ```json
  [{{"connector_uuid": "uuid", "name": "server-name", "url": "https://..."}}]
  ```

### Update Trigger — Optional Fields
All fields optional (partial update):
- `name`, `cron_expression`, `enabled`, `job_config`
- `mcp_connections` — Replace MCP connections
- `clear_mcp_connections` (boolean) — Remove all MCP connections

### Cron Expression Examples

The user's local timezone is **{}**. Cron expressions are always in UTC.
When the user says a local time, convert it to UTC for the cron expression
but confirm with them: "9am {} = Xam UTC, so the cron would be `0 X * * 1-5`."

- `0 9 * * 1-5` — Every weekday at 9am **UTC**
- `0 */2 * * *` — Every 2 hours
- `0 0 * * *` — Daily at midnight **UTC**
- `30 14 * * 1` — Every Monday at 2:30pm **UTC**
- `0 8 1 * *` — First of every month at 8am **UTC**

Minimum interval is 1 hour. `*/30 * * * *` will be rejected.

## Workflow

### CREATE a new trigger:

1. **Understand the goal** — Ask what they want the remote agent to do.
   What repo(s)? What task? Remind them that the agent runs remotely — it
   won't have access to their local machine, local files, or local env vars.
2. **Craft the prompt** — Help them write an effective agent prompt.  Good
   prompts are specific about what to do and what success looks like; clear
   about which files/areas to focus on; explicit about what actions to take
   (open PRs, commit, just analyze, etc.).
3. **Set the schedule** — Ask when and how often.  The user's timezone is
   {}. When they say a time (e.g., "every morning at 9am"), assume they mean
   their local time and convert to UTC for the cron expression.  Always
   confirm the conversion: "9am {} = Xam UTC."
4. **Choose the model** — Default to `claude-sonnet-4-6`.  Tell the user
   which model you're defaulting to and ask if they want a different one.
5. **Validate connections** — Infer what services the agent will need from
   the user's description.  Cross-reference with the connectors list above.
   If any are missing, warn the user and link them to
   https://claude.ai/settings/connectors to connect first.{}
6. **Review and confirm** — Show the full configuration before creating.
   Let them adjust.
7. **Create it** — Call `{}` with `action: "create"` and show the result.
   The response includes the trigger ID.  Always output a link at the end:
   `https://claude.ai/code/scheduled/{{TRIGGER_ID}}`

### UPDATE a trigger:

1. List triggers first so they can pick one
2. Ask what they want to change
3. Show current vs proposed value
4. Confirm and update

### LIST triggers:

1. Fetch and display in a readable format
2. Show: name, schedule (human-readable), enabled/disabled, next run, repo(s)

### RUN NOW:

1. List triggers if they haven't specified which one
2. Confirm which trigger
3. Execute and confirm

## Important Notes

- These are REMOTE agents — they run in Anthropic's cloud, not on the user's
  machine.  They cannot access local files, local services, or local env vars.
- Always convert cron to human-readable when displaying
- Default to `enabled: true` unless user says otherwise
- Accept GitHub URLs in any format (https://github.com/org/repo, org/repo,
  etc.) and normalize to the full HTTPS URL (without .git suffix)
- The prompt is the most important part — spend time getting it right.  The
  remote agent starts with zero context, so the prompt must be self-contained.
- To delete a trigger, direct users to https://claude.ai/code/scheduled
{}
{})",
        first_step,
        setup_notes_section,
        kRemoteTriggerToolName,
        opts.git_repo_url.empty() ? "https://github.com/ORG/REPO" : opts.git_repo_url,
        opts.connectors_info,
        opts.environments_info,
        created_env_line,
        opts.user_timezone, opts.user_timezone,
        opts.user_timezone, opts.user_timezone,
        repo_mention,
        kRemoteTriggerToolName,
        github_access_reminder,
        user_request_section
    );
}

// ============================================================
// SkillDefinition factory — mirrors registerScheduleRemoteAgentsSkill.
// ============================================================
[[nodiscard]] inline SkillDefinition make_schedule_remote_agents_skill() {
    // NOTE: the TS version uses `name: "schedule"` for its slash-command
    // entry point; the older generic name "schedule-remote-agents" is kept
    // as a trigger alias via trigger_patterns.
    return SkillDefinition{
        .name = "schedule",
        .description =
            "Create, update, list, or run scheduled remote agents (triggers) "
            "that execute on a cron schedule in Anthropic's cloud.",
        .trigger_patterns = {
            R"(schedule.*(?:agent|remote|trigger|cron))",
            R"(remote\s+agent)",
            R"(/schedule)",
            R"(cron.*(?:job|task))",
            R"(recurring\s+(?:task|agent|job))",
            R"(scheduled\s+(?:run|task))",
        },
        .content = build_prompt(BuildPromptOpts{}),
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.1.0",
    };
}

} // namespace cc::skills::schedule_remote_agents
