#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

import cc.utils.bash_shell_quoting;
import cc.utils.memory_file_detection;
import cc.utils.model_cost;
import cc.utils.read_file_in_range;

TEST(MemoryFileDetection, DetectsSessionFilesPatternsAndMemoryDirectories) {
    using namespace cc::utils::memory_file_detection;

    const MemoryDetectionConfig config{
        .claude_config_home_dir = "/Users/me/.claude",
        .memory_base_dir = "/Users/me/.claude",
        .auto_mem_path = "/Users/me/.claude/memory",
        .auto_memory_enabled = true,
        .team_memory_enabled = false,
        .team_mem_path = "",
        .windows = false,
    };

    EXPECT_EQ(detect_session_file_type("/Users/me/.claude/session-memory/session-1.md", config), SessionFileType::SessionMemory);
    EXPECT_EQ(detect_session_file_type("/Users/me/.claude/projects/proj/session.jsonl", config), SessionFileType::SessionTranscript);
    EXPECT_FALSE(detect_session_file_type("/Users/me/.claude/CLAUDE.md", config).has_value());

    EXPECT_EQ(detect_session_pattern_type("**/session-memory/*.md"), SessionFileType::SessionMemory);
    EXPECT_EQ(detect_session_pattern_type(".claude/projects/**/*.jsonl"), SessionFileType::SessionTranscript);
    EXPECT_TRUE(is_auto_managed_memory_pattern("/tmp/project/agent-memory/*.md", config));
    EXPECT_FALSE(is_auto_managed_memory_pattern("CLAUDE.md", config));

    EXPECT_TRUE(is_memory_directory("/Users/me/.claude/session-memory/abc/..", config));
    EXPECT_TRUE(is_shell_command_targeting_memory("grep foo /Users/me/.claude/session-memory/session-1.md;", config));
    EXPECT_FALSE(is_shell_command_targeting_memory("grep foo /Users/me/project/CLAUDE.md", config));

    const MemoryDetectionConfig windows_config{
        .claude_config_home_dir = "C:\\Users\\me\\.claude",
        .memory_base_dir = "C:\\Users\\me\\.claude",
        .auto_mem_path = "C:\\Users\\me\\.claude\\memory",
        .auto_memory_enabled = true,
        .team_memory_enabled = false,
        .team_mem_path = "",
        .windows = true,
    };
    EXPECT_TRUE(is_shell_command_targeting_memory("grep foo /c/Users/me/.claude/session-memory/session-1.md", windows_config));
}

TEST(ModelCost, CalculatesTokenCostsAndFormatsPricingStrings) {
    using namespace cc::utils::model_cost;

    Usage usage{
        .input_tokens = 1'000'000,
        .output_tokens = 2'000'000,
        .cache_read_input_tokens = 500'000,
        .cache_creation_input_tokens = 250'000,
        .web_search_requests = 3,
        .speed = std::nullopt,
    };

    EXPECT_DOUBLE_EQ(tokens_to_usd_cost(COST_TIER_3_15, usage), 34.1175);
    EXPECT_EQ(format_model_pricing(COST_TIER_3_15), "$3/$15 per Mtok");
    EXPECT_EQ(format_model_pricing(COST_HAIKU_35), "$0.80/$4 per Mtok");

    const auto opus_fast = get_model_costs("claude-opus-4-6", Usage{.speed = "fast"}, true);
    EXPECT_DOUBLE_EQ(opus_fast.input_tokens, 30.0);
    const auto unknown = get_model_costs("provider-unknown-model", Usage{}, true, "claude-sonnet-4-5");
    EXPECT_DOUBLE_EQ(unknown.input_tokens, 3.0);
    EXPECT_EQ(get_model_pricing_string("claude-sonnet-4-5"), std::optional<std::string>{"$3/$15 per Mtok"});
    EXPECT_FALSE(get_model_pricing_string("unknown-model").has_value());
}

TEST(ReadFileInRange, ReadsLineRangesStripsBomCrAndTruncatesByBytes) {
    const auto path = std::filesystem::temp_directory_path() / "cc_repl_read_file_in_range_test.txt";
    {
        std::ofstream out(path, std::ios::binary);
        out << "\xEF\xBB\xBF" "first\r\nsecond\r\nthird\n";
    }

    auto selected = cc::utils::read_file_in_range::read_file_in_range(path.string(), 1, 2);
    ASSERT_TRUE(selected.has_value()) << selected.error().message;
    EXPECT_EQ(selected->content, "second\nthird");
    EXPECT_EQ(selected->line_count, 2u);
    EXPECT_EQ(selected->total_lines, 4u);
    EXPECT_EQ(selected->read_bytes, 12u);
    EXPECT_GT(selected->mtime_ms, 0.0);

    auto truncated = cc::utils::read_file_in_range::read_file_in_range(
        path.string(), 0, std::nullopt, 11, true);
    ASSERT_TRUE(truncated.has_value()) << truncated.error().message;
    EXPECT_EQ(truncated->content, "first");
    EXPECT_TRUE(truncated->truncated_by_bytes);

    auto too_large = cc::utils::read_file_in_range::read_file_in_range(path.string(), 0, std::nullopt, 5, false);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().kind, cc::utils::read_file_in_range::ReadFileRangeErrorKind::FileTooLarge);

    std::filesystem::remove(path);
}

TEST(BashShellQuoting, QuotesCommandsDetectsRedirectsAndRewritesWindowsNull) {
    using namespace cc::utils::bash_shell_quoting;

    EXPECT_TRUE(has_shell_quote_single_quote_bug(R"(git ls-remote 'safe\' '--upload-pack=evil' 'repo')"));
    EXPECT_FALSE(has_shell_quote_single_quote_bug(R"(echo 'safe\\')"));

    EXPECT_EQ(quote_shell_command("cat <<EOF\nhi!\nEOF"), "'cat <<EOF\nhi!\nEOF'");
    EXPECT_EQ(quote_shell_command("printf 'a\nb'", true), "'printf '\"'\"'a\nb'\"'\"'' < /dev/null");
    EXPECT_TRUE(has_stdin_redirect("cat < input.txt"));
    EXPECT_FALSE(has_stdin_redirect("cat <<EOF"));
    EXPECT_FALSE(should_add_stdin_redirect("cat < input.txt"));
    EXPECT_FALSE(should_add_stdin_redirect("cat <<EOF"));
    EXPECT_TRUE(should_add_stdin_redirect("cat file.txt"));
    EXPECT_EQ(rewrite_windows_null_redirect("ls >nul 2>NUL && echo keep >null"), "ls >/dev/null 2>/dev/null && echo keep >null");
}

TEST(BashPrefix, CollapsesCompoundPrefixesByWordAlignedRoot) {
    using namespace cc::utils::bash_shell_quoting;

    EXPECT_EQ(longest_common_prefix({"git fetch origin", "git worktree list"}), "git");
    EXPECT_EQ(longest_common_prefix({"npm run test", "npm run lint"}), "npm run");
    EXPECT_EQ(compound_command_prefixes("git fetch origin && git worktree list"), (std::vector<std::string>{"git"}));
    EXPECT_EQ(compound_command_prefixes("git status && npm run test"), (std::vector<std::string>{"git status", "npm run test"}));
}

