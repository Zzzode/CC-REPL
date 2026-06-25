/// @file test_fix_hooks_integration.cpp
/// @brief Regression coverage for fix B2: confirm cc.utils.hooks_execution
///        (the user-configured-hook engine) is wired into the QueryEngine
///        tool-call dispatch path. Mirrors the TS wiring where
///        src/services/tools/toolExecution.ts runs executePreToolHooks /
///        executePostToolHooks from src/utils/hooks.ts around each tool call.
///
/// We exercise the wiring through the public testing seam
/// QueryEngine::execute_single_tool_for_testing, which routes through the
/// same execute_single_tool path as the live tool loop.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

import cc.query.query_engine;
import cc.tools.tool;
import cc.utils.hooks_execution;
import cc.utils.hooks_registry;
import cc.hooks.lifecycle_hooks;
import cc.types.types;

namespace fs = std::filesystem;
namespace he = cc::utils::hooks_execution;
namespace hr = cc::utils::hooks_registry;
using cc::core::QueryEngine;
using cc::core::QueryEngineConfig;
using cc::core::ToolDefinition;
using cc::core::ToolRegistry;
using cc::core::ToolUseBlock;
using hr::CommandHookConfig;
using hr::HookEventType;
using hr::HookSource;
using hr::IndividualHookConfig;

namespace {

/// Build an IndividualHookConfig for a shell-command hook.
IndividualHookConfig make_command_hook(HookEventType event,
                                       std::string command,
                                       std::string shell = "bash",
                                       std::optional<std::string> matcher = std::nullopt) {
    CommandHookConfig cfg;
    cfg.command = std::move(command);
    cfg.shell = std::move(shell);
    IndividualHookConfig h;
    h.event = event;
    h.config = cfg;
    h.matcher = std::move(matcher);
    h.source = HookSource::UserSettings;
    return h;
}

/// Minimal QueryEngine fixture with a single "Echo" tool registered. The tool
/// is registered with ReadOnly permission and auto-approval (no permission
/// hook is installed), so execute_single_tool reaches the hook dispatch path.
class HooksIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / "cc-repl-hooks-integration-test";
        fs::remove_all(root_);
        fs::create_directories(root_);

        QueryEngineConfig config;
        config.api_key = "test-key";
        config.base_url = "http://127.0.0.1:1";  // never called; tool runs locally
        config.cwd = root_.string();
        config.retry_policy.max_retries = 0;
        config.tools = {
            ToolDefinition{
                .name = "Echo",
                .description = "Echo back input",
                .input_schema = cc::core::InputSchema{},
                .permission = cc::core::ToolPermission::ReadOnly,
                .is_hidden = false,
                .category = std::nullopt,
                .max_result_size_chars = 100'000,
                .max_result_size_unbounded = false,
            },
        };

        engine_ = std::make_unique<QueryEngine>(std::move(config), registry_);
    }

    void TearDown() override {
        engine_.reset();
        fs::remove_all(root_);
    }

    ToolRegistry registry_;
    std::unique_ptr<QueryEngine> engine_;
    fs::path root_;
};

ToolUseBlock make_echo_call(std::string text) {
    ToolUseBlock blk;
    blk.id.value = "toolu_test";
    blk.name = "Echo";
    blk.input_json = std::string("{\"text\": \"") + text + "\"}";
    return blk;
}

}  // namespace

// ---------------------------------------------------------------------------
// PreToolUse: a hook that exits 2 (CommandHookRunner maps exit-2 to
// BlockToolCall) must deny the tool call. Mirrors TS where exit-2 blocks
// the tool. Without fix B2, the engine would run the tool and succeed.
TEST_F(HooksIntegrationTest, PreToolUseHookBlockingDeniesToolCall) {
    // `false` exits with status 1 on most shells; `exit 2` is the
    // convention a PreToolUse command uses to block. Use a portable form.
    auto hook = make_command_hook(
        HookEventType::PreToolUse,
        "exit 2",
        "bash",
        std::optional<std::string>{"tool.name == \"Echo\""});

    he::HookExecutionContext ctx_template;
    ctx_template.session_id = "test-session";
    engine_->set_user_hooks({hook}, std::move(ctx_template));

    auto result = engine_->execute_single_tool_for_testing(make_echo_call("hi"));
    EXPECT_TRUE(result.is_error);
    EXPECT_FALSE(result.content.empty());

    const auto denials = engine_->get_permission_denials();
    ASSERT_EQ(denials.size(), 1u);
    EXPECT_EQ(denials[0].tool_name, "Echo");
}

// ---------------------------------------------------------------------------
// PreToolUse: when no hooks match (matcher mismatches), the tool runs
// normally — guards that the engine is not spuriously blocking.
TEST_F(HooksIntegrationTest, PreToolUseHookNonMatchingDoesNotBlock) {
    // Matcher targets a different tool, so this hook never fires for "Echo".
    auto hook = make_command_hook(
        HookEventType::PreToolUse,
        "exit 2",
        "bash",
        std::optional<std::string>{"tool.name == \"Write\""});

    he::HookExecutionContext ctx_template;
    engine_->set_user_hooks({hook}, std::move(ctx_template));

    auto result = engine_->execute_single_tool_for_testing(make_echo_call("hi"));
    // The tool is not registered with an implementation in the ToolRegistry
    // (no .execute path), so it returns an error from the registry lookup,
    // but critically it must NOT be a hook-block denial. We assert the
    // permission denials list is empty (the hook did not block).
    const auto denials = engine_->get_permission_denials();
    EXPECT_TRUE(denials.empty());
    // The error (if any) must not be the hook-block message.
    if (result.is_error && !result.content.empty()) {
        const auto* tb = std::get_if<cc::core::TextBlock>(&result.content[0]);
        if (tb) {
            EXPECT_EQ(tb->text.find("user PreToolUse hook blocked the tool call"),
                      std::string::npos);
        }
    }
}

// ---------------------------------------------------------------------------
// Parity: with NO user hooks configured, behavior is unchanged — the engine
// neither calls hooks nor blocks. This is the default state for all callers
// that never call set_user_hooks, so it must keep working exactly as before.
TEST_F(HooksIntegrationTest, NoUserHooksLeavesBehaviorUnchanged) {
    auto result = engine_->execute_single_tool_for_testing(make_echo_call("hi"));
    // Whatever the registry returns, no hook-block denial was recorded.
    const auto denials = engine_->get_permission_denials();
    EXPECT_TRUE(denials.empty());
    // And the result must not be a user-hook block message.
    if (result.is_error && !result.content.empty()) {
        const auto* tb = std::get_if<cc::core::TextBlock>(&result.content[0]);
        if (tb) {
            EXPECT_EQ(tb->text.find("user PreToolUse hook blocked the tool call"),
                      std::string::npos);
            EXPECT_EQ(tb->text.find("user PostToolUse hook blocked the result"),
                      std::string::npos);
        }
    }
}

// ---------------------------------------------------------------------------
// Setter plumbing: set_user_hooks flips the configured flag so the dispatch
// path is entered. We verify the registry is observable (size) via repeated
// blocking calls — the second blocking call must record a second denial,
// proving the hook list is consulted on every tool call, not just once.
TEST_F(HooksIntegrationTest, BlockingHookFiresOnEveryToolCall) {
    auto hook = make_command_hook(
        HookEventType::PreToolUse,
        "exit 2",
        "bash",
        std::nullopt);  // no matcher -> matches everything

    he::HookExecutionContext ctx_template;
    engine_->set_user_hooks({hook}, std::move(ctx_template));

    (void)engine_->execute_single_tool_for_testing(make_echo_call("a"));
    (void)engine_->execute_single_tool_for_testing(make_echo_call("b"));

    const auto denials = engine_->get_permission_denials();
    EXPECT_EQ(denials.size(), 2u);
}
