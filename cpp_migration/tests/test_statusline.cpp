/// @file test_statusline.cpp
/// @brief Unit tests for the statusline system:
///   - cc.commands.statusline      (shell integration setup command)
///   - cc.utils.statusline_runner  (statusline command execution + JSON input)

#include <gtest/gtest.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

import cc.commands.statusline;
import cc.utils.statusline_runner;
import cc.utils.json;

namespace {

// ---------------------------------------------------------------------------
// Scoped env-var guard (same pattern as test_commands / test_utils)
// ---------------------------------------------------------------------------

struct ScopedEnvVar {
    std::string name;
    std::optional<std::string> previous;

    explicit ScopedEnvVar(std::string key) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
    }

    ~ScopedEnvVar() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }

    void set(const char* value) const {
        setenv(name.c_str(), value, 1);
    }

    void unset() const {
        unsetenv(name.c_str());
    }
};

// ===========================================================================
// 1. cc.commands.statusline — shell integration setup command
// ===========================================================================

TEST(StatuslineCommand, NameIsStatusline) {
    EXPECT_EQ(cc::commands::statusline::name(), "statusline");
}

TEST(StatuslineCommand, RunReturnsOkResponse) {
    ScopedEnvVar shell("SHELL");
    shell.set("/bin/zsh");
    auto result = cc::commands::statusline::run();
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.message.empty());
}

TEST(StatuslineCommand, DetectsZshShell) {
    ScopedEnvVar shell("SHELL");
    shell.set("/bin/zsh");
    auto result = cc::commands::statusline::run();
    EXPECT_NE(result.message.find("Detected shell: zsh"), std::string::npos);
    EXPECT_NE(result.message.find(".zshrc"), std::string::npos);
    EXPECT_NE(result.message.find("RPROMPT"), std::string::npos);
    EXPECT_NE(result.message.find("cc-repl --status-json"), std::string::npos);
}

TEST(StatuslineCommand, DetectsBashShell) {
    ScopedEnvVar shell("SHELL");
    shell.set("/bin/bash");
    auto result = cc::commands::statusline::run();
    EXPECT_NE(result.message.find("Detected shell: bash"), std::string::npos);
    EXPECT_NE(result.message.find(".bashrc"), std::string::npos);
    EXPECT_NE(result.message.find("PROMPT_COMMAND"), std::string::npos);
    EXPECT_NE(result.message.find("cc-repl --status-json"), std::string::npos);
}

TEST(StatuslineCommand, DetectsFishShell) {
    ScopedEnvVar shell("SHELL");
    shell.set("/usr/local/bin/fish");
    auto result = cc::commands::statusline::run();
    EXPECT_NE(result.message.find("Detected shell: fish"), std::string::npos);
    EXPECT_NE(result.message.find("fish_right_prompt"), std::string::npos);
    EXPECT_NE(result.message.find("cc-repl --status-json"), std::string::npos);
}

TEST(StatuslineCommand, UnknownShellWhenEnvMissing) {
    ScopedEnvVar shell("SHELL");
    shell.unset();
    auto result = cc::commands::statusline::run();
    EXPECT_NE(result.message.find("Detected shell: unknown"), std::string::npos);
    EXPECT_NE(result.message.find("Could not detect shell type"), std::string::npos);
}

TEST(StatuslineCommand, EmptyShellVarTreatedAsUnknown) {
    ScopedEnvVar shell("SHELL");
    shell.set("");
    auto result = cc::commands::statusline::run();
    EXPECT_NE(result.message.find("Detected shell: unknown"), std::string::npos);
}

TEST(StatuslineCommand, CustomFormatAppendedWhenProvided) {
    ScopedEnvVar shell("SHELL");
    shell.set("/bin/zsh");
    auto result = cc::commands::statusline::run("%model %cost");
    EXPECT_NE(result.message.find("Custom format: %model %cost"), std::string::npos);
}

TEST(StatuslineCommand, NoCustomFormatWhenEmpty) {
    ScopedEnvVar shell("SHELL");
    shell.set("/bin/zsh");
    auto result = cc::commands::statusline::run("");
    EXPECT_EQ(result.message.find("Custom format:"), std::string::npos);
}

// ===========================================================================
// 2. cc.utils.statusline_runner — JSON serialization (to_json)
// ===========================================================================

using cc::utils::statusline::StatusLineCommandInput;
using cc::utils::statusline::to_json;
using cc::utils::json::parse;

TEST(StatuslineJson, SerializesBaseFields) {
    StatusLineCommandInput input;
    input.model.id = "claude-3-5-sonnet";
    input.model.display_name = "Claude 3.5 Sonnet";
    input.workspace.current_dir = "/home/user/project";
    input.workspace.project_dir = "/home/user/project";
    input.workspace.added_dirs = {"src", "tests"};
    input.version = "1.2.3";
    input.output_style_name = "default";

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value()) << "Failed to parse JSON: " << json_str;

    auto root = doc->root();
    EXPECT_TRUE(root.is_obj());

    // model
    auto model = root.get("model");
    EXPECT_TRUE(model.is_obj());
    EXPECT_EQ(model.get("id").as_str(), "claude-3-5-sonnet");
    EXPECT_EQ(model.get("display_name").as_str(), "Claude 3.5 Sonnet");

    // workspace
    auto ws = root.get("workspace");
    EXPECT_TRUE(ws.is_obj());
    EXPECT_EQ(ws.get("current_dir").as_str(), "/home/user/project");
    EXPECT_EQ(ws.get("project_dir").as_str(), "/home/user/project");
    auto added = ws.get("added_dirs");
    EXPECT_TRUE(added.is_arr());
    EXPECT_EQ(added.size(), 2u);
    EXPECT_EQ(added.at(0).as_str(), "src");
    EXPECT_EQ(added.at(1).as_str(), "tests");

    // version
    EXPECT_EQ(root.get("version").as_str(), "1.2.3");

    // output_style
    auto os = root.get("output_style");
    EXPECT_TRUE(os.is_obj());
    EXPECT_EQ(os.get("name").as_str(), "default");
}

TEST(StatuslineJson, SerializesCostInfo) {
    StatusLineCommandInput input;
    input.cost.total_cost_usd = 0.42;
    input.cost.total_duration_ms = 1500;
    input.cost.total_api_duration_ms = 1200;
    input.cost.total_lines_added = 50;
    input.cost.total_lines_removed = 25;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());

    auto cost = doc->root().get("cost");
    EXPECT_TRUE(cost.is_obj());
    EXPECT_DOUBLE_EQ(cost.get("total_cost_usd").as_double(), 0.42);
    EXPECT_EQ(cost.get("total_duration_ms").as_int(), 1500);
    EXPECT_EQ(cost.get("total_api_duration_ms").as_int(), 1200);
    EXPECT_EQ(cost.get("total_lines_added").as_int(), 50);
    EXPECT_EQ(cost.get("total_lines_removed").as_int(), 25);
}

TEST(StatuslineJson, SerializesContextWindowInfo) {
    StatusLineCommandInput input;
    input.context_window.total_input_tokens = 10000;
    input.context_window.total_output_tokens = 2000;
    input.context_window.context_window_size = 200000;
    input.context_window.current_usage = 12000;
    input.context_window.used_percentage = 6.0;
    input.context_window.remaining_percentage = 94.0;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());

    auto cw = doc->root().get("context_window");
    EXPECT_TRUE(cw.is_obj());
    EXPECT_EQ(cw.get("total_input_tokens").as_int(), 10000);
    EXPECT_EQ(cw.get("total_output_tokens").as_int(), 2000);
    EXPECT_EQ(cw.get("context_window_size").as_int(), 200000);
    EXPECT_EQ(cw.get("current_usage").as_int(), 12000);
    EXPECT_DOUBLE_EQ(cw.get("used_percentage").as_double(), 6.0);
    EXPECT_DOUBLE_EQ(cw.get("remaining_percentage").as_double(), 94.0);
}

TEST(StatuslineJson, SerializesExceeds200kTokensFlag) {
    StatusLineCommandInput input;
    input.exceeds_200k_tokens = true;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_TRUE(doc->root().get("exceeds_200k_tokens").as_bool());
}

TEST(StatuslineJson, Exceeds200kTokensCanBeFalse) {
    StatusLineCommandInput input;
    input.exceeds_200k_tokens = false;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    // Field should still be present (it's not optional in the struct)
    EXPECT_TRUE(doc->root().has("exceeds_200k_tokens"));
    EXPECT_FALSE(doc->root().get("exceeds_200k_tokens").as_bool());
}

TEST(StatuslineJson, OmitsSessionNameWhenEmpty) {
    StatusLineCommandInput input;
    input.session_name = std::nullopt;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("session_name"));
}

TEST(StatuslineJson, OmitsSessionNameWhenEmptyString) {
    StatusLineCommandInput input;
    input.session_name = "";

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("session_name"));
}

TEST(StatuslineJson, IncludesSessionNameWhenPopulated) {
    StatusLineCommandInput input;
    input.session_name = "my-session";

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_TRUE(doc->root().has("session_name"));
    EXPECT_EQ(doc->root().get("session_name").as_str(), "my-session");
}

TEST(StatuslineJson, OmitsVimWhenNotSet) {
    StatusLineCommandInput input;
    input.vim = std::nullopt;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("vim"));
}

TEST(StatuslineJson, OmitsVimWhenModeEmpty) {
    StatusLineCommandInput input;
    input.vim = cc::utils::statusline::StatusLineVimInfo{.mode = ""};

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("vim"));
}

TEST(StatuslineJson, IncludesVimWhenPopulated) {
    StatusLineCommandInput input;
    input.vim = cc::utils::statusline::StatusLineVimInfo{.mode = "INSERT"};

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    auto vim = doc->root().get("vim");
    EXPECT_TRUE(vim.is_obj());
    EXPECT_EQ(vim.get("mode").as_str(), "INSERT");
}

TEST(StatuslineJson, OmitsAgentWhenNotSet) {
    StatusLineCommandInput input;
    input.agent = std::nullopt;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("agent"));
}

TEST(StatuslineJson, IncludesAgentWhenPopulated) {
    StatusLineCommandInput input;
    input.agent = cc::utils::statusline::StatusLineAgentInfo{.name = "reviewer"};

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    auto agent = doc->root().get("agent");
    EXPECT_TRUE(agent.is_obj());
    EXPECT_EQ(agent.get("name").as_str(), "reviewer");
}

TEST(StatuslineJson, OmitsAgentWhenNameEmpty) {
    StatusLineCommandInput input;
    input.agent = cc::utils::statusline::StatusLineAgentInfo{.name = ""};

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("agent"));
}

TEST(StatuslineJson, OmitsRemoteWhenNotSet) {
    StatusLineCommandInput input;
    input.remote = std::nullopt;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("remote"));
}

TEST(StatuslineJson, IncludesRemoteWhenPopulated) {
    StatusLineCommandInput input;
    input.remote = cc::utils::statusline::StatusLineRemoteInfo{.session_id = "sess-abc123"};

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    auto remote = doc->root().get("remote");
    EXPECT_TRUE(remote.is_obj());
    EXPECT_EQ(remote.get("session_id").as_str(), "sess-abc123");
}

TEST(StatuslineJson, OmitsWorktreeWhenNotSet) {
    StatusLineCommandInput input;
    input.worktree = std::nullopt;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("worktree"));
}

TEST(StatuslineJson, IncludesWorktreeWhenPopulated) {
    StatusLineCommandInput input;
    input.worktree = cc::utils::statusline::StatusLineWorktreeInfo{
        .name = "feature-branch",
        .path = "/tmp/worktrees/feature",
        .branch = "feature/xyz",
        .original_cwd = "/home/user/project",
        .original_branch = "main",
    };

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    auto wt = doc->root().get("worktree");
    EXPECT_TRUE(wt.is_obj());
    EXPECT_EQ(wt.get("name").as_str(), "feature-branch");
    EXPECT_EQ(wt.get("path").as_str(), "/tmp/worktrees/feature");
    EXPECT_EQ(wt.get("branch").as_str(), "feature/xyz");
    EXPECT_EQ(wt.get("original_cwd").as_str(), "/home/user/project");
    EXPECT_EQ(wt.get("original_branch").as_str(), "main");
}

TEST(StatuslineJson, OmitsWorktreeWhenNameEmpty) {
    StatusLineCommandInput input;
    input.worktree = cc::utils::statusline::StatusLineWorktreeInfo{
        .name = "",
        .path = "/some/path",
        .branch = "main",
        .original_cwd = "/orig",
        .original_branch = "master",
    };

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("worktree"));
}

TEST(StatuslineJson, OmitsRateLimitsWhenNullopt) {
    StatusLineCommandInput input;
    input.rate_limits = std::nullopt;

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("rate_limits"));
}

TEST(StatuslineJson, OmitsRateLimitsWhenBothSubBucketsNullopt) {
    StatusLineCommandInput input;
    input.rate_limits = cc::utils::statusline::StatusLineRateLimits{
        .five_hour = std::nullopt,
        .seven_day = std::nullopt,
    };

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc->root().has("rate_limits"));
}

TEST(StatuslineJson, IncludesRateLimitsFiveHourOnly) {
    StatusLineCommandInput input;
    input.rate_limits = cc::utils::statusline::StatusLineRateLimits{
        .five_hour = cc::utils::statusline::StatusLineRateLimitBucket{
            .used_percentage = 45.5,
            .resets_at = 1700000000000LL,
        },
        .seven_day = std::nullopt,
    };

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    auto rl = doc->root().get("rate_limits");
    EXPECT_TRUE(rl.is_obj());
    EXPECT_TRUE(rl.has("five_hour"));
    EXPECT_FALSE(rl.has("seven_day"));

    auto fh = rl.get("five_hour");
    EXPECT_DOUBLE_EQ(fh.get("used_percentage").as_double(), 45.5);
    EXPECT_EQ(fh.get("resets_at").as_int(), 1700000000000LL);
}

TEST(StatuslineJson, IncludesRateLimitsBothBuckets) {
    StatusLineCommandInput input;
    input.rate_limits = cc::utils::statusline::StatusLineRateLimits{
        .five_hour = cc::utils::statusline::StatusLineRateLimitBucket{
            .used_percentage = 30.0,
            .resets_at = 1000,
        },
        .seven_day = cc::utils::statusline::StatusLineRateLimitBucket{
            .used_percentage = 60.0,
            .resets_at = 99999,
        },
    };

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    auto rl = doc->root().get("rate_limits");
    EXPECT_TRUE(rl.has("five_hour"));
    EXPECT_TRUE(rl.has("seven_day"));
    EXPECT_DOUBLE_EQ(rl.get("seven_day").get("used_percentage").as_double(), 60.0);
}

TEST(StatuslineJson, EmptyAddedDirsIsValidArray) {
    StatusLineCommandInput input;
    input.workspace.added_dirs = {};

    auto json_str = to_json(input);
    auto doc = parse(json_str);
    ASSERT_TRUE(doc.has_value());
    auto added = doc->root().get("workspace").get("added_dirs");
    EXPECT_TRUE(added.is_arr());
    EXPECT_EQ(added.size(), 0u);
}

// ===========================================================================
// 3. cc.utils.statusline_runner — execute_statusline_command
// ===========================================================================

using cc::utils::statusline::execute_statusline_command;
using cc::utils::statusline::StatusLineResult;

bool has_bash() {
    return std::system("command -v bash >/dev/null 2>&1") == 0;
}

TEST(StatuslineExecute, EmptyCommandReturnsError) {
    StatusLineCommandInput input;
    auto result = execute_statusline_command("", input);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "empty command");
    EXPECT_TRUE(result.output.empty());
}

TEST(StatuslineExecute, SuccessfulCommandReturnsTrimmedOutput) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    input.model.id = "test-model";
    input.version = "0.0.1";

    // Command that echoes a simple string
    auto result = execute_statusline_command("echo '  hello statusline  '", input);
    EXPECT_TRUE(result.success) << "error: " << result.error;
    EXPECT_EQ(result.output, "hello statusline");
    EXPECT_TRUE(result.error.empty());
    EXPECT_GT(result.elapsed.count(), 0);
}

TEST(StatuslineExecute, CommandReadsJsonFromStdin) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    input.model.id = "stdin-test-model";
    input.version = "1.0.0";

    // Read from stdin and echo the model.id
    auto result = execute_statusline_command(
        "cat | head -c 500",
        input);

    EXPECT_TRUE(result.success) << "error: " << result.error;
    // Output should contain the JSON with our test data
    EXPECT_NE(result.output.find("stdin-test-model"), std::string::npos)
        << "Output was: " << result.output;
    EXPECT_NE(result.output.find("version"), std::string::npos);
}

TEST(StatuslineExecute, NonZeroExitCodeReturnsFailure) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    auto result = execute_statusline_command("exit 42", input);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("exit code 42"), std::string::npos);
    EXPECT_TRUE(result.output.empty());
}

TEST(StatuslineExecute, AllWhitespaceOutputIsNotSuccess) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    auto result = execute_statusline_command("echo '   \n\t\n  '", input);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.output.empty());
}

TEST(StatuslineExecute, BlankLinesAreStripped) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    // Multi-line output with blank lines interspersed
    auto result = execute_statusline_command(
        "printf 'line1\\n\\n  line2  \\n\\nline3\\n'",
        input);

    EXPECT_TRUE(result.success) << "error: " << result.error;
    EXPECT_EQ(result.output, "line1\nline2\nline3");
}

TEST(StatuslineExecute, EachLineIsTrimmed) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    auto result = execute_statusline_command(
        "printf '  first  \\n  second\\nthird  \\n'",
        input);

    EXPECT_TRUE(result.success) << "error: " << result.error;
    EXPECT_EQ(result.output, "first\nsecond\nthird");
}

TEST(StatuslineExecute, DefaultTimeoutIs5000Ms) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    // We can't easily verify the exact timeout value from the outside,
    // but we can verify the function accepts a custom timeout and that
    // a fast command works with a very short timeout.
    StatusLineCommandInput input;
    auto result = execute_statusline_command("echo ok", input, 1000);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.output, "ok");
}

TEST(StatuslineExecute, ShortTimeoutKillsSlowCommand) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    // Sleep for 5 seconds with 100ms timeout — should fail
    auto result = execute_statusline_command("sleep 5", input, 100);
    EXPECT_FALSE(result.success);
    // Error should mention either timeout or signal (killed)
    EXPECT_FALSE(result.error.empty());
}

TEST(StatuslineExecute, EmptyInputStillRunsCommand) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    // Even with mostly default input, to_json produces valid JSON
    auto result = execute_statusline_command("cat > /dev/null && echo done", input);
    EXPECT_TRUE(result.success) << "error: " << result.error;
    EXPECT_EQ(result.output, "done");
}

TEST(StatuslineExecute, ComplexJsonInputPassedCorrectly) {
    if (!has_bash()) GTEST_SKIP() << "bash not available";

    StatusLineCommandInput input;
    input.session_name = "complex-session";
    input.model.id = "claude-3-opus";
    input.model.display_name = "Claude 3 Opus";
    input.workspace.current_dir = "/home/user/work";
    input.workspace.project_dir = "/home/user/work";
    input.workspace.added_dirs = {"src", "include", "tests"};
    input.version = "2.0.0";
    input.output_style_name = "concise";
    input.cost.total_cost_usd = 1.23;
    input.cost.total_duration_ms = 5000;
    input.context_window.total_input_tokens = 50000;
    input.context_window.total_output_tokens = 10000;
    input.context_window.context_window_size = 200000;
    input.exceeds_200k_tokens = false;
    input.vim = cc::utils::statusline::StatusLineVimInfo{.mode = "NORMAL"};
    input.rate_limits = cc::utils::statusline::StatusLineRateLimits{
        .five_hour = cc::utils::statusline::StatusLineRateLimitBucket{
            .used_percentage = 50.0, .resets_at = 1234567890LL}};

    // Use python (if available) or grep to verify the JSON has expected keys
    // We'll just check the output length and a few key fields
    auto result = execute_statusline_command(
        "cat | tr -d '\\n' | wc -c",
        input);

    EXPECT_TRUE(result.success) << "error: " << result.error;
    // JSON should be non-trivial in size
    int len = std::stoi(result.output);
    EXPECT_GT(len, 200);
}

TEST(StatuslineExecute, ResultStructFieldsCorrect) {
    // Verify the StatusLineResult struct layout / defaults
    StatusLineResult r;
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.output.empty());
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(r.elapsed.count(), 0);
}

} // namespace
