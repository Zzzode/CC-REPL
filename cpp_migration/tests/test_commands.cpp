/// @file test_commands.cpp
/// @brief Command system smoke tests aligned with current C++ module APIs.

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

import cc.commands.command;
import cc.commands.registry;
import cc.query.query_engine;
import cc.tools.tool;
import cc.types.types;
import cc.commands.agents;
import cc.commands.clear;
import cc.commands.config;
import cc.commands.help;
import cc.commands.hooks;
import cc.commands.login;
import cc.commands.model;
import cc.commands.rewind;

namespace {

struct EnvironmentGuard {
    std::string name;
    std::optional<std::string> previous;

    EnvironmentGuard(std::string key, const std::string& value) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~EnvironmentGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

struct EnvironmentUnsetGuard {
    std::string name;
    std::optional<std::string> previous;

    explicit EnvironmentUnsetGuard(std::string key) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        unsetenv(name.c_str());
    }

    ~EnvironmentUnsetGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

struct LoginReaderGuard {
    ~LoginReaderGuard() {
        cc::commands::clear_login_api_key_reader_for_testing();
    }
};

cc::core::CommandContext ctx(std::vector<std::string> args = {}, std::string raw = {}) {
    return cc::core::CommandContext{.args = std::move(args), .raw_input = std::move(raw)};
}

std::vector<cc::core::Message> compact_runtime_messages(void* state) {
    auto* engine = static_cast<cc::core::QueryEngine*>(state);
    return engine ? engine->get_conversation() : std::vector<cc::core::Message>{};
}

cc::core::VoidResult compact_runtime_apply(void* state) {
    auto* engine = static_cast<cc::core::QueryEngine*>(state);
    if (!engine) {
        return std::unexpected(cc::core::Error::make(
            cc::core::ErrorCode::InternalError,
            "No active query engine is available for compaction"));
    }
    auto compacted = engine->compact_conversation();
    if (!compacted) {
        return std::unexpected(cc::core::Error::make(
            cc::core::ErrorCode::InternalError,
            compacted.error().format()));
    }
    return cc::core::VoidResult{};
}

} // namespace

TEST(CommandRegistry, ParsesSlashCommandsAndArguments) {
    auto parsed = cc::core::CommandRegistry::parse("/config get model.default_model");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->name, "config");
    ASSERT_EQ(parsed->args.size(), 2u);
    EXPECT_EQ(parsed->args[0], "get");
    EXPECT_EQ(parsed->args[1], "model.default_model");
    EXPECT_EQ(parsed->raw, "/config get model.default_model");

    EXPECT_FALSE(cc::core::CommandRegistry::parse("not a command").has_value());
}

TEST(CommandRegistry, ExecutesLegacyCommandsAndAliases) {
    cc::core::CommandRegistry registry;
    registry.register_command(cc::core::CommandRegistration{
        .name = "echo",
        .description = "Echo input",
        .usage = "/echo <text>",
        .handler = [](const cc::core::CommandContext& command_ctx) {
            return cc::core::CommandResult::success(command_ctx.args.empty() ? "" : command_ctx.args.front());
        },
        .aliases = {"say"},
        .hidden = false,
    });

    EXPECT_TRUE(registry.contains("echo"));
    EXPECT_TRUE(registry.contains("say"));

    auto result = registry.execute("/say hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok);
    EXPECT_EQ(result->message, "hello");

    auto missing = registry.execute("/missing");
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->ok);
}

TEST(CommandRegistry, RegistersTypedCommandsAndCompletesNames) {
    cc::core::CommandRegistry registry;
    registry.register_command<cc::commands::HelpCommand>();
    registry.register_command<cc::commands::ClearCommand>();

    EXPECT_EQ(registry.size(), 2u);
    EXPECT_NE(registry.get("help"), nullptr);
    EXPECT_NE(registry.get("h"), nullptr);

    auto completions = registry.complete("/he");
    ASSERT_FALSE(completions.empty());
    EXPECT_EQ(completions.front(), "/help");

    auto executed = registry.execute("/h --shortcuts");
    ASSERT_TRUE(executed.has_value());
    EXPECT_TRUE(executed->ok);
    EXPECT_NE(executed->message.find("Ctrl+C"), std::string::npos);
}

TEST(AppCommandRegistry, ReportsCommandPermissionLevels) {
    EXPECT_EQ(cc::commands::command_permission("help"), cc::commands::CommandPermission::ReadOnly);
    EXPECT_EQ(cc::commands::command_permission("clear"), cc::commands::CommandPermission::ReadWrite);
    EXPECT_EQ(cc::commands::command_permission("unknown"), cc::commands::CommandPermission::None);
}

TEST(AppCommandRegistry, DispatchesMigratedRuntimeCommands) {
    cc::commands::AppCommandRegistry registry;

    EXPECT_GT(registry.command_count(), 0u);
    EXPECT_TRUE(registry.has_command("commit"));
    EXPECT_TRUE(registry.has_command("mcp"));
    EXPECT_TRUE(registry.has_command("ant-trace"));
    EXPECT_TRUE(registry.has_command("version"));
    EXPECT_TRUE(registry.has_command("exit"));

    auto help = registry.execute("/help", ctx());
    ASSERT_TRUE(help.has_value());
    EXPECT_TRUE(help->ok);
    EXPECT_NE(help->message.find("/commit"), std::string::npos);
    EXPECT_NE(help->message.find("/mcp"), std::string::npos);

    auto mcp = registry.execute("/mcp list", ctx());
    ASSERT_TRUE(mcp.has_value());
    EXPECT_TRUE(mcp->ok);
    EXPECT_EQ(mcp->message.find("Unknown command"), std::string::npos);

    auto ant_trace = registry.execute("/ant-trace request-42", ctx());
    ASSERT_TRUE(ant_trace.has_value());
    EXPECT_TRUE(ant_trace->ok);
    EXPECT_NE(ant_trace->message.find("ANT trace snapshot"), std::string::npos);
    EXPECT_EQ(ant_trace->message.find("No dedicated local action"), std::string::npos);

    auto version = registry.execute("/version detail", ctx());
    ASSERT_TRUE(version.has_value());
    EXPECT_TRUE(version->ok);
    EXPECT_NE(version->message.find("cc-repl 1.0.0-cpp"), std::string::npos);
    EXPECT_EQ(version->message.find("No dedicated local action"), std::string::npos);

    auto exit = registry.execute("/exit", ctx());
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->metadata, "EXIT");
}

TEST(AppCommandRegistry, RuntimeSurfaceCommandsExecuteLocalLogic) {
    cc::commands::AppCommandRegistry registry;

    auto debug = registry.execute(R"(/debug-tool-call {"name":"Bash","input":{"command":"pwd"}})", ctx());
    ASSERT_TRUE(debug.has_value());
    EXPECT_TRUE(debug->ok);
    EXPECT_NE(debug->message.find("Tool-call payload valid"), std::string::npos);
    EXPECT_NE(debug->message.find("Tool: Bash"), std::string::npos);

    auto mock = registry.execute("/mock-limits 5", ctx());
    ASSERT_TRUE(mock.has_value());
    EXPECT_TRUE(mock->ok);
    EXPECT_NE(mock->message.find("Synthetic rate limit active"), std::string::npos);

    auto reset = registry.execute("/reset-limits all", ctx());
    ASSERT_TRUE(reset.has_value());
    EXPECT_TRUE(reset->ok);
    EXPECT_NE(reset->message.find("active=false"), std::string::npos);
    EXPECT_NE(reset->message.find("total_retries=0"), std::string::npos);

    auto extra_usage = registry.execute("/extra-usage status", ctx());
    ASSERT_TRUE(extra_usage.has_value());
    EXPECT_TRUE(extra_usage->ok);
    EXPECT_NE(extra_usage->message.find("Extra usage:"), std::string::npos);

    auto bridge = registry.execute("/bridge status", ctx());
    ASSERT_TRUE(bridge.has_value());
    EXPECT_TRUE(bridge->ok);
    EXPECT_NE(bridge->message.find("Bridge status:"), std::string::npos);

    auto onboarding = registry.execute("/onboarding status", ctx());
    ASSERT_TRUE(onboarding.has_value());
    EXPECT_TRUE(onboarding->ok);
    EXPECT_NE(onboarding->message.find("Onboarding status"), std::string::npos);
}

TEST(LoginCommand, ApiKeyFlowReadsInteractiveSecretWhenEnvIsMissing) {
    auto root = std::filesystem::temp_directory_path() / "cc_repl_login_apikey_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    EnvironmentUnsetGuard api_key_guard("ANTHROPIC_API_KEY");
    EnvironmentUnsetGuard xdg_guard("XDG_CONFIG_HOME");
    EnvironmentGuard home_guard("HOME", root.string());
    LoginReaderGuard reader_guard;
    cc::commands::set_login_api_key_reader_for_testing([] {
        return std::expected<std::string, std::string>{"sk-test-interactive"};
    });

    cc::commands::LoginCommand login;
    auto result = login.execute(ctx({"apikey"}));
    ASSERT_TRUE(result.has_value()) << result.error().format();
    EXPECT_TRUE(result->ok);
    EXPECT_EQ(result->message, "Authenticated with API key.");

    auto status = login.execute(ctx({"status"}));
    ASSERT_TRUE(status.has_value()) << status.error().format();
    EXPECT_NE(status->message.find("Method:  API Key"), std::string::npos);

    const auto credentials_path = root / ".config" / "cc-repl" / "credentials.json";
    std::ifstream input(credentials_path);
    ASSERT_TRUE(input.is_open()) << credentials_path;
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_NE(body.find(R"("type":"api_key")"), std::string::npos);
    EXPECT_NE(body.find(R"("api_key":"sk-test-interactive")"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(LoginCommand, ApiKeyFlowRejectsInvalidInteractiveSecret) {
    auto root = std::filesystem::temp_directory_path() / "cc_repl_login_invalid_apikey_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    EnvironmentUnsetGuard api_key_guard("ANTHROPIC_API_KEY");
    EnvironmentUnsetGuard xdg_guard("XDG_CONFIG_HOME");
    EnvironmentGuard home_guard("HOME", root.string());
    LoginReaderGuard reader_guard;
    cc::commands::set_login_api_key_reader_for_testing([] {
        return std::expected<std::string, std::string>{"not-a-key"};
    });

    cc::commands::LoginCommand login;
    auto result = login.execute(ctx({"apikey"}));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("API key does not look like"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / ".config" / "cc-repl" / "credentials.json"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(AgentsCommand, ListsRealAgentDefinitions) {
    EnvironmentGuard explore_enabled("CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS", "1");
    cc::commands::AgentsCommand agents;

    auto list = agents.execute(ctx({"list"}));
    ASSERT_TRUE(list.has_value());
    EXPECT_TRUE(list->ok);
    EXPECT_NE(list->message.find("Available agents:"), std::string::npos);
    EXPECT_NE(list->message.find("general-purpose"), std::string::npos);
    EXPECT_NE(list->message.find("Explore"), std::string::npos);
    EXPECT_EQ(list->message.find("default, fast, expert"), std::string::npos);

    auto details = agents.execute(ctx({"configure", "general-purpose"}));
    ASSERT_TRUE(details.has_value());
    EXPECT_TRUE(details->ok);
    EXPECT_NE(details->message.find("Agent: general-purpose"), std::string::npos);

    auto unknown = agents.execute(ctx({"use", "missing-agent"}));
    ASSERT_TRUE(unknown.has_value());
    EXPECT_FALSE(unknown->ok);
    EXPECT_NE(unknown->message.find("Unknown agent"), std::string::npos);
}

TEST(HelpCommand, FormatsDefaultShortcutsExamplesAndSpecificHelp) {
    auto help_def = cc::commands::HelpCommand::definition();
    cc::commands::HelpCommand help;
    help.set_command_definitions({&help_def});

    auto all = help.execute(ctx());
    ASSERT_TRUE(all.has_value());
    EXPECT_NE(all->message.find("Available Commands"), std::string::npos);

    auto shortcuts = help.execute(ctx({"--shortcuts"}));
    ASSERT_TRUE(shortcuts.has_value());
    EXPECT_NE(shortcuts->message.find("Ctrl+C"), std::string::npos);

    auto examples = help.execute(ctx({"--examples"}));
    ASSERT_TRUE(examples.has_value());
    EXPECT_NE(examples->message.find("Examples"), std::string::npos);

    auto detailed = help.execute(ctx({"help"}));
    ASSERT_TRUE(detailed.has_value());
    EXPECT_NE(detailed->message.find("/help"), std::string::npos);
}

TEST(ClearCommand, InvokesCallbacksForAllScope) {
    cc::commands::ClearCommand clear;
    bool screen_cleared = false;
    bool conversation_reset = false;
    clear.set_screen_clear_fn([&] { screen_cleared = true; });
    clear.set_conversation_reset_fn([&]() -> cc::core::VoidResult {
        conversation_reset = true;
        return {};
    });

    auto result = clear.execute(ctx({"--all"}));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok);
    EXPECT_TRUE(screen_cleared);
    EXPECT_TRUE(conversation_reset);
    EXPECT_NE(result->message.find("Screen cleared"), std::string::npos);
    EXPECT_NE(result->message.find("Conversation reset"), std::string::npos);
}

TEST(CompactCommand, RuntimeContextCompactsActiveQueryEngineConversation) {
    cc::core::ToolRegistry tool_registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = std::filesystem::current_path().string();
    cc::core::QueryEngine engine(std::move(config), tool_registry);

    auto make_user = [](std::string text, int index) {
        cc::core::UserMessage msg{};
        msg.id.value = "compact-user-" + std::to_string(index);
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };

    for (int i = 0; i < 10; ++i) {
        engine.append_message_for_testing(make_user(
            "retain compact command runtime detail " + std::to_string(i) + " " +
            std::string(400, static_cast<char>('a' + (i % 20))),
            i));
    }

    auto before = engine.get_conversation();
    ASSERT_GT(before.size(), 8u);

    cc::commands::AppCommandRegistry registry;
    auto command_ctx = ctx();
    command_ctx.runtime_state = &engine;
    command_ctx.compact_message_provider = compact_runtime_messages;
    command_ctx.compact_applier = compact_runtime_apply;
    auto result = registry.execute("/compact --target 20", command_ctx);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok);
    EXPECT_EQ(result->status, cc::core::CommandStatus::Succeeded);
    EXPECT_NE(result->message.find("Compaction complete"), std::string::npos);
    EXPECT_EQ(result->message.find("Summarize the following conversation segments"), std::string::npos);

    auto after = engine.get_conversation();
    EXPECT_LT(after.size(), before.size());
    ASSERT_GT(after.size(), 1u);
    bool found_summary_marker = false;
    for (const auto& message : after) {
        const auto* marker = std::get_if<cc::core::UserMessage>(&message);
        if (!marker || marker->content.empty()) continue;
        const auto* text = std::get_if<cc::core::TextBlock>(&marker->content.front());
        if (text && text->text.find("Preserve these details") != std::string::npos) {
            found_summary_marker = true;
            break;
        }
    }
    EXPECT_TRUE(found_summary_marker);
}

TEST(ConfigCommand, ValidatesRequiredArgumentsAndListsConfig) {
    cc::commands::ConfigCommand config;

    EXPECT_TRUE(config.validate(ctx({"list"})).has_value());
    EXPECT_FALSE(config.validate(ctx({"get"})).has_value());
    EXPECT_FALSE(config.validate(ctx({"set", "model.default_model"})).has_value());

    auto list = config.execute(ctx({"list"}));
    ASSERT_TRUE(list.has_value());
    EXPECT_NE(list->message.find("Current Configuration"), std::string::npos);

    auto completions = config.complete("pa");
    ASSERT_FALSE(completions.empty());
    EXPECT_EQ(completions.front(), "path");
}

TEST(ModelCommand, ListsSwitchesAndCompletesModels) {
    cc::commands::ModelCommand model;

    auto list = model.execute(ctx({"list"}));
    ASSERT_TRUE(list.has_value());
    EXPECT_NE(list->message.find("claude-sonnet-4-20250514"), std::string::npos);

    auto switched = model.execute(ctx({"set", "claude-opus-4-20250514"}));
    ASSERT_TRUE(switched.has_value());
    EXPECT_NE(switched->message.find("claude-opus-4-20250514"), std::string::npos);

    auto invalid = model.validate(ctx({"not-a-model"}));
    EXPECT_FALSE(invalid.has_value());

    auto completions = model.complete("claude-");
    EXPECT_GE(completions.size(), 3u);
}

TEST(MigratedCommandMetadata, HooksAndRewindExposeTypeScriptCompatibleMetadata) {
    auto hooks_def = cc::commands::HooksCommand::definition();
    auto rewind_def = cc::commands::RewindCommand::definition();

    EXPECT_EQ(hooks_def.name, "hooks");
    EXPECT_EQ(hooks_def.description, "View hook configurations for tool events");
    EXPECT_FALSE(hooks_def.hidden);

    ASSERT_EQ(rewind_def.aliases.size(), 1u);
    EXPECT_EQ(rewind_def.aliases.front(), "checkpoint");
}

TEST(MigratedCommands, ExecuteActionableMessagesAndCompletions) {
    cc::commands::HooksCommand hooks;
    auto hooks_result = hooks.execute(ctx({"list"}));
    ASSERT_TRUE(hooks_result.has_value());
    EXPECT_NE(hooks_result->message.find("PreToolUse"), std::string::npos);

    cc::commands::RewindCommand rewind;
    auto rewind_result = rewind.execute(ctx({"code"}));
    ASSERT_TRUE(rewind_result.has_value());
    EXPECT_NE(rewind_result->message.find("code"), std::string::npos);

    auto rewind_completions = rewind.complete("c");
    ASSERT_EQ(rewind_completions.size(), 2u);
    EXPECT_EQ(rewind_completions.front(), "conversation");
}
