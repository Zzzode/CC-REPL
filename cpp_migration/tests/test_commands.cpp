/// @file test_commands.cpp
/// @brief Command system smoke tests aligned with current C++ module APIs.

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

import cc.commands.command;
import cc.commands.registry;
import cc.types.types;
import cc.commands.clear;
import cc.commands.config;
import cc.commands.help;
import cc.commands.hooks;
import cc.commands.model;
import cc.commands.rewind;

namespace {

cc::core::CommandContext ctx(std::vector<std::string> args = {}, std::string raw = {}) {
    return cc::core::CommandContext{.args = std::move(args), .raw_input = std::move(raw)};
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
