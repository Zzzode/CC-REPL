// AgentTool - Sub-agent delegation with recursive API loop
module;

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include <utility>
#include <sstream>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <sys/wait.h>

export module cc.tools.agent;

import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;
import cc.tools.agent_runtime;
import cc.tools.send_message;
import cc.tools.mcp;
import cc.skills.skill;
import cc.services.api.client;
import cc.services.api.streaming;
import cc.services.api.bootstrap;
import cc.services.mcp.types;

export namespace cc::tools::agent {

namespace fs = std::filesystem;

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;
using cc::services::api::AnthropicClient;
using cc::services::api::CreateMessageRequest;
using cc::services::api::Message;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;
using cc::services::api::StreamParser;
using cc::services::api::StreamEventType;
using cc::services::api::StreamContentBlockType;
using cc::services::api::get_default_client;

// =========================================================================
// Agent Configuration
// =========================================================================

struct AgentConfig {
    int max_turns = 200;           // Max agentic loop iterations
    int max_depth = 3;             // Max recursive agent nesting depth
    std::string default_model = "claude-sonnet-4-20250514";
    std::vector<std::string> allowed_tools;  // Empty = inherit all from parent
    std::vector<std::string> denied_tools;   // Explicitly blocked tools for sub-agents
};

[[nodiscard]] inline SchemaProperty agent_schema_property(
    std::string name,
    std::string type,
    std::string description,
    bool required = false
) {
    return SchemaProperty{
        std::move(name),
        std::move(type),
        std::move(description),
        required,
        std::nullopt,
        std::nullopt
    };
}

struct AgentToolRequest {
    std::string prompt;
    std::optional<std::string> description;
    std::string subagent_type = "general-purpose";
    std::optional<std::string> model;
    bool run_in_background = false;
    std::optional<std::string> name;
    std::optional<std::string> team_name;
    std::optional<std::string> mode;
    std::optional<std::string> isolation;
    std::optional<std::string> cwd;
};

struct AgentMcpToolBinding {
    std::string server_name;
    std::string tool_name;
    std::string description;
};

struct AgentExecutionPlan {
    std::string agent_id;
    std::string prompt;
    std::string agent_type;
    std::string model;
    std::string system_prompt;
    std::vector<std::string> preloaded_skill_messages;
    std::vector<std::string> agent_mcp_servers;
    std::vector<AgentMcpToolBinding> agent_mcp_tools;
    std::optional<std::string> agent_mcp_context_message;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> disallowed_tools;
    int max_turns = 200;
    bool background = false;
    std::optional<std::string> name;
    std::optional<std::string> team_name;
    std::optional<std::string> mode;
    std::optional<std::string> isolation;
    std::optional<std::string> working_dir;
    cc::tools::agent_runtime::AgentHooksByEvent frontmatter_hooks;
};

[[nodiscard]] inline std::optional<std::string> json_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] inline bool json_bool(
    cc::utils::json::JsonVal root,
    std::string_view key,
    bool fallback = false
) {
    auto value = root.get(key);
    return value.is_bool() ? value.as_bool() : fallback;
}

[[nodiscard]] inline bool has_non_empty_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    return value.is_str() && !value.as_str().empty();
}

[[nodiscard]] inline std::string next_agent_id(const std::optional<std::string>& preferred_name) {
    if (preferred_name && !preferred_name->empty()) return *preferred_name;
    static std::atomic<std::uint64_t> counter{0};
    return std::format("agent-{}", counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

[[nodiscard]] inline std::optional<std::string> resolve_agent_model(std::optional<std::string> model) {
    if (!model || model->empty() || *model == "inherit") return std::nullopt;
    if (*model == "sonnet") return "claude-sonnet-4-20250514";
    if (*model == "opus") return "claude-opus-4-20250514";
    if (*model == "haiku") return "claude-3-5-haiku-20241022";
    return model;
}

[[nodiscard]] inline std::expected<AgentToolRequest, std::string> parse_agent_tool_request(
    const ToolInput& input
) {
    auto doc = cc::utils::json::parse(input.json());
    if (!doc) return std::unexpected(std::string(doc.error().format()));

    auto root = doc->root();
    if (!root.is_obj()) return std::unexpected("Agent input must be a JSON object");

    AgentToolRequest request;
    if (auto prompt = json_string(root, "prompt").or_else([&] { return json_string(root, "task"); })) {
        request.prompt = *prompt;
    }
    request.description = json_string(root, "description");
    if (auto subagent = json_string(root, "subagent_type").or_else([&] { return json_string(root, "skill"); })) {
        if (!subagent->empty()) request.subagent_type = *subagent;
    }
    request.model = resolve_agent_model(json_string(root, "model"));
    request.run_in_background = json_bool(root, "run_in_background", false);
    request.name = json_string(root, "name");
    request.team_name = json_string(root, "team_name").or_else([&] { return json_string(root, "teamName"); });
    request.mode = json_string(root, "mode")
        .or_else([&] { return json_string(root, "permission_mode"); })
        .or_else([&] { return json_string(root, "permissionMode"); });
    request.isolation = json_string(root, "isolation");
    request.cwd = json_string(root, "cwd");
    return request;
}

[[nodiscard]] inline std::string join_fields(const std::vector<std::string>& fields) {
    std::string output;
    for (const auto& field : fields) {
        if (!output.empty()) output += ", ";
        output += field;
    }
    return output;
}

[[nodiscard]] inline std::string built_in_system_prompt(std::string_view agent_type) {
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

[[nodiscard]] inline bool tool_name_allowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& allowed_tools
) {
    if (allowed_tools.empty()) return true;
    for (const auto& allowed : allowed_tools) {
        if (allowed == "*" || allowed == tool_name) return true;
    }
    return false;
}

[[nodiscard]] inline bool tool_name_disallowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& disallowed_tools
) {
    for (const auto& disallowed : disallowed_tools) {
        if (disallowed == "*" || disallowed == tool_name) return true;
    }
    return false;
}

[[nodiscard]] inline std::string lowercase_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

[[nodiscard]] inline bool case_insensitive_contains(std::string_view haystack, std::string_view needle) {
    return lowercase_copy(haystack).contains(lowercase_copy(needle));
}

[[nodiscard]] inline std::vector<std::string> available_mcp_servers_with_tools() {
    std::vector<std::string> names;
    for (const auto& status : cc::tools::native_mcp_statuses()) {
        if (status.status == "ready" && !status.tools.empty()) {
            names.push_back(status.name);
        }
    }
    return names;
}

[[nodiscard]] inline std::vector<std::string> missing_required_mcp_servers(
    const std::vector<std::string>& required_patterns,
    const std::vector<std::string>& available_servers
) {
    std::vector<std::string> missing;
    for (const auto& pattern : required_patterns) {
        bool matched = false;
        for (const auto& server : available_servers) {
            if (case_insensitive_contains(server, pattern)) {
                matched = true;
                break;
            }
        }
        if (!matched) missing.push_back(pattern);
    }
    return missing;
}

[[nodiscard]] inline std::string prepend_initial_prompt(
    const std::optional<std::string>& initial_prompt,
    std::string_view prompt
) {
    if (!initial_prompt || initial_prompt->empty()) return std::string(prompt);
    return std::format("{}\n\n{}", *initial_prompt, prompt);
}

[[nodiscard]] inline std::optional<std::string> resolve_agent_skill_name(
    std::string_view skill_name,
    const std::vector<cc::skills::SkillDefinition>& skills,
    const cc::tools::agent_runtime::AgentDefinition& agent_definition
) {
    for (const auto& skill : skills) {
        if (skill.name == skill_name) return skill.name;
    }

    const auto colon = agent_definition.agent_type.find(':');
    if (colon != std::string::npos) {
        const auto qualified = std::format("{}:{}", agent_definition.agent_type.substr(0, colon), skill_name);
        for (const auto& skill : skills) {
            if (skill.name == qualified) return skill.name;
        }
    }

    const auto suffix = std::format(":{}", skill_name);
    for (const auto& skill : skills) {
        if (skill.name.ends_with(suffix)) return skill.name;
    }

    return std::nullopt;
}

[[nodiscard]] inline std::string format_preloaded_skill_message(
    std::string_view skill_name,
    std::string_view content
) {
    return std::format("<skill name=\"{}\">\n{}\n</skill>", skill_name, content);
}

[[nodiscard]] inline std::vector<std::string> load_preloaded_skill_messages(
    const cc::tools::agent_runtime::AgentDefinition& definition
) {
    std::vector<std::string> messages;
    if (definition.skills.empty()) return messages;

    cc::skills::SkillLoader loader;
    std::vector<std::pair<std::string, fs::path>> plugin_skill_paths;
    for (const auto& plugin : cc::tools::agent_runtime::discover_plugin_component_paths()) {
        for (const auto& path : plugin.skills_paths) {
            plugin_skill_paths.emplace_back(plugin.plugin_name, path);
        }
    }
    auto discovered = loader.discover_all_with_plugin_skills(plugin_skill_paths);
    if (!discovered) return messages;

    for (const auto& requested : definition.skills) {
        auto resolved_name = resolve_agent_skill_name(requested, *discovered, definition);
        if (!resolved_name) continue;

        for (const auto& skill : *discovered) {
            if (skill.name != *resolved_name) continue;
            messages.push_back(format_preloaded_skill_message(skill.name, skill.content));
            break;
        }
    }
    return messages;
}

[[nodiscard]] inline std::string format_agent_mcp_context_message(
    const std::vector<AgentMcpToolBinding>& tools
) {
    if (tools.empty()) return {};

    std::string message = "The following MCP tools are available to this agent through the `mcp` tool.\n";
    message += "Call `mcp` with `server_name`, `tool_name`, and `arguments`.\n\n";
    for (const auto& tool : tools) {
        message += std::format("- {}/{}", tool.server_name, tool.tool_name);
        if (!tool.description.empty()) message += std::format(": {}", tool.description);
        message += "\n";
    }
    return message;
}

[[nodiscard]] inline std::vector<AgentMcpToolBinding> connect_agent_mcp_servers(
    const std::vector<std::string>& server_names
) {
    std::vector<AgentMcpToolBinding> tools;
    for (const auto& server_name : server_names) {
        if (server_name.empty()) continue;

        auto status = cc::tools::restart_native_mcp_server(server_name);
        if (!status || status->status != "ready") continue;

        for (const auto& tool : status->tools) {
            tools.push_back(AgentMcpToolBinding{
                .server_name = status->name,
                .tool_name = tool.name,
                .description = tool.description,
            });
        }
    }
    return tools;
}

[[nodiscard]] inline cc::tools::NativeMcpConfiguredServer to_native_agent_mcp_server(
    const cc::tools::agent_runtime::AgentInlineMcpServerConfig& config
) {
    cc::tools::NativeMcpConfiguredServer server;
    server.name = config.name;
    server.command = config.command;
    server.args = config.args;
    server.env = config.env;
    server.url = config.url;
    server.headers = config.headers;
    server.headers_helper = config.headers_helper;

    const auto transport = lowercase_copy(config.transport);
    if (transport == "sse") {
        server.transport = cc::services::mcp::TransportType::Sse;
    } else if (transport == "http" || transport == "streamable-http" || transport == "streamablehttp" ||
               (!server.url.empty() && server.command.empty())) {
        server.transport = cc::services::mcp::TransportType::StreamableHttp;
    } else {
        server.transport = cc::services::mcp::TransportType::Stdio;
    }
    return server;
}

inline void append_unique_agent_mcp_server(
    std::vector<std::string>& names,
    std::string name
) {
    if (name.empty()) return;
    for (const auto& existing : names) {
        if (existing == name) return;
    }
    names.push_back(std::move(name));
}

[[nodiscard]] inline std::expected<void, std::string> upsert_agent_inline_mcp_servers(
    const std::vector<cc::tools::agent_runtime::AgentInlineMcpServerConfig>& configs
) {
    if (configs.empty()) return {};
    std::vector<cc::tools::NativeMcpConfiguredServer> servers;
    servers.reserve(configs.size());
    for (const auto& config : configs) {
        servers.push_back(to_native_agent_mcp_server(config));
    }
    return cc::tools::upsert_native_mcp_servers(std::move(servers));
}

[[nodiscard]] inline std::string format_agent_runtime_context(
    const AgentExecutionPlan& plan
) {
    std::string context = "Native agent runtime context:\n";
    context += std::format("- agent_id: {}\n", plan.agent_id);
    if (plan.name) context += std::format("- name: {}\n", *plan.name);
    if (plan.team_name) context += std::format("- team_name: {}\n", *plan.team_name);
    if (plan.working_dir) context += std::format("- cwd: {}\n", *plan.working_dir);
    if (plan.isolation) context += std::format("- isolation: {}\n", *plan.isolation);
    if (plan.mode) context += std::format("- permission_mode: {}\n", *plan.mode);
    if (plan.background) context += "- background: true\n";
    return context;
}

[[nodiscard]] inline std::string shell_quote(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

[[nodiscard]] inline std::string sanitized_agent_file_part(std::string_view agent_id) {
    std::string out;
    out.reserve(agent_id.size());
    for (char ch : agent_id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "agent" : out;
}

[[nodiscard]] inline std::string default_agent_transcript_path(std::string_view agent_id) {
    auto base = fs::temp_directory_path();
    return (base / std::format("cc-repl-agent-{}.jsonl", sanitized_agent_file_part(agent_id))).string();
}

[[nodiscard]] inline bool hook_condition_allows(const cc::tools::agent_runtime::AgentHookCommand& hook) {
    if (!hook.condition || hook.condition->empty()) return true;
    const auto condition = lowercase_copy(*hook.condition);
    return condition == "true" || condition == "1" || condition == "yes";
}

[[nodiscard]] inline bool hook_pattern_matches_one(std::string_view pattern, std::string_view value) {
    if (pattern.empty() || pattern == "*") return true;
    if (pattern == value) return true;
    if (pattern.ends_with("*")) return value.starts_with(pattern.substr(0, pattern.size() - 1));
    if (pattern.starts_with("*")) return value.ends_with(pattern.substr(1));
    return false;
}

[[nodiscard]] inline bool hook_pattern_matches(
    const std::optional<std::string>& pattern,
    std::string_view value
) {
    if (!pattern || pattern->empty()) return true;
    std::size_t start = 0;
    while (start <= pattern->size()) {
        auto sep = pattern->find('|', start);
        auto part = std::string_view(*pattern).substr(
            start,
            sep == std::string::npos ? std::string_view::npos : sep - start);
        if (hook_pattern_matches_one(part, value)) return true;
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return false;
}

struct AgentHookRunResult {
    int exit_code = 0;
    std::string output;
};

[[nodiscard]] inline AgentHookRunResult run_agent_command_hook(
    const cc::tools::agent_runtime::AgentHookCommand& hook,
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message
) {
    const auto transcript_path = default_agent_transcript_path(plan.agent_id);
    const auto cwd = plan.working_dir.value_or(fs::current_path().string());
    const auto shell = hook.shell.empty() ? std::string("bash") : hook.shell;
    std::string command;
    command += "cd " + shell_quote(cwd) + " && ";
    command += "CLAUDE_HOOK_EVENT=" + shell_quote(event) + " ";
    command += "CLAUDE_HOOK_AGENT_ID=" + shell_quote(plan.agent_id) + " ";
    command += "CLAUDE_HOOK_AGENT_TYPE=" + shell_quote(plan.agent_type) + " ";
    command += "CLAUDE_HOOK_AGENT_TRANSCRIPT_PATH=" + shell_quote(transcript_path) + " ";
    command += "CLAUDE_HOOK_CWD=" + shell_quote(cwd) + " ";
    if (!last_assistant_message.empty()) {
        command += "CLAUDE_HOOK_LAST_ASSISTANT_MESSAGE=" + shell_quote(last_assistant_message) + " ";
    }
    command += shell_quote(shell) + " -c " + shell_quote(hook.command) + " 2>&1";

    AgentHookRunResult result;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        result.exit_code = 127;
        result.output = "failed to start hook command";
        return result;
    }

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        result.output += buffer.data();
        if (result.output.size() > 64 * 1024) {
            result.output.resize(64 * 1024);
            break;
        }
    }
    const auto status = ::pclose(pipe);
    if (status == -1) {
        result.exit_code = 127;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

struct AgentHookExecutionResult {
    int hook_count = 0;
    std::string output;
    std::optional<std::string> error;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

[[nodiscard]] inline AgentHookExecutionResult execute_agent_frontmatter_hooks(
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message = {}
) {
    AgentHookExecutionResult aggregate;
    auto it = plan.frontmatter_hooks.find(std::string(event));
    if (it == plan.frontmatter_hooks.end()) return aggregate;

    for (const auto& matcher : it->second) {
        if (!hook_pattern_matches(matcher.matcher, plan.agent_type)) continue;
        for (const auto& hook : matcher.hooks) {
            if (!hook_condition_allows(hook)) continue;
            auto result = run_agent_command_hook(hook, plan, event, last_assistant_message);
            ++aggregate.hook_count;
            if (!result.output.empty()) {
                if (!aggregate.output.empty()) aggregate.output += "\n";
                aggregate.output += result.output;
            }
            if (result.exit_code != 0 && !aggregate.error) {
                aggregate.error = std::format(
                    "{} hook for agent '{}' exited with code {}{}{}",
                    event,
                    plan.agent_id,
                    result.exit_code,
                    result.output.empty() ? "" : ": ",
                    result.output.empty() ? "" : result.output);
            }
        }
    }
    return aggregate;
}

[[nodiscard]] inline std::expected<std::optional<std::string>, std::string> normalize_agent_cwd(
    const std::optional<std::string>& cwd
) {
    if (!cwd || cwd->empty()) return std::optional<std::string>{};
    std::error_code ec;
    fs::path path = fs::path{*cwd};
    if (path.is_relative()) path = fs::current_path(ec) / path;
    if (ec) return std::unexpected(std::format("Cannot resolve current working directory: {}", ec.message()));
    path = fs::weakly_canonical(path, ec);
    if (ec) return std::unexpected(std::format("Cannot resolve agent cwd '{}': {}", *cwd, ec.message()));
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) {
        return std::unexpected(std::format("Agent cwd does not exist or is not a directory: {}", path.string()));
    }
    return path.string();
}

class ScopedCurrentPath {
public:
    ScopedCurrentPath() = default;
    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath(ScopedCurrentPath&& other) noexcept
        : previous_(std::move(other.previous_)), active_(other.active_) {
        other.active_ = false;
    }
    ScopedCurrentPath& operator=(ScopedCurrentPath&& other) noexcept {
        if (this != &other) {
            restore();
            previous_ = std::move(other.previous_);
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }
    ~ScopedCurrentPath() { restore(); }

    [[nodiscard]] static std::expected<ScopedCurrentPath, std::string> enter(std::string_view path_text) {
        std::error_code ec;
        auto previous = fs::current_path(ec);
        if (ec) return std::unexpected(std::format("Cannot read current working directory: {}", ec.message()));
        fs::current_path(fs::path{path_text}, ec);
        if (ec) return std::unexpected(std::format("Cannot enter agent cwd '{}': {}", path_text, ec.message()));
        ScopedCurrentPath guard;
        guard.previous_ = std::move(previous);
        guard.active_ = true;
        return guard;
    }

private:
    void restore() {
        if (!active_) return;
        std::error_code ec;
        fs::current_path(previous_, ec);
        active_ = false;
    }

    fs::path previous_;
    bool active_ = false;
};

inline void upsert_agent_record_for_plan(const AgentExecutionPlan& plan) {
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = plan.agent_id,
        .agent_type = plan.agent_type,
        .name = plan.name,
        .team_name = plan.team_name,
        .cwd = plan.working_dir,
        .isolation = plan.isolation,
        .mode = plan.mode,
        .background = plan.background,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .transcript_path = default_agent_transcript_path(plan.agent_id),
        .progress = 0.0,
    });
}

[[nodiscard]] inline std::string message_content_text(const Message& message) {
    std::string out;
    for (const auto& block : message.content) {
        if (!out.empty()) out += "\n";
        if (block.type == ContentBlockType::Text || block.type == ContentBlockType::ToolResult) {
            out += block.text;
        } else if (block.type == ContentBlockType::ToolUse) {
            out += std::format("[tool_use:{}]", block.tool_name);
        }
    }
    return out;
}

[[nodiscard]] inline std::expected<AgentExecutionPlan, std::string> build_agent_execution_plan(
    const AgentToolRequest& request,
    const AgentConfig& config
) {
    auto normalized_cwd = normalize_agent_cwd(request.cwd);
    if (!normalized_cwd) return std::unexpected(normalized_cwd.error());

    const auto agents = cc::tools::agent_runtime::get_all_agent_definitions(
        normalized_cwd->has_value() ? std::optional<fs::path>{fs::path{**normalized_cwd}} : std::nullopt);
    auto resolved_type = cc::tools::agent_runtime::resolve_requested_agent_type(request.subagent_type, agents);
    if (!resolved_type) {
        return std::unexpected(std::format(
            "Agent type '{}' not found. Available agents: {}",
            request.subagent_type,
            cc::tools::agent_runtime::format_agent_type_list(agents)));
    }

    std::optional<cc::tools::agent_runtime::AgentDefinition> definition;
    for (const auto& agent : agents) {
        if (agent.agent_type == *resolved_type) {
            definition = agent;
            break;
        }
    }
    if (!definition) {
        return std::unexpected(std::format("Agent type '{}' could not be resolved", request.subagent_type));
    }

    if (auto configured = upsert_agent_inline_mcp_servers(definition->inline_mcp_servers); !configured) {
        return std::unexpected(std::format(
            "Failed to configure MCP servers for agent '{}': {}",
            definition->agent_type,
            configured.error()));
    }

    auto definition_model = resolve_agent_model(definition->model);
    AgentExecutionPlan plan;
    plan.agent_id = next_agent_id(request.name);
    plan.prompt = prepend_initial_prompt(definition->initial_prompt, request.prompt);
    plan.agent_type = definition->agent_type;
    plan.model = request.model.value_or(definition_model.value_or(config.default_model));
    plan.system_prompt = definition->system_prompt.empty()
        ? built_in_system_prompt(definition->agent_type)
        : definition->system_prompt;
    plan.preloaded_skill_messages = load_preloaded_skill_messages(*definition);
    plan.agent_mcp_servers = definition->mcp_servers;
    for (const auto& inline_config : definition->inline_mcp_servers) {
        append_unique_agent_mcp_server(plan.agent_mcp_servers, inline_config.name);
    }
    plan.agent_mcp_tools = connect_agent_mcp_servers(plan.agent_mcp_servers);
    if (!plan.agent_mcp_tools.empty()) {
        plan.agent_mcp_context_message = format_agent_mcp_context_message(plan.agent_mcp_tools);
    }
    if (!definition->required_mcp_servers.empty()) {
        const auto available_servers = available_mcp_servers_with_tools();
        const auto missing = missing_required_mcp_servers(definition->required_mcp_servers, available_servers);
        if (!missing.empty()) {
            return std::unexpected(std::format(
                "Agent '{}' requires MCP servers matching: {}. MCP servers with tools: {}. Use /mcp to configure and authenticate the required MCP servers.",
                definition->agent_type,
                join_fields(missing),
                available_servers.empty() ? "none" : join_fields(available_servers)));
        }
    }
    plan.allowed_tools = definition->tools;
    plan.disallowed_tools = definition->disallowed_tools;
    plan.max_turns = definition->max_turns.value_or(config.max_turns);
    plan.background = request.run_in_background || definition->background;
    plan.name = request.name;
    plan.team_name = request.team_name;
    plan.mode = request.mode;
    plan.isolation = request.isolation.or_else([&] { return definition->isolation; });
    plan.working_dir = *normalized_cwd;
    plan.frontmatter_hooks = definition->hooks;
    const auto runtime_context = format_agent_runtime_context(plan);
    plan.system_prompt = plan.system_prompt.empty()
        ? runtime_context
        : std::format("{}\n\n{}", plan.system_prompt, runtime_context);
    return plan;
}

// =========================================================================
// AgentTool Implementation
// =========================================================================

/// AgentTool - Delegates tasks to sub-agents via recursive API loop
class AgentTool {
public:
    static constexpr std::string_view kName = "Agent";
    static constexpr std::string_view kDescription = 
        "Launch a new agent to handle complex, multi-step tasks autonomously. "
        "The agent runs in a recursive loop, making API calls and executing tools "
        "until it completes the task or reaches the turn limit.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    agent_schema_property("description", "string", "A short description of the task", true),
                    agent_schema_property("prompt", "string", "The task for the agent to perform", true),
                    agent_schema_property("subagent_type", "string", "The specialized agent type to use"),
                    agent_schema_property("model", "string", "Optional model override: sonnet, opus, haiku, or a concrete model id"),
                    agent_schema_property("run_in_background", "boolean", "Run the agent asynchronously"),
                    agent_schema_property("task", "string", "Legacy alias for prompt"),
                    agent_schema_property("skill", "string", "Legacy alias for subagent_type"),
                    agent_schema_property("name", "string", "Teammate name for agent swarms"),
                    agent_schema_property("team_name", "string", "Team name for agent swarms"),
                    agent_schema_property("mode", "string", "Permission mode for spawned teammates"),
                    agent_schema_property("isolation", "string", "Isolation mode for worktree or remote agents"),
                    agent_schema_property("cwd", "string", "Working directory override for the spawned agent")
                }
            },
            .permission = ToolPermission::Execute,
            .category = "agent"
        };
    }
    
    explicit AgentTool(AgentConfig config = {}, int current_depth = 0,
                       cc::core::ToolRegistry* registry = nullptr)
        : config_(config), current_depth_(current_depth), registry_(registry) {}
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        (void)input;
        // Deny if at max recursion depth
        if (current_depth_ >= config_.max_depth) return false;
        return true;
    }
    
    /// Check if a specific tool is allowed for this sub-agent
    [[nodiscard]] bool is_tool_allowed(std::string_view tool_name) const {
        // Check denied list first (takes precedence)
        for (const auto& denied : config_.denied_tools) {
            if (denied == tool_name) return false;
        }
        // If allowed list is specified, tool must be in it
        if (!config_.allowed_tools.empty()) {
            for (const auto& allowed : config_.allowed_tools) {
                if (allowed == tool_name) return true;
            }
            return false;
        }
        return true;  // No restrictions = allow all
    }
    
    /// Get config for creating child agents (depth incremented)
    [[nodiscard]] AgentConfig child_config() const {
        AgentConfig child = config_;
        // Children inherit tool restrictions
        return child;
    }
    
    [[nodiscard]] int current_depth() const { return current_depth_; }
    [[nodiscard]] int max_depth() const { return config_.max_depth; }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto request = parse_agent_tool_request(input);
        if (!request) return ToolResult::error(std::format("Invalid Agent input: {}", request.error()));

        if (request->prompt.empty()) {
            return ToolResult::error("Missing required 'prompt' field");
        }

        auto plan = build_agent_execution_plan(*request, config_);
        if (!plan) return ToolResult::error(plan.error());
        
        // Check recursion depth
        if (current_depth_ >= config_.max_depth) {
            return ToolResult::error(std::format(
                "Agent recursion depth limit reached ({}/{})", 
                current_depth_, config_.max_depth));
        }

        if (plan->background) {
            return start_background_agent(std::move(*plan));
        }
        
        upsert_agent_record_for_plan(*plan);
        // Run the sub-agent loop
        return run_agent_loop(*plan);
    }

private:
    [[nodiscard]] Result<ToolResult> start_background_agent(AgentExecutionPlan plan) {
        cc::tools::MessageRouter::instance().register_agent(plan.agent_id);
        upsert_agent_record_for_plan(plan);

        auto start_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStart");
        if (!start_hooks.ok()) {
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, *start_hooks.error);
            return ToolResult::error(*start_hooks.error);
        }
        if (!start_hooks.output.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "hook SubagentStart: " + start_hooks.output);
        }

        if (!registry_) {
            return ToolResult::success(std::format(
                "Queued background agent {} ({}) but execution is deferred because no tool registry is attached.",
                plan.agent_id,
                plan.agent_type));
        }

        auto config = config_;
        auto depth = current_depth_;
        auto* registry = registry_;
        auto agent_id = plan.agent_id;
        std::thread([plan = std::move(plan), config = std::move(config), depth, registry, agent_id]() mutable {
            cc::tools::agent_runtime::native_agent_store().mark_running(agent_id);
            AgentTool worker(std::move(config), depth, registry);
            auto result = worker.run_agent_loop(plan);
            if (result) {
                std::string output;
                for (const auto& content : result->content) {
                    if (!output.empty()) output += "\n";
                    output += content.text;
                }
                cc::tools::agent_runtime::native_agent_store().mark_completed(agent_id, std::move(output));
            } else {
                cc::tools::agent_runtime::native_agent_store().mark_failed(agent_id, result.error().format());
            }
        }).detach();

        return ToolResult::success(std::format(
            "Started background agent {} ({})",
            plan.agent_id,
            plan.agent_type));
    }

    [[nodiscard]] bool plan_allows_generic_mcp_tool(const AgentExecutionPlan& plan) const {
        return !plan.agent_mcp_tools.empty() &&
            is_tool_allowed("mcp") &&
            !tool_name_disallowed_by_definition("mcp", plan.disallowed_tools);
    }

    [[nodiscard]] bool is_tool_allowed_for_plan(
        std::string_view tool_name,
        const AgentExecutionPlan& plan
    ) const {
        if (tool_name == "mcp" && plan_allows_generic_mcp_tool(plan)) {
            return true;
        }
        return is_tool_allowed(tool_name) &&
            tool_name_allowed_by_definition(tool_name, plan.allowed_tools) &&
            !tool_name_disallowed_by_definition(tool_name, plan.disallowed_tools);
    }

    [[nodiscard]] std::optional<std::string> mcp_scope_error_for_plan(
        const ToolInput& input,
        const AgentExecutionPlan& plan
    ) const {
        if (plan.agent_mcp_servers.empty()) return std::nullopt;

        auto doc = cc::utils::json::parse(input.json());
        if (!doc || !doc->root().is_obj()) {
            return "MCP input must be a JSON object in this sub-agent context";
        }

        auto root = doc->root();
        auto server = json_string(root, "server_name").or_else([&] { return json_string(root, "server"); });
        if (!server || server->empty()) {
            return "MCP server_name is required in this sub-agent context";
        }

        for (const auto& allowed : plan.agent_mcp_servers) {
            if (*server == allowed) return std::nullopt;
        }
        return std::format(
            "MCP server '{}' is not available in this sub-agent context. Available servers: {}",
            *server,
            join_fields(plan.agent_mcp_servers));
    }

    [[nodiscard]] std::vector<cc::services::api::ToolDefinition> api_tools_for_plan(
        const AgentExecutionPlan& plan
    ) const {
        std::vector<cc::services::api::ToolDefinition> tools;
        if (!registry_) return tools;

        for (const auto& definition : registry_->get_visible_definitions()) {
            if (!is_tool_allowed_for_plan(definition.name, plan)) continue;
            tools.push_back(cc::services::api::ToolDefinition{
                .name = definition.name,
                .description = definition.description,
                .input_schema_json = definition.input_schema.to_json(),
                .defer_load = false,
            });
        }
        return tools;
    }

    void append_queued_agent_messages(
        const AgentExecutionPlan& plan,
        std::vector<Message>& messages
    ) const {
        SendMessageTool inbox(plan.agent_id);
        while (auto message = inbox.receive()) {
            messages.push_back(Message::from_text(
                "user",
                std::format(
                    "[Message from {} priority={}]\n{}",
                    message->from_agent,
                    message_priority_name(message->priority),
                    message->content)));
        }
    }

    /// Run the sub-agent's recursive API loop
    [[nodiscard]] Result<ToolResult> run_agent_loop(const AgentExecutionPlan& plan) {
        std::optional<ScopedCurrentPath> cwd_guard;
        if (plan.working_dir) {
            auto entered = ScopedCurrentPath::enter(*plan.working_dir);
            if (!entered) {
                cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, entered.error());
                return ToolResult::error(entered.error());
            }
            cwd_guard.emplace(std::move(*entered));
        }

        auto fail_agent = [&](std::string error) -> Result<ToolResult> {
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(std::move(error));
        };

        cc::tools::agent_runtime::native_agent_store().mark_running(plan.agent_id);
        cc::tools::agent_runtime::native_agent_store().append_transcript(plan.agent_id, "user: " + plan.prompt);
        if (!plan.background) {
            auto start_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStart");
            if (!start_hooks.ok()) return fail_agent(*start_hooks.error);
            if (!start_hooks.output.empty()) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook SubagentStart: " + start_hooks.output);
            }
        }
        
        auto client = get_default_client();
        
        // Build initial messages
        std::vector<Message> messages;
        for (const auto& skill_message : plan.preloaded_skill_messages) {
            messages.push_back(Message::from_text("user", skill_message));
        }
        if (plan.agent_mcp_context_message) {
            messages.push_back(Message::from_text("user", *plan.agent_mcp_context_message));
        }
        messages.push_back(Message::from_text("user", plan.prompt));
        
        std::string final_output;
        
        for (int turn = 0; turn < plan.max_turns; ++turn) {
            if (cc::tools::agent_runtime::native_agent_store().is_cancel_requested(plan.agent_id)) {
                const auto reason = std::format("Agent {} cancelled before turn {}", plan.agent_id, turn + 1);
                cc::tools::agent_runtime::native_agent_store().mark_cancelled(plan.agent_id, reason);
                return ToolResult::error(reason);
            }
            cc::tools::agent_runtime::native_agent_store().update_progress(
                plan.agent_id,
                static_cast<double>(turn) / static_cast<double>(std::max(plan.max_turns, 1)));
            append_queued_agent_messages(plan, messages);

            // Build request
            CreateMessageRequest req;
            req.model = plan.model;
            req.messages = messages;
            req.max_tokens = 16384;
            req.stream = true;
            if (!plan.system_prompt.empty()) req.system_prompt = plan.system_prompt;
            req.tools = api_tools_for_plan(plan);
            
            // Perform streaming request
            auto stream_result = client.create_message_stream(req);
            if (!stream_result) {
                return fail_agent(std::format(
                    "Agent API call failed: {}", stream_result.error().message()));
            }
            
            auto& parser = *stream_result;
            
            // Consume the stream and accumulate content blocks
            std::string text_content;
            std::vector<ContentBlock> tool_uses;
            std::string stop_reason;
            ContentBlock current_block;
            std::string accumulated_json;
            bool in_block = false;
            
            while (true) {
                auto event_result = parser.next_event();
                if (!event_result) break;
                if (!event_result->has_value()) {
                    if (parser.is_finished()) break;
                    // Brief wait for producer
                    continue;
                }
                
                const auto& event = **event_result;
                
                switch (event.type) {
                    case StreamEventType::ContentBlockStart:
                        in_block = true;
                        current_block = ContentBlock{};
                        if (event.block_type == StreamContentBlockType::Text) {
                            current_block.type = ContentBlockType::Text;
                        } else if (event.block_type == StreamContentBlockType::ToolUse) {
                            current_block.type = ContentBlockType::ToolUse;
                            current_block.tool_use_id = event.tool_use_id;
                            current_block.tool_name = event.tool_name;
                            accumulated_json.clear();
                        }
                        break;
                        
                    case StreamEventType::ContentBlockDelta:
                        if (event.delta.type == StreamContentBlockType::Text) {
                            current_block.text += event.delta.text;
                        } else if (event.delta.type == StreamContentBlockType::ToolUse) {
                            accumulated_json += event.delta.partial_json;
                        }
                        break;
                        
                    case StreamEventType::ContentBlockStop:
                        if (in_block) {
                            if (current_block.type == ContentBlockType::Text) {
                                text_content += current_block.text;
                            } else if (current_block.type == ContentBlockType::ToolUse) {
                                current_block.tool_input_json = accumulated_json;
                                tool_uses.push_back(current_block);
                            }
                            in_block = false;
                        }
                        break;
                        
                    case StreamEventType::MessageDelta:
                        if (event.message_delta.stop_reason) {
                            stop_reason = *event.message_delta.stop_reason;
                        }
                        break;
                        
                    case StreamEventType::MessageStop:
                        goto stream_done;
                        
                    case StreamEventType::Error:
                        return fail_agent(std::format(
                            "Agent stream error: {}", event.error.error_message));
                        
                    default:
                        break;
                }
            }
            stream_done:
            
            // If stop_reason is "end_turn" and no tool_use, we're done
            if (stop_reason == "end_turn" || tool_uses.empty()) {
                final_output = text_content;
                if (!text_content.empty()) {
                    cc::tools::agent_runtime::native_agent_store().append_transcript(
                        plan.agent_id,
                        "assistant: " + text_content);
                }
                break;
            }
            
            // Add assistant message to history
            Message assistant_msg;
            assistant_msg.role = "assistant";
            if (!text_content.empty()) {
                assistant_msg.content.push_back(ContentBlock{
                    .type = ContentBlockType::Text,
                    .text = text_content
                });
            }
            for (auto& tu : tool_uses) {
                assistant_msg.content.push_back(tu);
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "assistant: " + message_content_text(assistant_msg));
            messages.push_back(std::move(assistant_msg));
            
            // Execute tools and add results
            Message tool_result_msg;
            tool_result_msg.role = "user";
            for (const auto& tu : tool_uses) {
                ContentBlock result_block;
                result_block.type = ContentBlockType::ToolResult;
                result_block.tool_use_id = tu.tool_use_id;
                
                if (!registry_) {
                    result_block.text = "[Tool execution not available: no registry]";
                } else if (!is_tool_allowed_for_plan(tu.tool_name, plan)) {
                    result_block.text = std::format(
                        "[Tool '{}' not available in sub-agent context]", tu.tool_name);
                } else if (tu.tool_name == "Agent" && current_depth_ + 1 >= config_.max_depth) {
                    result_block.text = std::format(
                        "[Agent recursion depth limit reached ({}/{})]",
                        current_depth_ + 1, config_.max_depth);
                } else {
                    auto tool_input = ToolInput::from_json(tu.tool_input_json);
                    if (tu.tool_name == "mcp") {
                        if (auto scope_error = mcp_scope_error_for_plan(tool_input, plan)) {
                            result_block.text = std::format("[Tool execution error: {}]", *scope_error);
                            tool_result_msg.content.push_back(std::move(result_block));
                            continue;
                        }
                    }
                    if (tu.tool_name == "Agent") {
                        AgentTool child(child_config(), current_depth_ + 1, registry_);
                        auto child_result = child.execute(tool_input);
                        if (child_result) {
                            std::string output;
                            for (const auto& c : child_result->content) {
                                if (!output.empty()) output += "\n";
                                output += c.text;
                            }
                            result_block.text = std::move(output);
                        } else {
                            result_block.text = std::format(
                                "[Tool execution error: {}]", child_result.error().format());
                        }
                    } else {
                        auto exec_result = registry_->execute(tu.tool_name, tool_input);
                        if (exec_result) {
                            // Concatenate all content blocks from the tool result
                            std::string output;
                            for (const auto& c : exec_result->content) {
                                if (!output.empty()) output += "\n";
                                output += c.text;
                            }
                            result_block.text = std::move(output);
                        } else {
                            result_block.text = std::format(
                                "[Tool execution error: {}]", exec_result.error().message);
                        }
                    }
                }
                
                tool_result_msg.content.push_back(std::move(result_block));
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "user: " + message_content_text(tool_result_msg));
            messages.push_back(std::move(tool_result_msg));
            
            // Record partial output
            if (!text_content.empty()) {
                final_output += text_content + "\n";
            }
        }
        
        if (final_output.empty()) {
            final_output = "[Agent completed without producing output]";
        }

        auto stop_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStop", final_output);
        if (!stop_hooks.ok()) return fail_agent(*stop_hooks.error);
        if (!stop_hooks.output.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "hook SubagentStop: " + stop_hooks.output);
        }
        cc::tools::agent_runtime::native_agent_store().mark_completed(plan.agent_id, final_output);
        
        return ToolResult::success(final_output);
    }
    
    AgentConfig config_;
    int current_depth_ = 0;
    cc::core::ToolRegistry* registry_ = nullptr;
};

} // namespace cc::tools::agent

// Export main tool class
export namespace cc::tools {
    using cc::tools::agent::AgentTool;
    using cc::tools::agent::AgentConfig;

    /// Factory: create AgentTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_agent_tool(AgentConfig config = {},
                                        int depth = 0,
                                        cc::core::ToolRegistry* registry = nullptr)
        -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            AgentTool tool_;
            cc::core::ToolDefinition def_ = AgentTool::definition();

            explicit Adapter(AgentConfig cfg, int d, cc::core::ToolRegistry* reg)
                : tool_(std::move(cfg), d, reg) {}

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>(std::move(config), depth, registry);
    }
}
