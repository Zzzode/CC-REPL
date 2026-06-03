/// @file test_tools.cpp
/// @brief Tool registry smoke tests aligned with current C++ modules.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

import cc.tools.mcp;
import cc.tools.agent;
import cc.tools.agent_runtime;
import cc.tools.registry;
import cc.tools.runtime_registry;
import cc.tools.tool;

namespace fs = std::filesystem;

namespace {

struct CurrentPathGuard {
    fs::path previous;

    explicit CurrentPathGuard(const fs::path& next) : previous(fs::current_path()) {
        fs::current_path(next);
    }

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

} // namespace

TEST(ToolRegistry, ListsBuiltInTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    EXPECT_FALSE(names.empty());
}

TEST(ToolRegistry, ContainsExpectedTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    ASSERT_FALSE(names.empty());

    // Check that known tools are present
    bool has_bash = false;
    bool has_lsp = false;
    bool has_skill = false;
    bool has_task_create = false;
    for (const auto& name : names) {
        if (name == "Bash") has_bash = true;
        if (name == "lsp") has_lsp = true;
        if (name == "skill") has_skill = true;
        if (name == "task_create") has_task_create = true;
    }
    EXPECT_TRUE(has_bash);
    EXPECT_TRUE(has_lsp);
    EXPECT_TRUE(has_skill);
    EXPECT_TRUE(has_task_create);
}

TEST(ToolRegistry, CoreRegistryCanBeConstructed) {
    cc::tools::registry::ToolRegistry registry;
    EXPECT_EQ(registry.size(), 0u);  // Empty by default
}

TEST(ToolRegistry, RegistersRuntimeTools) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    EXPECT_GT(registry.size(), 0u);
    EXPECT_TRUE(registry.contains("Bash"));
    EXPECT_TRUE(registry.contains("Read"));
    EXPECT_TRUE(registry.contains("mcp"));
    EXPECT_TRUE(registry.contains("lsp"));
    EXPECT_TRUE(registry.contains("skill"));
    EXPECT_TRUE(registry.contains("task_create"));
}

TEST(Tools, AgentRuntimeLoadsMarkdownDefinitions) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "reviewer.md");
        agent << R"MD(---
name: reviewer
description: Reviews code changes
model: haiku
tools: [Read, Grep, Bash]
disallowedTools: [Bash]
maxTurns: 8
initialPrompt: Inspect only changed files first.
---
You review code changes and report risks.
)MD";
    }

    auto agents = cc::tools::agent_runtime::load_agent_definitions_from_dir(
        root / ".claude" / "agents",
        "projectSettings");

    ASSERT_EQ(agents.size(), 1u);
    EXPECT_EQ(agents.front().agent_type, "reviewer");
    EXPECT_EQ(agents.front().when_to_use, "Reviews code changes");
    EXPECT_EQ(agents.front().model, "haiku");
    ASSERT_EQ(agents.front().tools.size(), 3u);
    EXPECT_EQ(agents.front().tools.front(), "Read");
    ASSERT_EQ(agents.front().disallowed_tools.size(), 1u);
    EXPECT_EQ(agents.front().disallowed_tools.front(), "Bash");
    ASSERT_TRUE(agents.front().max_turns.has_value());
    EXPECT_EQ(*agents.front().max_turns, 8);
    ASSERT_TRUE(agents.front().initial_prompt.has_value());
    EXPECT_EQ(*agents.front().initial_prompt, "Inspect only changed files first.");

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeResolvesLooseAgentTypeInputs) {
    auto agents = cc::tools::agent_runtime::built_in_agent_definitions();

    auto general = cc::tools::agent_runtime::resolve_requested_agent_type("General Purpose", agents);
    ASSERT_TRUE(general.has_value());
    EXPECT_EQ(*general, "general-purpose");

    auto planner = cc::tools::agent_runtime::resolve_requested_agent_type("planner", agents);
    ASSERT_TRUE(planner.has_value());
    EXPECT_EQ(*planner, "Plan");

    auto explorer = cc::tools::agent_runtime::resolve_requested_agent_type("explorer", agents);
    ASSERT_TRUE(explorer.has_value());
    EXPECT_EQ(*explorer, "Explore");

    EXPECT_FALSE(cc::tools::agent_runtime::resolve_requested_agent_type("missing-agent-type", agents).has_value());
}

TEST(Tools, AgentToolAcceptsTypeScriptInputShape) {
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Inspect plan",
      "prompt": "Inspect the migration plan",
      "subagent_type": "planner",
      "model": "haiku"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("Missing required"), std::string::npos);
}

TEST(Tools, AgentToolRejectsUnknownAgentTypesBeforeExecution) {
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Missing agent",
      "prompt": "Use an unknown agent",
      "subagent_type": "cc-repl-missing-agent-type"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent type 'cc-repl-missing-agent-type' not found"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("recursion depth"), std::string::npos);
}

TEST(Tools, AgentToolLoadsProjectAgentDefinitions) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_tool_project_test";
    auto previous_cwd = fs::current_path();
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "project-reviewer.md");
        agent << R"MD(---
name: project-reviewer
description: Reviews project changes
model: haiku
tools: [Read, Grep]
maxTurns: 2
---
Review the project change and report concrete risks.
)MD";
    }

    fs::current_path(root);
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Review changes",
      "prompt": "Review this migration change",
      "subagent_type": "project-reviewer"
    })"));

    fs::current_path(previous_cwd);
    fs::remove_all(root);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("not found"), std::string::npos);
}

TEST(Tools, AgentToolAppliesInitialPromptAndToolRestrictionsInExecutionPlan) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_plan_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "restricted.md");
        agent << R"MD(---
name: restricted-reviewer
description: Reviews with restricted tools
model: haiku
tools: [Read, Bash]
disallowedTools: [Bash]
initialPrompt: First inspect the diff.
maxTurns: 2
---
Review with a narrow tool set.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "restricted-reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        EXPECT_EQ(plan->agent_type, "restricted-reviewer");
        EXPECT_EQ(plan->model, "claude-3-5-haiku-20241022");
        EXPECT_EQ(plan->max_turns, 2);
        EXPECT_NE(plan->prompt.find("First inspect the diff.\n\nReview this change."), std::string::npos);
        ASSERT_EQ(plan->allowed_tools.size(), 2u);
        EXPECT_EQ(plan->allowed_tools.front(), "Read");
        ASSERT_EQ(plan->disallowed_tools.size(), 1u);
        EXPECT_EQ(plan->disallowed_tools.front(), "Bash");
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolPreloadsSkillsFromDefinition) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_skills_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    fs::create_directories(root / ".claude" / "skills" / "review-skill");
    {
        std::ofstream agent(root / ".claude" / "agents" / "skillful.md");
        agent << R"MD(---
name: skillful-reviewer
description: Reviews with preloaded skills
skills: [review-skill, missing-skill]
---
Review with a preloaded workflow.
)MD";
    }
    {
        std::ofstream skill(root / ".claude" / "skills" / "review-skill" / "SKILL.md");
        skill << R"MD(---
description: Review skill
---
Inspect the patch before reporting findings.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "skillful-reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->preloaded_skill_messages.size(), 1u);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("review-skill"), std::string::npos);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("Inspect the patch"), std::string::npos);
        EXPECT_EQ(plan->preloaded_skill_messages.front().find("missing-skill"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolRejectsUnsupportedDefinitionFeatures) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_unsupported_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "async.md");
        agent << R"MD(---
name: async-reviewer
description: Reviews in unsupported native modes
background: true
isolation: worktree
skills: [review]
---
Review asynchronously.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Async review",
          "prompt": "Review this change",
          "subagent_type": "async-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("features not yet supported"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("background"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("isolation"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("skills"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("recursion depth"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolRejectsMissingRequiredMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_required_mcp_missing_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "linear.md");
        agent << R"MD(---
name: linear-reviewer
description: Requires Linear MCP tools
requiredMcpServers: [linear]
---
Review Linear context.
)MD";
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Linear review",
          "prompt": "Review with Linear context",
          "subagent_type": "linear-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("requires MCP servers matching: linear"), std::string::npos);
        EXPECT_NE(result->content.front().text.find("MCP servers with tools: none"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("recursion depth"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolLoadsAgentSpecificMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_mcp_servers_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'agent-mcp-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'lookup', description: 'Lookup agent context', inputSchema: { type: 'object' } }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "mcp-agent.md");
        agent << R"MD(---
name: mcp-agent
description: Uses an agent-specific MCP server
tools: [Read]
mcpServers: [agent_fixture]
---
Use the agent-specific MCP server.
)MD";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "agent_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Use MCP context.";
        request.subagent_type = "mcp-agent";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->agent_mcp_servers.size(), 1u);
        EXPECT_EQ(plan->agent_mcp_servers.front(), "agent_fixture");
        ASSERT_EQ(plan->agent_mcp_tools.size(), 1u);
        EXPECT_EQ(plan->agent_mcp_tools.front().server_name, "agent_fixture");
        EXPECT_EQ(plan->agent_mcp_tools.front().tool_name, "lookup");
        ASSERT_TRUE(plan->agent_mcp_context_message.has_value());
        EXPECT_NE(plan->agent_mcp_context_message->find("agent_fixture/lookup"), std::string::npos);
        ASSERT_EQ(plan->allowed_tools.size(), 1u);
        EXPECT_EQ(plan->allowed_tools.front(), "Read");
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsReadyRequiredMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_required_mcp_ready_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'linear-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'lookup', description: 'Lookup issue', inputSchema: { type: 'object' } }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "linear.md");
        agent << R"MD(---
name: linear-reviewer
description: Requires Linear MCP tools
requiredMcpServers: [linear]
---
Review Linear context.
)MD";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "linear_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());
    auto restarted = cc::tools::restart_native_mcp_server("linear_fixture");
    ASSERT_TRUE(restarted.has_value()) << restarted.error();
    ASSERT_EQ(restarted->status, "ready");
    ASSERT_EQ(restarted->tools.size(), 1u);

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Linear review",
          "prompt": "Review with Linear context",
          "subagent_type": "linear-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("requires MCP servers"), std::string::npos);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolRejectsUnsupportedNativeParameters) {
    cc::tools::AgentTool tool;

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async",
      "prompt": "Run in the background",
      "run_in_background": true,
      "isolation": "worktree"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("run_in_background"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("isolation"), std::string::npos);
}

TEST(Tools, TodoWriteParsesTypeScriptInputShape) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto result = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Inspect migration gaps","status":"in_progress","activeForm":"Inspecting migration gaps"},
        {"content":"Run native validation","status":"pending","activeForm":"Running native validation"}
      ]
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("2 total, 2 added"), std::string::npos);
}

TEST(Tools, TodoWriteClearsAllDoneReplacementLists) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto initial = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Implement parser","status":"in_progress","activeForm":"Implementing parser"},
        {"content":"Verify parser","status":"pending","activeForm":"Verifying parser"}
      ]
    })"));
    ASSERT_TRUE(initial.has_value());
    ASSERT_FALSE(initial->is_error);

    auto completed = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Implement parser","status":"completed","activeForm":"Implementing parser"},
        {"content":"Verify parser","status":"completed","activeForm":"Verifying parser"}
      ]
    })"));

    ASSERT_TRUE(completed.has_value());
    ASSERT_FALSE(completed->is_error);
    ASSERT_FALSE(completed->content.empty());
    EXPECT_NE(completed->content.front().text.find("0 total"), std::string::npos);
}

TEST(Tools, GlobFiltersByPattern) {
    auto root = fs::temp_directory_path() / "cc_repl_glob_test";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    {
        std::ofstream(root / "src" / "match.cpp") << "int main() {}\n";
        std::ofstream(root / "src" / "skip.txt") << "not source\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("Glob", cc::core::ToolInput::from_json(
        std::format(R"({{"pattern":"**/*.cpp","path":"{}"}})", root.string())));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    EXPECT_NE(result->content.front().text.find("match.cpp"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("skip.txt"), std::string::npos);
    fs::remove_all(root);
}

TEST(Tools, GrepUsesPathAndRegex) {
    auto root = fs::temp_directory_path() / "cc_repl_grep_test";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    {
        std::ofstream(root / "src" / "match.cpp") << "alpha_123\nbeta\n";
        std::ofstream(root / "src" / "skip.cpp") << "gamma\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("Grep", cc::core::ToolInput::from_json(
        std::format(R"({{"pattern":"alpha_[0-9]+","path":"{}"}})", (root / "src").string())));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    EXPECT_NE(result->content.front().text.find("match.cpp"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("alpha_123"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("gamma"), std::string::npos);
    fs::remove_all(root);
}

TEST(Tools, McpToolCallsNativeStdioServer) {
    auto root = fs::temp_directory_path() / "cc_repl_mcp_stdio_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {}, resources: {} },
        serverInfo: { name: 'fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'echo', description: 'Echo value', inputSchema: { type: 'object' } }]
      }
    });
    return;
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{ type: 'text', text: 'echo:' + request.params.arguments.value }]
      }
    });
    return;
  }
  if (request.method === 'resources/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: { resources: [{ uri: 'fixture://one', name: 'one', mimeType: 'text/plain' }] }
    });
    return;
  }
  if (request.method === 'resources/read') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: { contents: [{ uri: request.params.uri, mimeType: 'text/plain', text: 'resource-body' }] }
    });
  }
});
)JS";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "echo_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());

    auto restarted = cc::tools::restart_native_mcp_server("echo_fixture");
    ASSERT_TRUE(restarted.has_value()) << restarted.error();
    EXPECT_EQ(restarted->status, "ready");
    ASSERT_EQ(restarted->tools.size(), 1u);
    EXPECT_EQ(restarted->tools.front().name, "echo");

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("mcp", cc::core::ToolInput::from_json(
        R"({"server_name":"echo_fixture","tool_name":"echo","arguments":{"value":"hello"}})"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_EQ(result->content.front().text, "echo:hello");

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}
