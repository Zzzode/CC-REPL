// Implementation unit for cc.tools.agent.utils — env/config/memory path
// resolution, model alias resolution, effort beta-header handling,
// permission-mode normalization, canonical tool-name string utilities,
// identity/teammate-name helpers, and the built-in system prompts.
module;

module cc.tools.agent.utils;

import std;

import cc.utils.git;
import cc.utils.env_utils;
import cc.utils.parse_int;
import cc.utils.team_helpers;
import cc.tools.team;
import cc.services.api.client;

namespace cc::tools::agent::utils {

namespace fs = std::filesystem;

[[nodiscard]] bool is_auto_memory_enabled() {
    const char* disable = std::getenv("LOOM_DISABLE_AUTO_MEMORY");
    if (cc::utils::is_env_truthy(disable)) return false;
    if (cc::utils::is_env_defined_falsy(disable)) return true;
    if (cc::utils::is_env_truthy(std::getenv("LOOM_SIMPLE"))) return false;
    if (cc::utils::is_env_truthy(std::getenv("LOOM_REMOTE")) &&
        (!std::getenv("LOOM_REMOTE_MEMORY_DIR") || !*std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"))) {
        return false;
    }
    return true;
}

[[nodiscard]] fs::path config_home_dir() {
    if (const char* configured = std::getenv("LOOM_CONFIG_DIR"); configured && *configured) {
        return fs::path{configured};
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path{home} / ".loom";
    }
    return fs::path{".loom"};
}

[[nodiscard]] fs::path agent_memory_base_dir() {
    if (const char* remote = std::getenv("LOOM_REMOTE_MEMORY_DIR"); remote && *remote) {
        return fs::path{remote};
    }
    return config_home_dir();
}

[[nodiscard]] std::string sanitize_agent_memory_component(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(ch == ':' ? '-' : ch);
    }
    return out.empty() ? std::string{"agent"} : out;
}

[[nodiscard]] std::string agent_memory_dir(
    std::string_view agent_type,
    std::string_view scope,
    const std::optional<std::string>& working_dir
) {
    const auto dir_name = sanitize_agent_memory_component(agent_type);
    const auto cwd = working_dir && !working_dir->empty() ? fs::path{*working_dir} : fs::current_path();
    if (scope == "project") {
        return ((cwd / ".loom" / "agent-memory" / dir_name).string() + fs::path::preferred_separator);
    }
    if (scope == "local") {
        if (const char* remote = std::getenv("LOOM_REMOTE_MEMORY_DIR"); remote && *remote) {
            const auto git_root = cc::utils::git::find_git_root(cwd).value_or(cwd);
            const auto project_component = sanitize_agent_memory_component(git_root.string());
            return ((fs::path{remote} / "projects" / project_component / "agent-memory-local" / dir_name).string() +
                fs::path::preferred_separator);
        }
        return ((cwd / ".loom" / "agent-memory-local" / dir_name).string() + fs::path::preferred_separator);
    }
    return ((agent_memory_base_dir() / "agent-memory" / dir_name).string() + fs::path::preferred_separator);
}

[[nodiscard]] std::string agent_memory_scope_note(std::string_view scope) {
    if (scope == "project") {
        return "- Since this memory is project-scope and shared with your team via version control, tailor your memories to this project";
    }
    if (scope == "local") {
        return "- Since this memory is local-scope (not checked into version control), tailor your memories to this project and machine";
    }
    return "- Since this memory is user-scope, keep learnings general since they apply across all projects";
}

[[nodiscard]] std::string join_agent_memory_prompt_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) out += '\n';
        out += lines[i];
    }
    return out;
}

[[nodiscard]] std::string load_agent_memory_prompt(
    std::string_view agent_type,
    std::string_view scope,
    const std::optional<std::string>& working_dir
) {
    const auto dir = agent_memory_dir(agent_type, scope, working_dir);
    std::error_code ec;
    fs::create_directories(fs::path{dir}, ec);
    return join_agent_memory_prompt_lines({
        "# Persistent Agent Memory",
        "",
        "You have a persistent, file-based memory system at `" + dir + "`.",
        "This directory already exists; write to it directly with the Write tool.",
        "",
        "If the user explicitly asks you to remember something, save it immediately as the best fitting memory type. If they ask you to forget something, find and remove the relevant entry.",
        "",
        "## How to save memories",
        "Write each memory to its own file and keep `MEMORY.md` as a concise index of links to those files.",
        "Use descriptive filenames, avoid duplicate memories, and update or remove memories that are wrong or outdated.",
        "",
        "## When to access memories",
        "Access memory when it seems relevant, when the user refers to prior work, or when the user explicitly asks you to check, recall, or remember.",
        "If the user asks you to ignore memory, proceed as if `MEMORY.md` were empty.",
        "",
        agent_memory_scope_note(scope),
    });
}

void add_agent_memory_tools(std::vector<std::string>& tools) {
    if (tools.empty()) return;
    for (std::string_view tool : {"Write", "Edit", "Read"}) {
        if (!std::ranges::contains(tools, tool)) {
            tools.emplace_back(tool);
        }
    }
}

[[nodiscard]] std::string next_agent_id(const std::optional<std::string>& preferred_name) {
    if (preferred_name && !preferred_name->empty()) return *preferred_name;
    static std::atomic<std::uint64_t> counter{0};
    return std::format("agent-{}", counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

[[nodiscard]] std::string sanitize_teammate_agent_name(std::string name) {
    if (name.empty()) return "agent";
    for (auto& ch : name) {
        if (ch == '@') ch = '-';
    }
    return name;
}

[[nodiscard]] std::string lowercase_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

[[nodiscard]] std::string teammate_name_from_agent_id(std::string_view agent_id) {
    const auto at = agent_id.find('@');
    if (at == std::string_view::npos) return std::string{agent_id};
    return std::string{agent_id.substr(0, at)};
}

[[nodiscard]] std::string unique_teammate_agent_name(
    std::string base_name,
    const cc::tools::Team& team
) {
    if (base_name.empty()) base_name = "agent";

    std::unordered_set<std::string> existing_names;
    for (const auto& member : team.members) {
        existing_names.insert(lowercase_ascii(teammate_name_from_agent_id(member.agent_id)));
    }

    const auto base_key = lowercase_ascii(base_name);
    if (!existing_names.contains(base_key)) return base_name;

    int suffix = 2;
    while (existing_names.contains(lowercase_ascii(std::format("{}-{}", base_name, suffix)))) {
        ++suffix;
    }
    return std::format("{}-{}", base_name, suffix);
}

[[nodiscard]] std::string format_teammate_agent_id(std::string_view agent_name, std::string_view team_name) {
    return std::format("{}@{}", agent_name, team_name);
}

[[nodiscard]] bool current_session_is_teammate() {
    if (cc::utils::is_in_process_teammate()) return true;
    auto agent_id = cc::utils::get_agent_id();
    auto team_name = cc::utils::get_team_name();
    return agent_id && !agent_id->empty() && team_name && !team_name->empty();
}

[[nodiscard]] std::optional<std::string> resolve_agent_model(std::optional<std::string> model) {
    if (!model || model->empty() || *model == "inherit") return std::nullopt;
    if (*model == "sonnet") return "claude-sonnet-4-20250514";
    if (*model == "opus") return "claude-opus-4-20250514";
    if (*model == "haiku") return "claude-3-5-haiku-20241022";
    return model;
}

[[nodiscard]] std::string join_fields(const std::vector<std::string>& fields) {
    std::string output;
    for (const auto& field : fields) {
        if (!output.empty()) output += ", ";
        output += field;
    }
    return output;
}

[[nodiscard]] std::string built_in_system_prompt(std::string_view agent_type) {
    if (agent_type == "Explore") {
        return R"(You are an exploration agent. Your job is to understand codebases, find relevant files, and gather context.

Guidelines:
- Use search and read tools extensively.
- Summarize findings concisely.
- Identify key files, patterns, and architecture.
- Report dependencies and relationships between components.
- Do not make code changes.)";
    }
    if (agent_type == "Plan") {
        return R"(You are a planning agent. Your job is to create detailed implementation plans.

Guidelines:
- Break down tasks into clear, sequential steps.
- Identify risks and dependencies.
- Suggest testing strategies.
- Consider edge cases.
- Output a structured plan.
- Do not implement the plan yourself.)";
    }
    if (agent_type == "verification") {
        return R"(You are a verification agent. Your job is to test and validate completed work.

Guidelines:
- Run targeted checks when tools are available.
- Verify code compiles or builds successfully.
- Check for regressions.
- Validate that requirements are met.
- Report issues with specific evidence.)";
    }
    return R"(You are a sub-agent working on a delegated task. Complete the task thoroughly.

Guidelines:
- Focus only on the assigned task.
- Use available tools effectively.
- Report results concisely.
- If blocked, explain what is needed to proceed.)";
}

[[nodiscard]] std::string_view trim_tool_rule(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::string_view permission_rule_tool_name(std::string_view rule) {
    rule = trim_tool_rule(rule);
    const auto paren = rule.find('(');
    if (paren != std::string_view::npos) {
        rule = rule.substr(0, paren);
    }
    return trim_tool_rule(rule);
}

[[nodiscard]] std::string canonical_tool_name(std::string_view value) {
    auto trimmed = trim_tool_rule(value);
    std::string out;
    out.reserve(trimmed.size());
    for (char ch : trimmed) {
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

[[nodiscard]] bool env_flag_enabled(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') return false;
    const auto value = lowercase_ascii(raw);
    return value != "0" && value != "false" && value != "no" && value != "off";
}

[[nodiscard]] bool is_ant_user() {
    if (const char* value = std::getenv("USER_TYPE")) {
        return std::string_view(value) == "ant";
    }
    return false;
}

[[nodiscard]] bool agent_model_supports_effort(std::string_view model) {
    if (env_flag_enabled("LOOM_ALWAYS_ENABLE_EFFORT")) return true;
    const auto lower = lowercase_ascii(model);
    if (lower.find("opus-4-6") != std::string::npos ||
        lower.find("sonnet-4-6") != std::string::npos) {
        return true;
    }
    if (lower.find("haiku") != std::string::npos ||
        lower.find("sonnet") != std::string::npos ||
        lower.find("opus") != std::string::npos) {
        return false;
    }
    return true;
}

[[nodiscard]] bool agent_model_supports_max_effort(std::string_view model) {
    return is_ant_user() || lowercase_ascii(model).find("opus-4-6") != std::string::npos;
}

[[nodiscard]] bool is_agent_effort_level(std::string_view value) {
    return value == "low" || value == "medium" || value == "high" || value == "max";
}

[[nodiscard]] std::optional<std::string> normalized_permission_mode(
    const std::optional<std::string>& mode
) {
    if (!mode) return std::nullopt;
    const auto trimmed = trim_tool_rule(*mode);
    if (trimmed.empty()) return std::nullopt;
    return std::string{trimmed};
}

[[nodiscard]] bool parent_permission_mode_blocks_agent_override(
    const std::optional<std::string>& parent_mode
) {
    const auto mode = normalized_permission_mode(parent_mode);
    if (!mode) return false;
    return *mode == "bypassPermissions" || *mode == "acceptEdits" || *mode == "auto";
}

[[nodiscard]] std::optional<std::string> effective_agent_permission_mode(
    const std::optional<std::string>& request_mode,
    const std::optional<std::string>& definition_mode,
    const std::optional<std::string>& parent_mode
) {
    if (auto explicit_mode = normalized_permission_mode(request_mode)) return explicit_mode;
    auto normalized_parent_mode = normalized_permission_mode(parent_mode);
    if (parent_permission_mode_blocks_agent_override(normalized_parent_mode)) {
        return normalized_parent_mode;
    }
    if (auto agent_mode = normalized_permission_mode(definition_mode)) return agent_mode;
    return normalized_parent_mode;
}

[[nodiscard]] std::optional<int> parse_agent_numeric_effort(std::string_view value) {
    value = trim_tool_rule(value);
    if (value.empty()) return std::nullopt;
    int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = cc::utils::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return parsed;
}

void append_agent_effort_beta(CreateMessageRequest& request) {
    static constexpr std::string_view k_effort_beta_header = "effort-2025-11-24";
    if (!std::ranges::contains(request.betas, k_effort_beta_header)) {
        request.betas.emplace_back(k_effort_beta_header);
    }
}

void apply_agent_effort_to_request(
    CreateMessageRequest& request,
    const std::optional<std::string>& effort
) {
    if (!effort) return;
    const auto trimmed = trim_tool_rule(*effort);
    if (trimmed.empty()) return;

    if (auto numeric = parse_agent_numeric_effort(trimmed)) {
        if (is_ant_user()) {
            request.internal_effort_override = *numeric;
        }
        return;
    }

    if (!agent_model_supports_effort(request.model)) return;
    auto level = lowercase_ascii(trimmed);
    if (!is_agent_effort_level(level)) return;
    if (level == "max" && !agent_model_supports_max_effort(request.model)) {
        level = "high";
    }
    request.output_effort = std::move(level);
    append_agent_effort_beta(request);
}

[[nodiscard]] std::string prepend_initial_prompt(
    const std::optional<std::string>& initial_prompt,
    std::string_view prompt
) {
    if (!initial_prompt || initial_prompt->empty()) return std::string(prompt);
    return std::format("{}\n\n{}", *initial_prompt, prompt);
}

[[nodiscard]] std::string format_critical_system_reminder(std::string_view reminder) {
    return std::format("<critical_system_reminder>\n{}\n</critical_system_reminder>", reminder);
}

} // namespace cc::tools::agent::utils
