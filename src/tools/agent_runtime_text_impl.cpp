// Implementation unit for cc.tools.agent_runtime — pure text/enum/
// predicate/builder helpers: whitespace and list parsing, agent-type
// canonicalization and resolution, hook-event canonical names, fork-child
// message builders, JSON/XML escaping, transcript role helpers. Lightest
// unit: no project imports.
module;

#include <cctype>
#include <cstdlib>

module cc.tools.agent_runtime;

import std;

namespace cc::tools::agent_runtime {

namespace {

inline constexpr std::string_view kForkBoilerplateTag = "fork-boilerplate";
inline constexpr std::string_view kForkDirectivePrefix = "Your directive: ";

} // namespace

[[nodiscard]] std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}
[[nodiscard]] std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}
[[nodiscard]] std::vector<std::string> split_list_value(std::string_view value) {
    std::string text = trim(value);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text = text.substr(1, text.size() - 2);
    }
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        auto parsed = unquote(item);
        if (!parsed.empty()) values.push_back(std::move(parsed));
    }
    if (values.empty() && !text.empty()) values.push_back(unquote(text));
    return values;
}
[[nodiscard]] std::optional<int> parse_positive_int(std::string_view value) {
    auto text = trim(value);
    if (text.empty()) return std::nullopt;
    int parsed = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
        parsed = parsed * 10 + (ch - '0');
    }
    return parsed > 0 ? std::optional<int>{parsed} : std::nullopt;
}
[[nodiscard]] std::string canonicalize_agent_type(std::string_view value) {
    auto trimmed = trim(value);
    std::string canonical;
    canonical.reserve(trimmed.size());
    for (char ch : trimmed) {
        if (ch == '_' || ch == ' ') {
            canonical.push_back('-');
        } else {
            canonical.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return canonical;
}
[[nodiscard]] bool parse_bool_field(std::string_view value, bool fallback) {
    auto text = canonicalize_agent_type(value);
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
    return fallback;
}
[[nodiscard]] bool valid_agent_effort(std::string_view value) {
    auto text = canonicalize_agent_type(value);
    if (text == "low" || text == "medium" || text == "high" || text == "max") return true;
    if (text.empty()) return false;
    return std::ranges::all_of(text, [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch));
    });
}
[[nodiscard]] bool is_ant_user_type() {
    const char* value = std::getenv("USER_TYPE");
    return value && std::string_view(value) == "ant";
}
[[nodiscard]] bool valid_agent_isolation(std::string_view value) {
    if (value == "worktree") return true;
    if (value == "remote") return is_ant_user_type();
    return false;
}
[[nodiscard]] std::string valid_agent_isolation_options() {
    return is_ant_user_type() ? "worktree, remote" : "worktree";
}
[[nodiscard]] std::string canonical_hook_event_name(std::string_view event) {
    auto canonical = canonicalize_agent_type(event);
    if (canonical == "pretooluse" || canonical == "pre-tool-use") return "PreToolUse";
    if (canonical == "posttooluse" || canonical == "post-tool-use") return "PostToolUse";
    if (canonical == "posttoolusefailure" || canonical == "post-tool-use-failure") return "PostToolUseFailure";
    if (canonical == "permissiondenied" || canonical == "permission-denied") return "PermissionDenied";
    if (canonical == "notification") return "Notification";
    if (canonical == "userpromptsubmit" || canonical == "user-prompt-submit") return "UserPromptSubmit";
    if (canonical == "sessionstart" || canonical == "session-start") return "SessionStart";
    if (canonical == "sessionend" || canonical == "session-end") return "SessionEnd";
    if (canonical == "stop") return "SubagentStop";
    if (canonical == "subagentstart" || canonical == "subagent-start") return "SubagentStart";
    if (canonical == "subagentstop" || canonical == "subagent-stop") return "SubagentStop";
    if (canonical == "precompact" || canonical == "pre-compact") return "PreCompact";
    if (canonical == "postcompact" || canonical == "post-compact") return "PostCompact";
    if (canonical == "elicitation") return "Elicitation";
    if (canonical == "elicitationresult" || canonical == "elicitation-result") return "ElicitationResult";
    return std::string(event);
}
[[nodiscard]] std::vector<std::string> agent_alias_candidates(std::string_view requested_type) {
    const auto canonical = canonicalize_agent_type(requested_type);
    if (canonical == "explore") return {"explore", "code-explorer"};
    if (canonical == "explorer") return {"explore", "code-explorer", "explorer"};
    if (canonical == "plan") return {"plan"};
    if (canonical == "planner") return {"plan"};
    return {canonical};
}
[[nodiscard]] std::optional<std::string> find_canonical_agent_type_match(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents
) {
    const auto canonical_requested = canonicalize_agent_type(requested_type);
    for (const auto& agent : agents) {
        if (canonicalize_agent_type(agent.agent_type) == canonical_requested) {
            return agent.agent_type;
        }
    }
    return std::nullopt;
}
[[nodiscard]] std::optional<std::string> resolve_requested_agent_type(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents
) {
    const auto requested = trim(requested_type);
    // migrated edge case: empty/whitespace-only input returns nullopt
    if (requested.empty()) return std::nullopt;

    // migrated edge case: exact match preserves original casing ("Plan" != canonical "plan")
    for (const auto& agent : agents) {
        if (agent.agent_type == requested) return agent.agent_type;
    }

    // migrated edge case: canonical match folds case, underscores, spaces to dashes
    if (auto canonical = find_canonical_agent_type_match(requested, agents)) {
        return canonical;
    }

    // migrated edge case: legacy alias table (explore/explorer/plan/planner)
    for (const auto& alias : agent_alias_candidates(requested)) {
        if (auto alias_match = find_canonical_agent_type_match(alias, agents)) {
            return alias_match;
        }
    }

    // migrated edge case: suffix match `:alias` requires exactly one candidate
    for (const auto& alias : agent_alias_candidates(requested)) {
        const auto suffix = ":" + alias;
        std::vector<std::string> matches;
        for (const auto& agent : agents) {
            const auto canonical = canonicalize_agent_type(agent.agent_type);
            if (canonical.ends_with(suffix)) matches.push_back(agent.agent_type);
        }
        // migrated edge case: ambiguous suffix match (e.g. two code-explorer
        // variants under different namespaces) returns nullopt instead of first
        if (matches.size() == 1) return matches.front();
    }

    // migrated edge case: no compatible match returns nullopt (caller surfaces error)
    return std::nullopt;
}
[[nodiscard]] std::expected<AgentDefinition, ResolutionError> resolve_agent_type(
    std::string_view id,
    const std::vector<AgentDefinition>& agents
) {
    const auto requested = trim(id);
    if (requested.empty()) return std::unexpected(ResolutionError::EmptyRequestedType);

    auto resolved = resolve_requested_agent_type(requested, agents);
    if (!resolved) {
        const auto canonical = canonicalize_agent_type(requested);
        const bool is_legacy_alias =
            canonical == "explore" || canonical == "explorer" ||
            canonical == "plan" || canonical == "planner";
        if (is_legacy_alias) {
            bool ambiguous = false;
            for (const auto& alias : agent_alias_candidates(requested)) {
                const auto suffix = ":" + alias;
                std::size_t count = 0;
                for (const auto& agent : agents) {
                    if (canonicalize_agent_type(agent.agent_type).ends_with(suffix)) ++count;
                }
                if (count > 1) { ambiguous = true; break; }
            }
            return std::unexpected(ambiguous
                ? ResolutionError::LegacyAliasAmbiguous
                : ResolutionError::LegacyAliasNoMatch);
        }
        return std::unexpected(ResolutionError::NoCompatibleMatch);
    }

    for (const auto& agent : agents) {
        if (agent.agent_type == *resolved) return agent;
    }
    return std::unexpected(ResolutionError::NoCompatibleMatch);
}
[[nodiscard]] std::string format_agent_type_list(
    const std::vector<AgentDefinition>& agents
) {
    std::string output;
    for (const auto& agent : agents) {
        if (!output.empty()) output += ", ";
        output += agent.agent_type;
    }
    return output;
}
[[nodiscard]] bool is_sdk_entrypoint() {
    const char* entrypoint = std::getenv("LOOM_ENTRYPOINT");
    const std::string_view entry = entrypoint ? std::string_view(entrypoint) : std::string_view{};
    return entry == "sdk-ts" || entry == "sdk-py" || entry == "sdk-cli";
}
[[nodiscard]] bool is_fork_subagent_enabled() {
    if (env_truthy("FORK_SUBAGENT")) {
        if (env_truthy("CC_COORDINATOR_MODE")) return false;
        if (env_truthy("CC_NON_INTERACTIVE")) return false;
        return true;
    }
    return false;
}
[[nodiscard]] std::string build_fork_child_message(std::string_view directive) {
    return std::format(
        R"(<{}>
STOP. READ THIS FIRST.

You are a forked worker process. You are NOT the main agent.

RULES (non-negotiable):
1. Your system prompt says "default to forking." IGNORE IT — that's for the parent. You ARE the fork. Do NOT spawn sub-agents; execute directly.
2. Do NOT converse, ask questions, or suggest next steps
3. Do NOT editorialize or add meta-commentary
4. USE your tools directly: Bash, Read, Write, etc.
5. If you modify files, commit your changes before reporting. Include the commit hash in your report.
6. Do NOT emit text between tool calls. Use tools silently, then report once at the end.
7. Stay strictly within your directive's scope. If you discover related systems outside your scope, mention them in one sentence at most — other workers cover those areas.
8. Keep your report under 500 words unless the directive specifies otherwise. Be factual and concise.
9. Your response MUST begin with "Scope:". No preamble, no thinking-out-loud.
10. REPORT structured facts, then stop

Output format (plain text labels, not markdown headers):
  Scope: <echo back your assigned scope in one sentence>
  Result: <the answer or key findings, limited to the scope above>
  Key files: <relevant file paths — include for research tasks>
  Files changed: <list with commit hash — include only if you modified files>
  Issues: <list — include only if there are issues to flag>
</{}>

{}{})",
        kForkBoilerplateTag,
        kForkBoilerplateTag,
        kForkDirectivePrefix,
        directive);
}
[[nodiscard]] std::string build_worktree_fork_notice(
    std::string_view parent_cwd,
    std::string_view worktree_cwd
) {
    return std::format(
        "You've inherited the conversation context above from a parent agent working in {}. "
        "You are operating in an isolated git worktree at {} — same repository, same relative "
        "file structure, separate working copy. Paths in the inherited context refer to the "
        "parent's working directory; translate them to your worktree root. Re-read files "
        "before editing if the parent may have modified them since they appear in the context. "
        "Your changes stay in this worktree and will not affect the parent's files.",
        parent_cwd,
        worktree_cwd);
}
[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}
[[nodiscard]] std::string safe_agent_filename(std::string_view agent_id) {
    std::string out;
    out.reserve(agent_id.size());
    for (char ch : agent_id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "agent" : out;
}
[[nodiscard]] std::string xml_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}
[[nodiscard]] std::pair<std::string_view, std::string_view> split_transcript_role(
    std::string_view entry
) {
    const auto sep = entry.find(": ");
    if (sep == std::string_view::npos || sep == 0) return {"system", entry};
    auto role = entry.substr(0, sep);
    if (role != "user" && role != "assistant" && role != "system" && role != "tool" && role != "hook") {
        return {"system", entry};
    }
    return {role, entry.substr(sep + 2)};
}
[[nodiscard]] std::string sidechain_message_role(std::string_view role) {
    if (role == "user" || role == "assistant" || role == "system") return std::string(role);
    return "system";
}
} // namespace cc::tools::agent_runtime
