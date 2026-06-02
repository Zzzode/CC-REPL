#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

import cc.utils.settings_validation;

TEST(SettingsValidationConfig, ClassifiesPatternToolsAndCustomWebValidators) {
    using namespace cc::utils::settings_validation;

    EXPECT_TRUE(is_file_pattern_tool("Read"));
    EXPECT_TRUE(is_file_pattern_tool("NotebookEdit"));
    EXPECT_FALSE(is_file_pattern_tool("Bash"));

    EXPECT_TRUE(is_bash_prefix_tool("Bash"));
    EXPECT_FALSE(is_bash_prefix_tool("WebFetch"));

    auto web_search = validate_tool_content("WebSearch", "claude * docs");
    EXPECT_FALSE(web_search.valid);
    EXPECT_EQ(web_search.error, std::optional<std::string>{"WebSearch does not support wildcards"});
    ASSERT_EQ(web_search.examples.size(), 2u);
    EXPECT_EQ(web_search.examples[0], "WebSearch(claude ai)");

    auto web_fetch_url = validate_tool_content("WebFetch", "https://example.com/path");
    EXPECT_FALSE(web_fetch_url.valid);
    EXPECT_EQ(web_fetch_url.suggestion, std::optional<std::string>{"Use \"domain:hostname\" format"});

    EXPECT_TRUE(validate_tool_content("WebFetch", "domain:*.example.com").valid);
}

TEST(SettingsPermissionValidation, ValidatesRulesAndReportsHelpfulErrors) {
    using namespace cc::utils::settings_validation;

    EXPECT_TRUE(validate_permission_rule("Bash(npm run *)").valid);
    EXPECT_TRUE(validate_permission_rule("Read(src/**)").valid);
    EXPECT_TRUE(validate_permission_rule("mcp__server__tool").valid);

    auto empty = validate_permission_rule("   ");
    EXPECT_FALSE(empty.valid);
    EXPECT_EQ(empty.error, std::optional<std::string>{"Permission rule cannot be empty"});

    auto mismatch = validate_permission_rule("Bash(npm run");
    EXPECT_FALSE(mismatch.valid);
    EXPECT_EQ(mismatch.error, std::optional<std::string>{"Mismatched parentheses"});

    auto empty_parens = validate_permission_rule("Read()");
    EXPECT_FALSE(empty_parens.valid);
    EXPECT_EQ(empty_parens.error, std::optional<std::string>{"Empty parentheses"});
    EXPECT_EQ(empty_parens.examples, (std::vector<std::string>{"Read", "Read(some-pattern)"}));

    auto lower = validate_permission_rule("bash(npm test)");
    EXPECT_FALSE(lower.valid);
    EXPECT_EQ(lower.suggestion, std::optional<std::string>{"Use \"Bash\""});

    auto bash_bad_prefix = validate_permission_rule("Bash(npm:* test)");
    EXPECT_FALSE(bash_bad_prefix.valid);
    EXPECT_EQ(bash_bad_prefix.error, std::optional<std::string>{"The :* pattern must be at the end"});

    auto file_bad_prefix = validate_permission_rule("Read(src:*)");
    EXPECT_FALSE(file_bad_prefix.valid);
    EXPECT_EQ(file_bad_prefix.error, std::optional<std::string>{"The \":*\" syntax is only for Bash prefix rules"});

    auto mcp_pattern = validate_permission_rule("mcp__server__tool(*)");
    EXPECT_FALSE(mcp_pattern.valid);
    EXPECT_EQ(mcp_pattern.error, std::optional<std::string>{"MCP rules do not support patterns in parentheses"});
}

TEST(SettingsValidationTips, MirrorsTypeScriptTipPriorityAndDocFallbacks) {
    using namespace cc::utils::settings_validation;

    auto mode = get_validation_tip({
        .path = "permissions.defaultMode",
        .code = "invalid_value",
        .expected = std::nullopt,
        .received = std::nullopt,
        .enum_values = std::nullopt,
        .message = std::nullopt,
        .value = std::nullopt,
    });
    ASSERT_TRUE(mode.has_value());
    EXPECT_NE(mode->suggestion->find("acceptEdits"), std::string::npos);
    EXPECT_EQ(mode->doc_link, std::optional<std::string>{"https://code.claude.com/docs/en/iam#permission-modes"});

    auto env = get_validation_tip({
        .path = "env.PORT",
        .code = "invalid_type",
        .expected = std::nullopt,
        .received = std::nullopt,
        .enum_values = std::nullopt,
        .message = std::nullopt,
        .value = std::nullopt,
    });
    ASSERT_TRUE(env.has_value());
    EXPECT_EQ(env->doc_link, std::optional<std::string>{"https://code.claude.com/docs/en/settings#environment-variables"});

    auto enum_tip = get_validation_tip({
        .path = "model",
        .code = "invalid_value",
        .expected = std::nullopt,
        .received = std::nullopt,
        .enum_values = std::vector<std::string>{"sonnet", "opus"},
        .message = std::nullopt,
        .value = std::nullopt,
    });
    ASSERT_TRUE(enum_tip.has_value());
    EXPECT_EQ(enum_tip->suggestion, std::optional<std::string>{"Valid values: \"sonnet\", \"opus\""});

    auto unknown = get_validation_tip({
        .path = "other",
        .code = "custom",
        .expected = std::nullopt,
        .received = std::nullopt,
        .enum_values = std::nullopt,
        .message = std::nullopt,
        .value = std::nullopt,
    });
    EXPECT_FALSE(unknown.has_value());
}
