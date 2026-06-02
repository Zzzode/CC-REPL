/// @file test_utils.cpp
/// @brief cc_utils 模块单元测试
/// 覆盖: string, json, error, circular_buffer, token_budget, shell_parser, permissions

#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

import cc.utils.string;
import cc.utils.string_utils;
import cc.utils.array_utils;
import cc.utils.json;
import cc.utils.error;
import cc.utils.circular_buffer;
import cc.utils.token_budget;
import cc.utils.settings_sources;
import cc.utils.ssrf_guard;
import cc.utils.plugin_identifier;
import cc.utils.plugin_dependency_resolver;
import cc.utils.settings_paths;
import cc.utils.settings_merge;
import cc.utils.plugin_marketplace_rules;
import cc.utils.plugin_versioning;
import cc.utils.argument_substitution;
import cc.utils.semantic_boolean;
import cc.utils.semantic_number;
import cc.utils.query_guard;
import cc.utils.collapse_notifications;
import cc.utils.agent_id;
import cc.utils.auto_mode_denials;
import cc.utils.activity_manager;
import cc.utils.agent_swarms_enabled;
import cc.utils.env_utils;
import cc.utils.cache_paths;
import cc.utils.binary_check;
import cc.utils.claude_code_hints;
import cc.utils.commit_attribution;
import cc.utils.hash;
import cc.utils.tagged_id;
import cc.utils.message_predicates;
import cc.utils.content_array;
import cc.utils.object_group_by;
import cc.utils.timeouts;
import cc.utils.slash_command_parsing;
import cc.utils.collapse_read_search;
import cc.utils.set_utils;
import cc.utils.words;
import cc.utils.fps_tracker;
import cc.utils.privacy_level;
import cc.utils.script_tool_enabled;
import cc.utils.prompt_category;
import cc.utils.control_message_compat;
import cc.utils.sanitization;

// ═══════════════════════════════════════════════════════════════════════════════
// cc.utils.string 测试: 字符串操作工具函数
// ═══════════════════════════════════════════════════════════════════════════════

TEST(StringUtils, TrimRemovesWhitespace) {
    EXPECT_EQ(cc::utils::string::trim("  hello  "), "hello");
    EXPECT_EQ(cc::utils::string::trim("\t\n text \r\n"), "text");
    EXPECT_EQ(cc::utils::string::trim("no_change"), "no_change");
    EXPECT_EQ(cc::utils::string::trim(""), "");
}

TEST(StringUtils, SplitByDelimiter) {
    auto parts = cc::utils::string::split("a,b,c", ',');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");

    // 空字符串分割应返回单个空元素
    auto empty = cc::utils::string::split("", ',');
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty[0], "");
}

TEST(StringUtils, SplitHandlesConsecutiveDelimiters) {
    auto parts = cc::utils::string::split("a,,b", ',');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[1], "");
}

TEST(StringUtils, JoinCombinesStrings) {
    std::vector<std::string> items = {"hello", "world", "test"};
    EXPECT_EQ(cc::utils::string::join(items, " "), "hello world test");
    EXPECT_EQ(cc::utils::string::join(items, ", "), "hello, world, test");

    // 空向量应返回空字符串
    std::vector<std::string> empty_vec;
    EXPECT_EQ(cc::utils::string::join(empty_vec, ","), "");
}

TEST(StringUtils, CaseConversion) {
    EXPECT_EQ(cc::utils::string::to_lower("Hello World"), "hello world");
    EXPECT_EQ(cc::utils::string::to_upper("Hello World"), "HELLO WORLD");
    EXPECT_EQ(cc::utils::string::to_lower("ALREADY"), "already");
    EXPECT_EQ(cc::utils::string::to_upper("already"), "ALREADY");
}

TEST(StringUtils, StartsWithAndEndsWith) {
    EXPECT_TRUE(cc::utils::string::starts_with("hello world", "hello"));
    EXPECT_FALSE(cc::utils::string::starts_with("hello world", "world"));
    EXPECT_TRUE(cc::utils::string::ends_with("hello world", "world"));
    EXPECT_FALSE(cc::utils::string::ends_with("hello world", "hello"));

    // 边界情况: 空前缀/后缀
    EXPECT_TRUE(cc::utils::string::starts_with("anything", ""));
    EXPECT_TRUE(cc::utils::string::ends_with("anything", ""));
}

TEST(StringUtils, ReplaceAllTreatsEmptyNeedleAsNoop) {
    EXPECT_EQ(cc::utils::string::replace_all("abc", "", "x"), "abc");
    EXPECT_EQ(cc::utils::string::replace_all("a-b-a", "a", "z"), "z-b-z");
}

TEST(StringUtils, TruncateDoesNotUnderflowWhenEllipsisIsLongerThanLimit) {
    EXPECT_EQ(cc::utils::string::truncate("abcdef", 2, "..."), "..");
    EXPECT_EQ(cc::utils::string::truncate("abcdef", 5, "..."), "ab...");
}

TEST(StringUtilsCompat, EscapeRegexEscapesSpecialCharacters) {
    EXPECT_EQ(cc::utils::escape_regex(R"(a.b*c+$^?{}()|[]\)"), R"(a\.b\*c\+\$\^\?\{\}\(\)\|\[\]\\)");
    EXPECT_EQ(cc::utils::escape_regex("plain"), "plain");
}

TEST(StringUtilsCompat, CapitalizePreservesRestOfString) {
    EXPECT_EQ(cc::utils::capitalize("fooBar"), "FooBar");
    EXPECT_EQ(cc::utils::capitalize("hELLO"), "HELLO");
    EXPECT_EQ(cc::utils::capitalize(""), "");
}

TEST(StringUtilsCompat, PluralSupportsRegularAndIrregularForms) {
    EXPECT_EQ(cc::utils::plural(1, "file"), "file");
    EXPECT_EQ(cc::utils::plural(2, "file"), "files");
    EXPECT_EQ(cc::utils::plural(2, "entry", "entries"), "entries");
}

TEST(StringUtilsCompat, FirstLineAndCountCharMatchTypeScriptHelpers) {
    EXPECT_EQ(cc::utils::first_line_of("#!/bin/sh\necho ok"), "#!/bin/sh");
    EXPECT_EQ(cc::utils::first_line_of("single"), "single");
    EXPECT_EQ(cc::utils::count_char_in_string("a,b,c,d", ',', 2), 2u);
}

TEST(StringUtilsCompat, NormalizesFullWidthDigitsAndSpace) {
    EXPECT_EQ(cc::utils::normalize_full_width_digits("０１２３ abc ９"), "0123 abc 9");
    EXPECT_EQ(cc::utils::normalize_full_width_space("foo　bar　baz"), "foo bar baz");
}

TEST(StringUtilsCompat, SafeJoinLinesTruncatesLikeTypeScriptHelper) {
    std::vector<std::string> lines = {"abc", "def", "ghi"};
    EXPECT_EQ(cc::utils::safe_join_lines(lines, ",", 10), "abc,def...[truncated]");
    EXPECT_EQ(cc::utils::safe_join_lines(lines, ",", 20), "abc,def,ghi");
}

TEST(ArrayUtilsCompat, CountUniqAndIntersperseMatchTypeScriptHelpers) {
    std::vector<int> nums = {1, 2, 2, 3, 4};
    EXPECT_EQ(cc::utils::count(nums, [](int value) { return value % 2 == 0; }), 3u);
    EXPECT_EQ(cc::utils::uniq(nums), (std::vector<int>{1, 2, 3, 4}));

    std::vector<std::string> labels = {"a", "b", "c"};
    auto interspersed = cc::utils::intersperse(labels, [](std::size_t index) {
        return std::string("|") + std::to_string(index);
    });
    EXPECT_EQ(interspersed, (std::vector<std::string>{"a", "|1", "b", "|2", "c"}));
}

TEST(SetUtilsCompat, DifferenceIntersectsEveryAndUnionMatchTypeScriptHelpers) {
    const std::set<std::string> a = {"alpha", "beta", "gamma"};
    const std::set<std::string> b = {"beta", "delta"};

    EXPECT_EQ(cc::utils::difference(a, b), (std::set<std::string>{"alpha", "gamma"}));
    EXPECT_TRUE(cc::utils::intersects(a, b));
    EXPECT_FALSE(cc::utils::intersects(std::set<std::string>{}, b));
    EXPECT_TRUE(cc::utils::every(std::set<std::string>{"alpha", "gamma"}, a));
    EXPECT_FALSE(cc::utils::every(a, b));
    EXPECT_EQ(cc::utils::union_sets(a, b), (std::set<std::string>{"alpha", "beta", "delta", "gamma"}));
}

TEST(CollapseReadSearchSummary, BuildsActiveAndCompletedSummaryText) {
    EXPECT_EQ(
        cc::utils::collapse_read_search::get_search_read_summary_text(3, 2, true, 1),
        "Searching for 3 patterns, reading 2 files, REPL'ing 1 time…");
    EXPECT_EQ(
        cc::utils::collapse_read_search::get_search_read_summary_text(1, 1, false, 2),
        "Searched for 1 pattern, read 1 file, REPL'd 2 times");
}

TEST(CollapseReadSearchSummary, PutsMemoryAndListOperationsInTypeScriptOrder) {
    cc::utils::collapse_read_search::MemoryCounts memory_counts{
        .memory_search_count = 1,
        .memory_read_count = 2,
        .memory_write_count = 1,
    };

    EXPECT_EQ(
        cc::utils::collapse_read_search::get_search_read_summary_text(
            1, 0, true, 0, memory_counts, 2),
        "Recalling 2 memories, searching memories, writing 1 memory, searching for 1 pattern, listing 2 directories…");
}

TEST(CollapseReadSearchSummary, IncludesTeamMemorySummaryPartsInTypeScriptOrder) {
    cc::utils::collapse_read_search::MemoryCounts memory_counts{
        .memory_search_count = 1,
        .memory_read_count = 1,
        .memory_write_count = 0,
        .team_memory_search_count = 1,
        .team_memory_read_count = 2,
        .team_memory_write_count = 1,
    };

    EXPECT_EQ(
        cc::utils::collapse_read_search::get_search_read_summary_text(
            0, 1, false, 0, memory_counts),
        "Recalled 1 memory, searched memories, recalled 2 team memories, searched team memories, wrote 1 team memory, read 1 file");
}

TEST(CollapseReadSearchSummary, SummarizesTrailingSearchReadActivities) {
    using cc::utils::collapse_read_search::RecentActivity;
    const std::vector<RecentActivity> activities = {
        {.activity_description = "Edited file"},
        {.activity_description = "Grep", .is_search = true},
        {.activity_description = "Read", .is_read = true},
        {.activity_description = "Glob", .is_search = true},
    };

    EXPECT_EQ(
        cc::utils::collapse_read_search::summarize_recent_activities(activities),
        "Searching for 2 patterns, reading 1 file…");
}

TEST(CollapseReadSearchSummary, FallsBackToMostRecentDescription) {
    using cc::utils::collapse_read_search::RecentActivity;

    EXPECT_EQ(cc::utils::collapse_read_search::summarize_recent_activities({}), std::nullopt);
    EXPECT_EQ(
        cc::utils::collapse_read_search::summarize_recent_activities({
            RecentActivity{.activity_description = "Ran command"},
            RecentActivity{},
        }),
        std::optional<std::string>{"Ran command"});
}

TEST(WordsSlug, ExposesTypeScriptWordTablesAndDeterministicSlugAssembly) {
    EXPECT_TRUE(cc::utils::words::is_adjective("abundant"));
    EXPECT_TRUE(cc::utils::words::is_adjective("virtual"));
    EXPECT_TRUE(cc::utils::words::is_noun("aurora"));
    EXPECT_TRUE(cc::utils::words::is_noun("yao"));
    EXPECT_TRUE(cc::utils::words::is_verb("baking"));
    EXPECT_TRUE(cc::utils::words::is_verb("zooming"));
    EXPECT_FALSE(cc::utils::words::is_noun("not-in-table"));

    EXPECT_EQ(cc::utils::words::word_slug_from_indices(0, 4, 0), "abundant-brewing-aurora");
    EXPECT_EQ(cc::utils::words::short_word_slug_from_indices(0, 0), "abundant-aurora");
}

TEST(WordsSlug, GeneratesRandomSlugsWithExpectedShapeAndKnownWords) {
    const auto slug = cc::utils::words::generate_word_slug();
    const auto parts = cc::utils::string::split(slug, '-');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_TRUE(cc::utils::words::is_adjective(parts[0]));
    EXPECT_TRUE(cc::utils::words::is_verb(parts[1]));
    EXPECT_TRUE(cc::utils::words::is_noun(parts[2]));

    const auto short_slug = cc::utils::words::generate_short_word_slug();
    const auto short_parts = cc::utils::string::split(short_slug, '-');
    ASSERT_EQ(short_parts.size(), 2u);
    EXPECT_TRUE(cc::utils::words::is_adjective(short_parts[0]));
    EXPECT_TRUE(cc::utils::words::is_noun(short_parts[1]));
}

TEST(FpsTracker, ReturnsNulloptBeforeFramesOrWithoutElapsedTime) {
    cc::utils::fps::FpsTracker tracker;
    EXPECT_EQ(tracker.get_metrics(), std::nullopt);

    tracker.record(16.0, 1000.0);
    EXPECT_EQ(tracker.get_metrics(), std::nullopt);
}

TEST(FpsTracker, ComputesRoundedAverageAndLowOnePercentFps) {
    cc::utils::fps::FpsTracker tracker;
    tracker.record(10.0, 1000.0);
    tracker.record(20.0, 1100.0);
    tracker.record(50.0, 1250.0);

    const auto metrics = tracker.get_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_DOUBLE_EQ(metrics->average_fps, 12.0);
    EXPECT_DOUBLE_EQ(metrics->low_1_pct_fps, 20.0);
}

TEST(PrivacyLevel, ResolvesMostRestrictiveTrafficAndTelemetrySignals) {
    using cc::utils::privacy::EnvLike;
    EXPECT_EQ(cc::utils::privacy::get_privacy_level(EnvLike{}), cc::utils::privacy::PrivacyLevel::Default);
    EXPECT_EQ(cc::utils::privacy::get_privacy_level({{"DISABLE_TELEMETRY", "1"}}), cc::utils::privacy::PrivacyLevel::NoTelemetry);
    EXPECT_EQ(cc::utils::privacy::get_privacy_level({{"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "0"}}), cc::utils::privacy::PrivacyLevel::EssentialTraffic);
    EXPECT_TRUE(cc::utils::privacy::is_telemetry_disabled({{"DISABLE_TELEMETRY", "true"}}));
    EXPECT_TRUE(cc::utils::privacy::is_essential_traffic_only({{"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "true"}}));
    EXPECT_EQ(
        cc::utils::privacy::get_essential_traffic_only_reason({{"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "true"}}),
        std::optional<std::string>{"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"});
}

TEST(ScriptToolEnabled, MirrorsScriptAndBashToolEnvironmentChecks) {
    using cc::utils::script_tool::EnvLike;
    EXPECT_FALSE(cc::utils::script_tool::is_script_tool_enabled(EnvLike{}));
    EXPECT_TRUE(cc::utils::script_tool::is_script_tool_enabled({{"ENABLE_SCRIPT_TOOL", "yes"}}));
    EXPECT_FALSE(cc::utils::script_tool::is_script_tool_enabled({{"ENABLE_SCRIPT_TOOL", "0"}}));
    EXPECT_TRUE(cc::utils::script_tool::is_bash_tool_disabled({{"DISABLE_BASH_TOOL", "on"}}));
    EXPECT_TRUE(cc::utils::script_tool::is_bash_tool_disabled({{"ENABLE_SCRIPT_TOOL", "true"}}));
    EXPECT_FALSE(cc::utils::script_tool::is_bash_tool_disabled({{"DISABLE_BASH_TOOL", "false"}}));
}

TEST(PromptCategory, BuildsAgentAndReplQuerySources) {
    EXPECT_EQ(cc::utils::prompt_category::get_query_source_for_agent("reviewer", true), "agent:builtin:reviewer");
    EXPECT_EQ(cc::utils::prompt_category::get_query_source_for_agent(std::nullopt, true), "agent:default");
    EXPECT_EQ(cc::utils::prompt_category::get_query_source_for_agent("anything", false), "agent:custom");

    const std::set<std::string> builtin_styles = {"default", "explanatory", "learning"};
    EXPECT_EQ(cc::utils::prompt_category::get_query_source_for_repl("default", builtin_styles), "repl_main_thread");
    EXPECT_EQ(cc::utils::prompt_category::get_query_source_for_repl("learning", builtin_styles), "repl_main_thread:outputStyle:learning");
    EXPECT_EQ(cc::utils::prompt_category::get_query_source_for_repl("my-style", builtin_styles), "repl_main_thread:outputStyle:custom");
}

TEST(ControlMessageCompat, NormalizesRequestIdKeysWithSnakeCasePrecedence) {
    cc::utils::control_message_compat::ControlMessageLike message;
    message.fields["requestId"] = "camel-root";
    message.has_response = true;
    message.response["requestId"] = "camel-response";

    cc::utils::control_message_compat::normalize_control_message_keys(message);
    EXPECT_FALSE(message.fields.contains("requestId"));
    EXPECT_EQ(message.fields.at("request_id"), "camel-root");
    EXPECT_FALSE(message.response.contains("requestId"));
    EXPECT_EQ(message.response.at("request_id"), "camel-response");

    message.fields["requestId"] = "ignored";
    message.fields["request_id"] = "snake-wins";
    cc::utils::control_message_compat::normalize_control_message_keys(message);
    EXPECT_EQ(message.fields.at("request_id"), "snake-wins");
    EXPECT_TRUE(message.fields.contains("requestId"));
}

TEST(Sanitization, RemovesDangerousHiddenUnicodeRanges) {
    EXPECT_EQ(
        cc::utils::sanitization::partially_sanitize_unicode("safe\xE2\x80\x8Bhidden\xEF\xBB\xBFtext"),
        "safehiddentext");
    EXPECT_EQ(
        cc::utils::sanitization::partially_sanitize_unicode("left\xE2\x80\xAEright"),
        "leftright");
    EXPECT_EQ(
        cc::utils::sanitization::partially_sanitize_unicode("private\xEE\x80\x80use"),
        "privateuse");
}

TEST(Sanitization, AppliesCompatibilityNormalizationBeforeFiltering) {
    EXPECT_EQ(
        cc::utils::sanitization::partially_sanitize_unicode("\xEF\xBC\xA8\xEF\xBD\x85\xEF\xBD\x8C\xEF\xBD\x8C\xEF\xBD\x8F\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93"),
        "Hello123");
    EXPECT_EQ(cc::utils::sanitization::partially_sanitize_unicode(std::string("o") + "\xEF\xAC\x83" + "ce"), "office");
    EXPECT_EQ(cc::utils::sanitization::partially_sanitize_unicode("\xE2\x91\xA0\xE2\x85\xA0\xC2\xB2\xE3\x8E\x8F"), "1I2kg");
}

TEST(Sanitization, RecursivelySanitizesStringsArraysObjectsAndKeys) {
    using cc::utils::sanitization::SanitizedValue;

    SanitizedValue input = SanitizedValue::object({
        {"safe\xE2\x80\x8Bkey", SanitizedValue("value\xEF\xBB\xBFtext")},
        {"items", SanitizedValue::array({
            SanitizedValue("\xEF\xBC\xA8\xEF\xBD\x89"),
            SanitizedValue::object({{"nested\xE2\x80\xAEkey", SanitizedValue("ok")}}),
        })},
    });

    auto sanitized = cc::utils::sanitization::recursively_sanitize_unicode(input);
    const auto& obj = sanitized.as_object();
    ASSERT_TRUE(obj.contains("safekey"));
    EXPECT_EQ(obj.at("safekey").as_string(), "valuetext");
    const auto& items = obj.at("items").as_array();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].as_string(), "Hi");
    EXPECT_TRUE(items[1].as_object().contains("nestedkey"));
}

TEST(Sanitization, RecursivelySanitizedObjectKeysUseLastWriteWinsOnCollisions) {
    using cc::utils::sanitization::SanitizedValue;

    SanitizedValue input = SanitizedValue::object({
        {"safekey", SanitizedValue("plain")},
        {"safe\xE2\x80\x8Bkey", SanitizedValue("hidden")},
    });

    auto sanitized = cc::utils::sanitization::recursively_sanitize_unicode(input);
    const auto& obj = sanitized.as_object();
    ASSERT_TRUE(obj.contains("safekey"));
    EXPECT_EQ(obj.at("safekey").as_string(), "hidden");
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.utils.json 测试: yyjson 封装层
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonUtils, ParseValidJson) {
    auto doc = cc::utils::json::parse(R"({"name":"test","value":42})");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->get_string("name"), "test");
    EXPECT_EQ(doc->get_int("value"), 42);
}

TEST(JsonUtils, ParseInvalidJsonReturnsError) {
    auto doc = cc::utils::json::parse("{invalid json}");
    EXPECT_FALSE(doc.has_value());
}

TEST(JsonUtils, SerializeToString) {
    auto obj = cc::utils::json::object();
    obj.set("key", "value");
    obj.set("num", 123);

    auto str = obj.serialize();
    EXPECT_TRUE(str.find("\"key\"") != std::string::npos);
    EXPECT_TRUE(str.find("\"value\"") != std::string::npos);
    EXPECT_TRUE(str.find("123") != std::string::npos);
}

TEST(JsonUtils, ArrayOperations) {
    auto arr = cc::utils::json::array();
    arr.push(1);
    arr.push(2);
    arr.push(3);

    EXPECT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr.get_int(0), 1);
    EXPECT_EQ(arr.get_int(2), 3);
}

TEST(JsonUtils, NestedObjectAccess) {
    auto doc = cc::utils::json::parse(R"({"outer":{"inner":"deep"}})");
    ASSERT_TRUE(doc.has_value());
    auto inner = doc->get_object("outer");
    ASSERT_TRUE(inner.has_value());
    EXPECT_EQ(inner->get_string("inner"), "deep");
}

TEST(JsonUtils, NullAndMissingFields) {
    auto doc = cc::utils::json::parse(R"({"key":null})");
    ASSERT_TRUE(doc.has_value());
    EXPECT_TRUE(doc->is_null("key"));
    EXPECT_FALSE(doc->has("nonexistent"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.utils.error 测试: 错误类型与链式错误
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ErrorUtils, CreateBasicError) {
    auto err = cc::utils::error::make("something went wrong");
    EXPECT_EQ(err.message(), "something went wrong");
    EXPECT_FALSE(err.has_cause());
}

TEST(ErrorUtils, ErrorChaining) {
    auto root = cc::utils::error::make("root cause");
    auto wrapped = cc::utils::error::wrap(root, "higher level failure");

    EXPECT_EQ(wrapped.message(), "higher level failure");
    EXPECT_TRUE(wrapped.has_cause());
    EXPECT_EQ(wrapped.cause().message(), "root cause");
}

TEST(ErrorUtils, ExpectedWithValue) {
    auto result = cc::utils::error::expected<int>(42);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
}

TEST(ErrorUtils, ExpectedWithError) {
    auto result = cc::utils::error::expected<int>(
        cc::utils::error::make("computation failed"));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), "computation failed");
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.utils.circular_buffer 测试: 环形缓冲区
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CircularBuffer, PushAndPop) {
    cc::utils::CircularBuffer<int, 4> buf;

    buf.push_back(1);
    buf.push_back(2);
    buf.push_back(3);

    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf.front(), 1);
    buf.pop_front();
    EXPECT_EQ(buf.front(), 2);
    buf.pop_front();
    EXPECT_EQ(buf.front(), 3);
    buf.pop_front();
    EXPECT_TRUE(buf.empty());
}

TEST(CircularBuffer, FullStateOverwrites) {
    cc::utils::CircularBuffer<int, 3> buf;

    buf.push_back(1);
    buf.push_back(2);
    buf.push_back(3);
    EXPECT_TRUE(buf.full());

    // 溢出时应覆盖最早的元素
    buf.push_back(4);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf.front(), 2); // 1 被覆盖
    EXPECT_EQ(buf.back(), 4);
}

TEST(CircularBuffer, EmptyState) {
    cc::utils::CircularBuffer<int, 4> buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_EQ(buf.capacity(), 4u);
}

TEST(CircularBuffer, Iteration) {
    cc::utils::CircularBuffer<int, 8> buf;
    for (int i = 0; i < 5; ++i) buf.push_back(i);

    std::vector<int> collected;
    for (auto val : buf) {
        collected.push_back(val);
    }
    ASSERT_EQ(collected.size(), 5u);
    EXPECT_EQ(collected[0], 0);
    EXPECT_EQ(collected[4], 4);
}

TEST(CircularBuffer, TypeScriptCompatibleRecentAndArrayViews) {
    cc::utils::CircularBuffer<int, 3> buf;
    buf.add_all({1, 2, 3, 4});

    EXPECT_EQ(buf.length(), 3u);
    EXPECT_EQ(buf.to_array(), (std::vector<int>{2, 3, 4}));
    EXPECT_EQ(buf.get_recent(2), (std::vector<int>{3, 4}));
    EXPECT_EQ(buf.get_recent(10), (std::vector<int>{2, 3, 4}));

    buf.clear();
    EXPECT_EQ(buf.length(), 0u);
    EXPECT_TRUE(buf.to_array().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.utils.token_budget 测试: Token 预算管理
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TokenBudget, BasicAllocation) {
    cc::utils::token_budget::TokenBudget config{
        .max_context = 1000,
        .max_output = 100,
        .reserved_for_tools = 100,
        .reserved_for_system = 100,
    };
    cc::utils::token_budget::BudgetManager budget(config);

    auto remaining_in_category = budget.allocate(
        cc::utils::token_budget::BudgetCategory::SystemPrompt, 80);
    EXPECT_EQ(remaining_in_category, 20u);
    EXPECT_TRUE(budget.can_fit(200));
    EXPECT_EQ(budget.remaining(), 920u);
}

TEST(TokenBudget, AvailableForMessagesAccountsForReservedTokens) {
    cc::utils::token_budget::TokenBudget budget{
        .max_context = 1000,
        .max_output = 100,
        .reserved_for_tools = 200,
        .reserved_for_system = 100,
    };

    EXPECT_EQ(budget.available_for_messages(), 600u);
}

TEST(TokenBudget, OverflowHandling) {
    cc::utils::token_budget::TokenBudget config{
        .max_context = 100,
        .max_output = 0,
        .reserved_for_tools = 0,
        .reserved_for_system = 100,
    };
    cc::utils::token_budget::BudgetManager budget(config);

    EXPECT_EQ(budget.allocate(cc::utils::token_budget::BudgetCategory::SystemPrompt, 80), 20u);

    // 超出分类限额时分配仍记录用量，但剩余量归零并触发容量检查失败
    EXPECT_EQ(budget.allocate(cc::utils::token_budget::BudgetCategory::SystemPrompt, 50), 0u);
    EXPECT_FALSE(budget.can_fit(1));
    EXPECT_EQ(budget.remaining(), 0u);
    EXPECT_TRUE(budget.should_compact());
}

TEST(TokenBudget, ResetBudget) {
    cc::utils::token_budget::TokenBudget config{
        .max_context = 500,
        .max_output = 50,
        .reserved_for_tools = 50,
        .reserved_for_system = 50,
    };
    cc::utils::token_budget::BudgetManager budget(config);
    (void)budget.allocate(cc::utils::token_budget::BudgetCategory::UserMessages, 300);
    budget.reset();
    EXPECT_EQ(budget.remaining(), 500u);
}

TEST(TokenBudget, EstimatesTextTokens) {
    cc::utils::token_budget::TokenEstimator estimator;
    EXPECT_GT(estimator.estimate_tokens("hello world"), 0u);
    EXPECT_GT(estimator.estimate_message_tokens("user", "hello world"),
              estimator.estimate_tokens("hello world"));
}

TEST(TokenBudget, ParsesOriginalTokenBudgetSyntax) {
    EXPECT_EQ(cc::utils::token_budget::parse_token_budget("+500k"), 500000u);
    EXPECT_EQ(cc::utils::token_budget::parse_token_budget("please use 2M tokens"), 2000000u);
    EXPECT_EQ(cc::utils::token_budget::parse_token_budget("finish with +1.5m."), 1500000u);
    EXPECT_FALSE(cc::utils::token_budget::parse_token_budget("budget 500k in the middle").has_value());
}

TEST(TokenBudget, FindsBudgetPositionsAndFormatsContinuationMessage) {
    auto positions = cc::utils::token_budget::find_token_budget_positions(" +1.5m and use 2k tokens");

    ASSERT_EQ(positions.size(), 2u);
    EXPECT_EQ(positions[0].start, 1u);
    EXPECT_EQ(positions[0].end, 6u);
    EXPECT_EQ(positions[1].start, 11u);
    EXPECT_EQ(positions[1].end, 24u);

    EXPECT_EQ(
        cc::utils::token_budget::get_budget_continuation_message(80, 1200, 1500),
        "Stopped at 80% of token target (1,200 / 1,500). Keep working — do not summarize.");
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.utils settings/hooks/plugins 纯函数迁移覆盖
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SettingsSources, ParsesCliFlagAndFormatsDisplayNames) {
    using cc::utils::settings_sources::SettingSource;

    auto parsed = cc::utils::settings_sources::parse_setting_sources_flag("user, project,local");
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed.value(), (std::vector<SettingSource>{
        SettingSource::UserSettings,
        SettingSource::ProjectSettings,
        SettingSource::LocalSettings,
    }));

    EXPECT_TRUE(cc::utils::settings_sources::parse_setting_sources_flag("")->empty());
    EXPECT_FALSE(cc::utils::settings_sources::parse_setting_sources_flag("user,managed").has_value());
    EXPECT_EQ(cc::utils::settings_sources::get_setting_source_name(SettingSource::LocalSettings), "project, gitignored");
    EXPECT_EQ(cc::utils::settings_sources::get_source_display_name(SettingSource::PolicySettings), "Managed");
    EXPECT_EQ(cc::utils::settings_sources::get_display_name_lowercase("cliArg"), "CLI argument");
    EXPECT_EQ(cc::utils::settings_sources::get_display_name_capitalized("session"), "Current session");
}

TEST(SsrfGuard, BlocksPrivateLinkLocalAndMappedAddressesButAllowsLoopback) {
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("0.1.2.3"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("10.0.0.1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("100.64.0.0"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("100.127.255.255"));
    EXPECT_FALSE(cc::utils::ssrf_guard::is_blocked_address("100.128.0.1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("169.254.169.254"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("172.16.0.1"));
    EXPECT_FALSE(cc::utils::ssrf_guard::is_blocked_address("172.32.0.1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("192.168.1.1"));
    EXPECT_FALSE(cc::utils::ssrf_guard::is_blocked_address("127.0.0.1"));
    EXPECT_FALSE(cc::utils::ssrf_guard::is_blocked_address("8.8.8.8"));

    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("::"));
    EXPECT_FALSE(cc::utils::ssrf_guard::is_blocked_address("::1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("fc00::1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("fdff::1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("fe80::1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("febf::1"));
    EXPECT_FALSE(cc::utils::ssrf_guard::is_blocked_address("fec0::1"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("::ffff:169.254.169.254"));
    EXPECT_TRUE(cc::utils::ssrf_guard::is_blocked_address("::ffff:a9fe:a9fe"));
    EXPECT_FALSE(cc::utils::ssrf_guard::is_blocked_address("example.com"));
}

TEST(PluginIdentifier, ParsesBuildsAndMapsScopes) {
    auto parsed = cc::utils::plugin_identifier::parse_plugin_identifier("plugin@market@ignored");
    EXPECT_EQ(parsed.name, "plugin");
    ASSERT_TRUE(parsed.marketplace.has_value());
    EXPECT_EQ(*parsed.marketplace, "market");

    auto bare = cc::utils::plugin_identifier::parse_plugin_identifier("local-plugin");
    EXPECT_EQ(bare.name, "local-plugin");
    EXPECT_FALSE(bare.marketplace.has_value());

    EXPECT_EQ(cc::utils::plugin_identifier::build_plugin_id("a", "b"), "a@b");
    EXPECT_EQ(cc::utils::plugin_identifier::build_plugin_id("a", std::nullopt), "a");
    EXPECT_TRUE(cc::utils::plugin_identifier::is_official_marketplace_name("anthropic-marketplace"));
    EXPECT_TRUE(cc::utils::plugin_identifier::is_official_marketplace_name("ANTHROPIC-MARKETPLACE"));
    EXPECT_FALSE(cc::utils::plugin_identifier::is_official_marketplace_name("third-party"));

    auto source = cc::utils::plugin_identifier::scope_to_setting_source(cc::utils::plugin_identifier::PluginScope::Project);
    ASSERT_TRUE(source.has_value()) << source.error();
    EXPECT_EQ(source.value(), cc::utils::settings_sources::SettingSource::ProjectSettings);
    EXPECT_FALSE(cc::utils::plugin_identifier::scope_to_setting_source(cc::utils::plugin_identifier::PluginScope::Managed).has_value());
    EXPECT_EQ(cc::utils::plugin_identifier::setting_source_to_scope(cc::utils::settings_sources::SettingSource::LocalSettings), cc::utils::plugin_identifier::PluginScope::Local);
}

TEST(PluginDependencyResolver, ResolvesClosureAndReportsDependencyErrors) {
    using cc::utils::plugin_dependency_resolver::DependencyLookupResult;
    using cc::utils::plugin_dependency_resolver::LoadedPlugin;

    const std::map<std::string, DependencyLookupResult> graph = {
        {"app@main", DependencyLookupResult{{"core", "theme@main"}}},
        {"core@main", DependencyLookupResult{{"shared@main"}}},
        {"theme@main", DependencyLookupResult{{}}},
        {"shared@main", DependencyLookupResult{{}}},
    };

    auto result = cc::utils::plugin_dependency_resolver::resolve_dependency_closure(
        "app@main",
        [&](const std::string& id) -> std::optional<DependencyLookupResult> {
            auto it = graph.find(id);
            if (it == graph.end()) return std::nullopt;
            return it->second;
        },
        {"theme@main"}
    );

    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(result.closure, (std::vector<std::string>{"shared@main", "core@main", "app@main"}));

    auto cross = cc::utils::plugin_dependency_resolver::resolve_dependency_closure(
        "app@main",
        [&](const std::string& id) -> std::optional<DependencyLookupResult> {
            if (id == "app@main") return DependencyLookupResult{{"dep@other"}};
            if (id == "dep@other") return DependencyLookupResult{{}};
            return std::nullopt;
        }
    );
    EXPECT_FALSE(cross.ok);
    EXPECT_EQ(cross.reason, cc::utils::plugin_dependency_resolver::ResolutionFailure::CrossMarketplace);

    auto cycle = cc::utils::plugin_dependency_resolver::resolve_dependency_closure(
        "a@main",
        [&](const std::string& id) -> std::optional<DependencyLookupResult> {
            if (id == "a@main") return DependencyLookupResult{{"b"}};
            if (id == "b@main") return DependencyLookupResult{{"a"}};
            return std::nullopt;
        }
    );
    EXPECT_FALSE(cycle.ok);
    EXPECT_EQ(cycle.reason, cc::utils::plugin_dependency_resolver::ResolutionFailure::Cycle);

    std::vector<LoadedPlugin> plugins = {
        {.name = "a", .source = "a@main", .enabled = true, .dependencies = {"b@main"}},
        {.name = "b", .source = "b@main", .enabled = false, .dependencies = {}},
        {.name = "c", .source = "c@main", .enabled = true, .dependencies = {"a@main"}},
    };
    auto demotion = cc::utils::plugin_dependency_resolver::verify_and_demote(plugins);
    EXPECT_EQ(demotion.demoted, (std::set<std::string>{"a@main", "c@main"}));
    ASSERT_EQ(demotion.errors.size(), 2u);
    EXPECT_EQ(demotion.errors[0].dependency, "b@main");
    EXPECT_EQ(demotion.errors[0].reason, "not-enabled");
    EXPECT_EQ(cc::utils::plugin_dependency_resolver::find_reverse_dependents("a@main", plugins), (std::vector<std::string>{"c"}));
    EXPECT_EQ(cc::utils::plugin_dependency_resolver::format_dependency_count_suffix({"a"}), " (+ 1 dependency)");
    EXPECT_EQ(cc::utils::plugin_dependency_resolver::format_dependency_count_suffix({"a", "b"}), " (+ 2 dependencies)");
    EXPECT_EQ(cc::utils::plugin_dependency_resolver::format_reverse_dependents_suffix({"a", "b"}), " — warning: required by a, b");
}

TEST(SettingsPathsAndMerge, ComputesManagedAndRelativePathsAndDedupesArrays) {
    using cc::utils::settings_sources::SettingSource;

    EXPECT_EQ(cc::utils::settings_paths::managed_file_path(cc::utils::settings_paths::Platform::MacOS), "/Library/Application Support/ClaudeCode");
    EXPECT_EQ(cc::utils::settings_paths::managed_file_path(cc::utils::settings_paths::Platform::Windows), "C:\\Program Files\\ClaudeCode");
    EXPECT_EQ(cc::utils::settings_paths::managed_file_path(cc::utils::settings_paths::Platform::Linux), "/etc/claude-code");
    EXPECT_EQ(cc::utils::settings_paths::managed_file_path(cc::utils::settings_paths::Platform::Linux, "ant", "/tmp/managed"), "/tmp/managed");
    EXPECT_EQ(cc::utils::settings_paths::managed_settings_drop_in_dir("/etc/claude-code"), "/etc/claude-code/managed-settings.d");
    EXPECT_EQ(cc::utils::settings_paths::relative_settings_file_path_for_source(SettingSource::ProjectSettings), ".claude/settings.json");
    EXPECT_EQ(cc::utils::settings_paths::relative_settings_file_path_for_source(SettingSource::LocalSettings), ".claude/settings.local.json");

    EXPECT_EQ(
        cc::utils::settings_merge::merge_arrays_unique({"Bash(ls:*)", "Read(*)"}, {"Read(*)", "Edit(src:*)"}),
        (std::vector<std::string>{"Bash(ls:*)", "Read(*)", "Edit(src:*)"})
    );
}

TEST(PluginMarketplaceRules, AppliesOfficialNameAndAutoUpdateRules) {
    using cc::utils::plugin_marketplace_rules::MarketplaceSource;
    using cc::utils::plugin_marketplace_rules::MarketplaceSourceType;

    EXPECT_TRUE(cc::utils::plugin_marketplace_rules::is_marketplace_auto_update("anthropic-marketplace", std::nullopt));
    EXPECT_FALSE(cc::utils::plugin_marketplace_rules::is_marketplace_auto_update("knowledge-work-plugins", std::nullopt));
    EXPECT_TRUE(cc::utils::plugin_marketplace_rules::is_marketplace_auto_update("third-party", true));
    EXPECT_FALSE(cc::utils::plugin_marketplace_rules::is_marketplace_auto_update("anthropic-marketplace", false));

    EXPECT_FALSE(cc::utils::plugin_marketplace_rules::is_blocked_official_name("anthropic-marketplace"));
    EXPECT_TRUE(cc::utils::plugin_marketplace_rules::is_blocked_official_name("claude-official"));
    EXPECT_TRUE(cc::utils::plugin_marketplace_rules::is_blocked_official_name("anthropic-marketplace-new"));
    EXPECT_TRUE(cc::utils::plugin_marketplace_rules::is_blocked_official_name("clаude")); // contains Cyrillic a

    EXPECT_FALSE(cc::utils::plugin_marketplace_rules::validate_official_name_source(
        "anthropic-marketplace",
        MarketplaceSource{.type = MarketplaceSourceType::Github, .repo = "anthropics/plugins", .url = ""}
    ).has_value());
    auto invalid = cc::utils::plugin_marketplace_rules::validate_official_name_source(
        "anthropic-marketplace",
        MarketplaceSource{.type = MarketplaceSourceType::Git, .repo = "", .url = "https://github.com/other/plugins.git"}
    );
    ASSERT_TRUE(invalid.has_value());
    EXPECT_NE(invalid->find("reserved for official Anthropic marketplaces"), std::string::npos);
}

TEST(PluginVersioning, ExtractsVersionedPathsAndDerivesPureVersions) {
    EXPECT_EQ(cc::utils::plugin_versioning::get_version_from_path("/Users/me/.claude/plugins/cache/main/plugin/1.2.3"), "1.2.3");
    EXPECT_FALSE(cc::utils::plugin_versioning::get_version_from_path("/Users/me/.claude/plugins/main/plugin").has_value());
    EXPECT_TRUE(cc::utils::plugin_versioning::is_versioned_path("/plugins/cache/main/plugin/v1"));
    EXPECT_FALSE(cc::utils::plugin_versioning::is_versioned_path("/plugins/main/plugin/v1"));

    EXPECT_EQ(cc::utils::plugin_versioning::derive_plugin_version("1.0.0", "2.0.0", "abcdef1234567890"), "1.0.0");
    EXPECT_EQ(cc::utils::plugin_versioning::derive_plugin_version(std::nullopt, "2.0.0", "abcdef1234567890"), "2.0.0");
    EXPECT_EQ(cc::utils::plugin_versioning::derive_plugin_version(std::nullopt, std::nullopt, "abcdef1234567890"), "abcdef123456");
    EXPECT_EQ(
        cc::utils::plugin_versioning::derive_plugin_version(std::nullopt, std::nullopt, "abcdef1234567890", "git-subdir", R"(.\a\)"),
        "abcdef123456-ca978112"
    );
    EXPECT_EQ(cc::utils::plugin_versioning::derive_plugin_version(std::nullopt, std::nullopt, std::nullopt), "unknown");
}

TEST(ArgumentSubstitution, ParsesNamesHintsAndSubstitutesPlaceholders) {
    using namespace cc::utils::argument_substitution;

    EXPECT_TRUE(parse_arguments("  \t ").empty());
    EXPECT_EQ(parse_arguments(R"(foo "hello world" 'again there' $FOO)"), (std::vector<std::string>{"foo", "hello world", "again there", "$FOO"}));
    EXPECT_EQ(parse_arguments(R"(foo "unterminated)"), (std::vector<std::string>{"foo", "\"unterminated"}));

    EXPECT_EQ(parse_argument_names(std::optional<std::string>{"foo bar 123  baz"}), (std::vector<std::string>{"foo", "bar", "baz"}));
    EXPECT_EQ(parse_argument_names(std::vector<std::string>{"foo", "", "42", "bar"}), (std::vector<std::string>{"foo", "bar"}));
    EXPECT_EQ(generate_progressive_argument_hint({"arg1", "arg2", "arg3"}, {"value1"}), "[arg2] [arg3]");
    EXPECT_FALSE(generate_progressive_argument_hint({"arg1"}, {"value1"}).has_value());

    EXPECT_EQ(
        substitute_arguments("all=$ARGUMENTS first=$ARGUMENTS[0] second=$1 named=$foo missing=$2", "alpha beta", true, {"foo"}),
        "all=alpha beta first=alpha second=beta named=alpha missing="
    );
    EXPECT_EQ(substitute_arguments("unchanged", std::nullopt), "unchanged");
    EXPECT_EQ(substitute_arguments("prefix", "x y", true), "prefix\n\nARGUMENTS: x y");
    EXPECT_EQ(substitute_arguments("$foo $foobar $foo[0]", "one two", true, {"foo"}), "one $foobar $foo[0]");
}

TEST(SemanticInputCoercion, CoercesOnlyExplicitBooleanAndDecimalStringLiterals) {
    using cc::utils::semantic_boolean::coerce_semantic_boolean;
    using cc::utils::semantic_number::coerce_semantic_number;

    EXPECT_EQ(coerce_semantic_boolean("true"), true);
    EXPECT_EQ(coerce_semantic_boolean("false"), false);
    EXPECT_FALSE(coerce_semantic_boolean("False").has_value());
    EXPECT_FALSE(coerce_semantic_boolean("0").has_value());

    ASSERT_TRUE(coerce_semantic_number("30").has_value());
    EXPECT_DOUBLE_EQ(*coerce_semantic_number("30"), 30.0);
    EXPECT_DOUBLE_EQ(*coerce_semantic_number("-5"), -5.0);
    EXPECT_DOUBLE_EQ(*coerce_semantic_number("3.14"), 3.14);
    EXPECT_FALSE(coerce_semantic_number("").has_value());
    EXPECT_FALSE(coerce_semantic_number("1.").has_value());
    EXPECT_FALSE(coerce_semantic_number(".5").has_value());
    EXPECT_FALSE(coerce_semantic_number("1e3").has_value());
}

TEST(QueryGuard, EnforcesDispatchingRunningGenerationTransitions) {
    cc::utils::query_guard::QueryGuard guard;
    int notifications = 0;
    auto unsubscribe = guard.subscribe([&] { ++notifications; });

    EXPECT_FALSE(guard.is_active());
    EXPECT_EQ(guard.generation(), 0);
    EXPECT_TRUE(guard.reserve());
    EXPECT_TRUE(guard.is_active());
    EXPECT_FALSE(guard.reserve());

    auto first = guard.try_start();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);
    EXPECT_FALSE(guard.try_start().has_value());
    EXPECT_FALSE(guard.end(0));
    EXPECT_TRUE(guard.end(*first));
    EXPECT_FALSE(guard.is_active());

    EXPECT_TRUE(guard.reserve());
    guard.cancel_reservation();
    EXPECT_FALSE(guard.is_active());

    auto second = guard.try_start();
    ASSERT_TRUE(second.has_value());
    guard.force_end();
    EXPECT_EQ(guard.generation(), *second + 1);
    EXPECT_FALSE(guard.end(*second));

    unsubscribe();
    EXPECT_TRUE(guard.reserve());
    EXPECT_EQ(notifications, 7);
}

TEST(CollapseNotifications, MergesAdjacentHookSummariesByLabel) {
    using namespace cc::utils::collapse_notifications;

    std::vector<HookSummaryMessage> messages = {
        {.hook_label = "PostToolUse", .hook_count = 1, .hook_infos = {"a"}, .hook_errors = {}, .prevented_continuation = false, .has_output = false, .total_duration_ms = 10},
        {.hook_label = "PostToolUse", .hook_count = 2, .hook_infos = {"b", "c"}, .hook_errors = {"err"}, .prevented_continuation = true, .has_output = true, .total_duration_ms = 25},
        {.hook_label = "Stop", .hook_count = 1, .hook_infos = {"d"}, .hook_errors = {}, .prevented_continuation = false, .has_output = false, .total_duration_ms = 5},
    };

    auto collapsed = collapse_hook_summaries(messages);
    ASSERT_EQ(collapsed.size(), 2u);
    EXPECT_EQ(collapsed[0].hook_label, "PostToolUse");
    EXPECT_EQ(collapsed[0].hook_count, 3);
    EXPECT_EQ(collapsed[0].hook_infos, (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(collapsed[0].hook_errors, (std::vector<std::string>{"err"}));
    EXPECT_TRUE(collapsed[0].prevented_continuation);
    EXPECT_TRUE(collapsed[0].has_output);
    EXPECT_EQ(collapsed[0].total_duration_ms, 25);
    EXPECT_EQ(collapsed[1].hook_label, "Stop");
}

TEST(CollapseNotifications, MergesTeammateShutdownRunsAndBackgroundBashCompletions) {
    using namespace cc::utils::collapse_notifications;

    std::vector<TeammateShutdownMessage> teammate_messages = {
        {.uuid = "u1", .timestamp_ms = 100, .is_shutdown = true},
        {.uuid = "u2", .timestamp_ms = 101, .is_shutdown = true},
        {.uuid = "u3", .timestamp_ms = 102, .is_shutdown = false},
        {.uuid = "u4", .timestamp_ms = 103, .is_shutdown = true},
    };
    auto teammates = collapse_teammate_shutdowns(teammate_messages);
    ASSERT_EQ(teammates.size(), 3u);
    EXPECT_TRUE(teammates[0].is_batch);
    EXPECT_EQ(teammates[0].count, 2);
    EXPECT_EQ(teammates[0].uuid, "u1");
    EXPECT_FALSE(teammates[1].is_shutdown);
    EXPECT_FALSE(teammates[2].is_batch);

    const std::string bash1 = "<task-notification><status>completed</status><summary>Background command \"ls\" completed</summary></task-notification>";
    const std::string bash2 = "<task-notification><summary>Background command \"pwd\" completed</summary><status>completed</status></task-notification>";
    const std::string failed = "<task-notification><status>failed</status><summary>Background command \"bad\" failed</summary></task-notification>";
    auto collapsed = collapse_background_bash_notifications({bash1, bash2, failed}, true, false);
    ASSERT_EQ(collapsed.size(), 2u);
    EXPECT_EQ(collapsed[0], "<task-notification><status>completed</status><summary>2 background commands completed</summary></task-notification>");
    EXPECT_EQ(collapsed[1], failed);
    EXPECT_EQ(collapse_background_bash_notifications({bash1, bash2}, true, true).size(), 2u);
    EXPECT_EQ(collapse_background_bash_notifications({bash1, bash2}, false, false).size(), 2u);
}

TEST(AgentId, FormatsAndParsesAgentAndRequestIds) {
    using namespace cc::utils::agent_id;

    EXPECT_EQ(format_agent_id("researcher", "my-project"), "researcher@my-project");
    auto parsed = parse_agent_id("researcher@my-project@nested");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->agent_name, "researcher");
    EXPECT_EQ(parsed->team_name, "my-project@nested");
    EXPECT_FALSE(parse_agent_id("missing-separator").has_value());

    EXPECT_EQ(generate_request_id("shutdown", "researcher@team", 1702500000000LL), "shutdown-1702500000000@researcher@team");
    auto request = parse_request_id("plan-approval-1702500000000@researcher@team");
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->request_type, "plan-approval");
    EXPECT_EQ(request->timestamp, 1702500000000LL);
    EXPECT_EQ(request->agent_id, "researcher@team");
    EXPECT_FALSE(parse_request_id("shutdown-nope@agent@team").has_value());
    EXPECT_FALSE(parse_request_id("missing-at").has_value());
}

TEST(AutoModeDenials, RespectsFeatureFlagAndKeepsMostRecentTwenty) {
    using namespace cc::utils::auto_mode_denials;

    AutoModeDenialStore disabled;
    disabled.record({.tool_name = "Bash", .display = "rm -rf /", .reason = "danger", .timestamp = 1}, false);
    EXPECT_TRUE(disabled.get().empty());

    AutoModeDenialStore store;
    for (int i = 0; i < 25; ++i) {
        store.record({.tool_name = "Bash", .display = "cmd" + std::to_string(i), .reason = "r", .timestamp = i}, true);
    }
    auto denials = store.get();
    ASSERT_EQ(denials.size(), 20u);
    EXPECT_EQ(denials.front().display, "cmd24");
    EXPECT_EQ(denials.back().display, "cmd5");
}

TEST(ActivityManager, DeduplicatesCliActivityAndRecordsUserWithinTimeout) {
    using namespace cc::utils::activity_manager;

    long long now = 1000;
    std::vector<ActiveTimeRecord> records;
    ActivityManager manager([&] { return now; }, [&](double seconds, ActivityType type) {
        records.push_back({seconds, type});
    });

    manager.record_user_activity();
    now += 3000;
    manager.record_user_activity();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_DOUBLE_EQ(records[0].seconds, 3.0);
    EXPECT_EQ(records[0].type, ActivityType::User);

    manager.start_cli_activity("tool");
    now += 2000;
    manager.start_cli_activity("tool");
    ASSERT_EQ(records.size(), 2u);
    EXPECT_DOUBLE_EQ(records[1].seconds, 2.0);
    EXPECT_EQ(records[1].type, ActivityType::Cli);

    now += 4000;
    manager.end_cli_activity("tool");
    ASSERT_EQ(records.size(), 3u);
    EXPECT_DOUBLE_EQ(records[2].seconds, 4.0);
    EXPECT_EQ(records[2].type, ActivityType::Cli);

    auto state = manager.get_activity_states();
    EXPECT_FALSE(state.is_user_active);
    EXPECT_FALSE(state.is_cli_active);
    EXPECT_EQ(state.active_operation_count, 0u);

    manager.record_user_activity();
    EXPECT_TRUE(manager.get_activity_states().is_user_active);
    now += 6000;
    EXPECT_FALSE(manager.get_activity_states().is_user_active);
}

TEST(AgentSwarmsEnabled, AppliesAntOverrideOptInAndKillswitch) {
    using namespace cc::utils::agent_swarms_enabled;

    EXPECT_TRUE(is_agent_swarms_enabled({.user_type = "ant", .env_opt_in = false, .flag_set = false, .growthbook_enabled = false}));
    EXPECT_FALSE(is_agent_swarms_enabled({.user_type = "external", .env_opt_in = false, .flag_set = false, .growthbook_enabled = true}));
    EXPECT_TRUE(is_agent_swarms_enabled({.user_type = "external", .env_opt_in = true, .flag_set = false, .growthbook_enabled = true}));
    EXPECT_TRUE(is_agent_swarms_enabled({.user_type = "external", .env_opt_in = false, .flag_set = true, .growthbook_enabled = true}));
    EXPECT_FALSE(is_agent_swarms_enabled({.user_type = "external", .env_opt_in = true, .flag_set = false, .growthbook_enabled = false}));
    EXPECT_TRUE(is_env_truthy(" YES "));
    EXPECT_FALSE(is_env_truthy("off"));
}

TEST(EnvUtilsCompat, ParsesTruthyFalsyOptionsAndEnvVars) {
    EXPECT_TRUE(cc::utils::is_env_truthy(" on "));
    EXPECT_TRUE(cc::utils::is_env_truthy("TRUE"));
    EXPECT_FALSE(cc::utils::is_env_truthy("0"));
    EXPECT_TRUE(cc::utils::is_env_defined_falsy(" no "));
    EXPECT_TRUE(cc::utils::is_env_defined_falsy(false));
    EXPECT_FALSE(cc::utils::is_env_defined_falsy(std::nullopt));

    EXPECT_TRUE(cc::utils::has_node_option("--max-old-space-size=4096 --trace-warnings", "--trace-warnings"));
    EXPECT_FALSE(cc::utils::has_node_option("--trace-warnings-extra", "--trace-warnings"));
    EXPECT_TRUE(cc::utils::is_bare_mode("0", {"cc", "--bare"}));
    EXPECT_TRUE(cc::utils::is_bare_mode("true", {"cc"}));
    EXPECT_FALSE(cc::utils::is_bare_mode("", {"cc"}));

    auto parsed = cc::utils::parse_env_vars({"A=1", "B=two=parts"});
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->at("A"), "1");
    EXPECT_EQ(parsed->at("B"), "two=parts");
    EXPECT_FALSE(cc::utils::parse_env_vars({"NO_EQUALS"}).has_value());

    EXPECT_EQ(cc::utils::resolve_aws_region("eu-west-1", "us-west-2"), "eu-west-1");
    EXPECT_EQ(cc::utils::resolve_aws_region(std::nullopt, "us-west-2"), "us-west-2");
    EXPECT_EQ(cc::utils::resolve_aws_region(std::nullopt, std::nullopt), "us-east-1");
    EXPECT_EQ(cc::utils::resolve_default_vertex_region(std::nullopt), "us-east5");
    EXPECT_EQ(cc::utils::resolve_default_vertex_region("asia-northeast1"), "asia-northeast1");
}

TEST(CachePaths, SanitizesStableProjectAndMcpLogPaths) {
    using namespace cc::utils::cache_paths;

    EXPECT_EQ(sanitize_path("/Users/me/project:alpha"), "-Users-me-project-alpha");
    EXPECT_EQ(project_dir("/tmp/work repo"), "-tmp-work-repo");
    std::string long_name(205, 'a');
    auto sanitized = sanitize_path(long_name);
    EXPECT_GT(sanitized.size(), 200u);
    EXPECT_EQ(sanitized.substr(0, 200), std::string(200, 'a'));
    EXPECT_NE(sanitized.find('-'), std::string::npos);

    CachePathSet paths = build_cache_paths("/var/cache/claude-cli", "/Users/me/project", "server:name");
    EXPECT_EQ(paths.base_logs, "/var/cache/claude-cli/-Users-me-project");
    EXPECT_EQ(paths.errors, "/var/cache/claude-cli/-Users-me-project/errors");
    EXPECT_EQ(paths.messages, "/var/cache/claude-cli/-Users-me-project/messages");
    EXPECT_EQ(paths.mcp_logs, "/var/cache/claude-cli/-Users-me-project/mcp-logs-server-name");
}

TEST(BinaryCheck, TrimsCommandsCachesResultsAndClearsCache) {
    cc::utils::binary_check::BinaryChecker checker;
    int calls = 0;
    auto resolver = [&](std::string_view command) {
        ++calls;
        return command == "git";
    };

    EXPECT_FALSE(checker.is_binary_installed("   ", resolver));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(checker.is_binary_installed(" git ", resolver));
    EXPECT_TRUE(checker.is_binary_installed("git", resolver));
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(checker.is_binary_installed("missing", resolver));
    EXPECT_FALSE(checker.is_binary_installed("missing", resolver));
    EXPECT_EQ(calls, 2);
    checker.clear();
    EXPECT_TRUE(checker.is_binary_installed("git", resolver));
    EXPECT_EQ(calls, 3);
}

TEST(ClaudeCodeHints, ExtractsWholeLineHintsStripsThemAndKeepsSourceCommand) {
    using namespace cc::utils::claude_code_hints;

    const std::string output =
        "before\n"
        "  <claude-code-hint v=1 type=plugin value=eslint@marketplace />\n"
        "quoted <claude-code-hint v=1 type=plugin value=ignored /> text\n"
        "\t<claude-code-hint v=2 type=plugin value=future@marketplace />\n"
        "after";

    auto result = extract_claude_code_hints(output, "  npx eslint .");

    ASSERT_EQ(result.hints.size(), 1u);
    EXPECT_EQ(result.hints[0].v, 1);
    EXPECT_EQ(result.hints[0].type, "plugin");
    EXPECT_EQ(result.hints[0].value, "eslint@marketplace");
    EXPECT_EQ(result.hints[0].source_command, "npx");
    EXPECT_EQ(result.stripped, "before\n\nquoted <claude-code-hint v=1 type=plugin value=ignored /> text\n\nafter");

    auto no_hint = extract_claude_code_hints("plain output", "cmd");
    EXPECT_TRUE(no_hint.hints.empty());
    EXPECT_EQ(no_hint.stripped, "plain output");

    auto huge_version = extract_claude_code_hints(
        "<claude-code-hint v=999999999999999999999999999999 type=plugin value=x@marketplace />",
        "tool");
    EXPECT_TRUE(huge_version.hints.empty());
    EXPECT_EQ(huge_version.stripped, "");
}

TEST(ClaudeCodeHints, PendingHintStoreIsSingleSlotAndOncePerSession) {
    using namespace cc::utils::claude_code_hints;

    PendingHintStore store;
    int notifications = 0;
    auto unsubscribe = store.subscribe([&] { ++notifications; });

    store.set_pending_hint({.v = 1, .type = "plugin", .value = "a@marketplace", .source_command = "a"});
    ASSERT_TRUE(store.get_pending_hint_snapshot().has_value());
    EXPECT_EQ(store.get_pending_hint_snapshot()->value, "a@marketplace");
    store.set_pending_hint({.v = 1, .type = "plugin", .value = "b@marketplace", .source_command = "b"});
    EXPECT_EQ(store.get_pending_hint_snapshot()->value, "b@marketplace");
    store.clear_pending_hint();
    EXPECT_FALSE(store.get_pending_hint_snapshot().has_value());
    store.mark_shown_this_session();
    store.set_pending_hint({.v = 1, .type = "plugin", .value = "c@marketplace", .source_command = "c"});
    EXPECT_FALSE(store.get_pending_hint_snapshot().has_value());
    EXPECT_TRUE(store.has_shown_hint_this_session());
    unsubscribe();
    store.clear_pending_hint();
    EXPECT_EQ(notifications, 3);
}

TEST(CommitAttribution, SanitizesInternalModelNamesAndSurfaceKeys) {
    using namespace cc::utils::commit_attribution;

    EXPECT_EQ(sanitize_model_name("claude-opus-4-6-fast"), "claude-opus-4-6");
    EXPECT_EQ(sanitize_model_name("internal-sonnet-4-5-thinking"), "claude-sonnet-4-5");
    EXPECT_EQ(sanitize_model_name("haiku-3-5-test"), "claude-haiku-3-5");
    EXPECT_EQ(sanitize_model_name("unknown-codename"), "claude");
    EXPECT_EQ(sanitize_surface_key("cli/opus-4-5-fast"), "cli/claude-opus-4-5");
    EXPECT_EQ(sanitize_surface_key("cli"), "cli");
}

TEST(CommitAttribution, TracksChangedRegionCreationDeletionAndBulkChanges) {
    using namespace cc::utils::commit_attribution;

    auto state = create_empty_attribution_state("cli/claude-sonnet-4-5");
    EXPECT_EQ(state.surface, "cli/claude-sonnet-4-5");

    state = track_file_modification(state, "src/a.ts", "hello world", "hello brave world", 10.0);
    ASSERT_TRUE(state.file_states.contains("src/a.ts"));
    EXPECT_EQ(state.file_states.at("src/a.ts").claude_contribution, 6u);
    EXPECT_EQ(state.file_states.at("src/a.ts").content_hash,
              "169f95520526c347f9ef612918879703d7b5340d162efd0880ccad0be7e17673");
    EXPECT_DOUBLE_EQ(state.file_states.at("src/a.ts").mtime, 10.0);

    state = track_file_creation(state, "src/new.ts", "abcdef", 11.0);
    EXPECT_EQ(state.file_states.at("src/new.ts").claude_contribution, 6u);

    state = track_file_deletion(state, "src/a.ts", "hello brave world", 12.0);
    EXPECT_EQ(state.file_states.at("src/a.ts").claude_contribution, 23u);
    EXPECT_EQ(state.file_states.at("src/a.ts").content_hash, "");

    std::vector<FileChange> changes = {
        {.path = "src/new.ts", .type = FileChangeType::Modified, .old_content = "abcdef", .new_content = "abcXYZdef", .mtime = 13.0},
        {.path = "src/old.ts", .type = FileChangeType::Deleted, .old_content = "gone", .new_content = "", .mtime = 14.0},
    };
    state = track_bulk_file_changes(state, changes);
    EXPECT_EQ(state.file_states.at("src/new.ts").claude_contribution, 9u);
    EXPECT_EQ(state.file_states.at("src/old.ts").claude_contribution, 4u);

    auto unicode = create_empty_attribution_state();
    unicode = track_file_creation(unicode, "emoji.txt", "\xF0\x9F\x98\x80", 15.0);
    unicode = track_file_creation(unicode, "cjk.txt", "\xE4\xBD\xA0", 16.0);
    EXPECT_EQ(unicode.file_states.at("emoji.txt").claude_contribution, 2u);
    EXPECT_EQ(unicode.file_states.at("cjk.txt").claude_contribution, 1u);
}

TEST(HashUtils, MatchesTypeScriptDjb2Sha256AndPairHashing) {
    using namespace cc::utils::hash;

    EXPECT_EQ(djb2_hash(""), 0);
    EXPECT_EQ(djb2_hash("hello"), 99162322);
    EXPECT_EQ(djb2_hash("CC-REPL"), 1295427772);
    EXPECT_EQ(djb2_hash("\xF0\x9F\x98\x80"), 1772899);

    EXPECT_EQ(hash_content("hello"), "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    EXPECT_EQ(hash_pair("ts", "code"), "8aea308fb5b062d3ab47d67952ee9d76beab21da1610bf7fbcc97e728e84c226");
    EXPECT_EQ(hash_pair("tsc", "ode"), "2dd84b1bcba070e82fd5d6278dcc35424d1d6d3cc8d87fd3b12486e8dcfe5749");
    EXPECT_NE(hash_pair("ts", "code"), hash_pair("tsc", "ode"));
}

TEST(TaggedId, EncodesUuidAsApiCompatibleBase58TaggedId) {
    using namespace cc::utils::tagged_id;

    auto zero = to_tagged_id("user", "00000000-0000-0000-0000-000000000000");
    ASSERT_TRUE(zero.has_value()) << zero.error();
    EXPECT_EQ(*zero, "user_011111111111111111111111");

    auto max = to_tagged_id("user", "ffffffff-ffff-ffff-ffff-ffffffffffff");
    ASSERT_TRUE(max.has_value()) << max.error();
    EXPECT_EQ(*max, "user_01YcVfxkQb6JRzqk5kF2tNLv");

    auto with_hyphens = to_tagged_id("org", "123e4567-e89b-12d3-a456-426614174000");
    auto without_hyphens = to_tagged_id("org", "123e4567e89b12d3a456426614174000");
    ASSERT_TRUE(with_hyphens.has_value()) << with_hyphens.error();
    ASSERT_TRUE(without_hyphens.has_value()) << without_hyphens.error();
    EXPECT_EQ(*with_hyphens, "org_013FfGK34vwMvVFDedyb2nkf");
    EXPECT_EQ(*without_hyphens, *with_hyphens);

    EXPECT_FALSE(to_tagged_id("user", "not-a-uuid").has_value());
    EXPECT_FALSE(to_tagged_id("user", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz").has_value());
}

TEST(MessagePredicates, HumanTurnsExcludeMetaAndToolResults) {
    using namespace cc::utils::message_predicates;

    EXPECT_TRUE(is_human_turn({.type = "user", .is_meta = false, .has_tool_use_result = false}));
    EXPECT_FALSE(is_human_turn({.type = "assistant", .is_meta = false, .has_tool_use_result = false}));
    EXPECT_FALSE(is_human_turn({.type = "user", .is_meta = true, .has_tool_use_result = false}));
    EXPECT_FALSE(is_human_turn({.type = "user", .is_meta = false, .has_tool_use_result = true}));
}

TEST(ContentArray, InsertsBlockAfterToolResultsOrBeforeLastBlock) {
    using namespace cc::utils::content_array;

    std::vector<ContentBlock> with_results = {
        {.type = "text", .text = "start"},
        {.type = "tool_result", .text = "r1"},
        {.type = "tool_result", .text = "r2"},
    };
    insert_block_after_tool_results(with_results, {.type = "cache", .text = "directive"});
    ASSERT_EQ(with_results.size(), 5u);
    EXPECT_EQ(with_results[2].type, "tool_result");
    EXPECT_EQ(with_results[3].type, "cache");
    EXPECT_EQ(with_results[4].type, "text");
    EXPECT_EQ(with_results[4].text, ".");

    std::vector<ContentBlock> mixed = {
        {.type = "tool_result", .text = "r"},
        {.type = "text", .text = "tail"},
    };
    insert_block_after_tool_results(mixed, {.type = "cache", .text = "directive"});
    ASSERT_EQ(mixed.size(), 3u);
    EXPECT_EQ(mixed[1].type, "cache");
    EXPECT_EQ(mixed[2].text, "tail");

    std::vector<ContentBlock> no_results = {{.type = "text", .text = "a"}, {.type = "text", .text = "b"}};
    insert_block_after_tool_results(no_results, {.type = "cache", .text = "directive"});
    ASSERT_EQ(no_results.size(), 3u);
    EXPECT_EQ(no_results[1].type, "cache");
    EXPECT_EQ(no_results[2].text, "b");
}

TEST(ObjectGroupBy, GroupsItemsBySelectorAndPassesIndex) {
    using namespace cc::utils::object_group_by;

    std::vector<std::string> items = {"apple", "ape", "banana", "berry"};
    auto grouped = object_group_by<std::string, std::string>(items, [](const std::string& item, std::size_t index) {
        return std::string(1, item[0]) + std::to_string(index % 2);
    });

    EXPECT_EQ(grouped["a0"], (std::vector<std::string>{"apple"}));
    EXPECT_EQ(grouped["a1"], (std::vector<std::string>{"ape"}));
    EXPECT_EQ(grouped["b0"], (std::vector<std::string>{"banana"}));
    EXPECT_EQ(grouped["b1"], (std::vector<std::string>{"berry"}));
}

TEST(Timeouts, ParsesDefaultAndMaxBashTimeoutsLikeTypeScript) {
    using namespace cc::utils::timeouts;

    EXPECT_EQ(get_default_bash_timeout_ms({}), 120000);
    EXPECT_EQ(get_default_bash_timeout_ms({{"BASH_DEFAULT_TIMEOUT_MS", "3000"}}), 3000);
    EXPECT_EQ(get_default_bash_timeout_ms({{"BASH_DEFAULT_TIMEOUT_MS", "0"}}), 120000);
    EXPECT_EQ(get_default_bash_timeout_ms({{"BASH_DEFAULT_TIMEOUT_MS", "abc"}}), 120000);
    EXPECT_EQ(get_default_bash_timeout_ms({{"BASH_DEFAULT_TIMEOUT_MS", "42abc"}}), 42);

    EXPECT_EQ(get_max_bash_timeout_ms({}), 600000);
    EXPECT_EQ(get_max_bash_timeout_ms({{"BASH_MAX_TIMEOUT_MS", "5000"}}), 120000);
    EXPECT_EQ(get_max_bash_timeout_ms({{"BASH_DEFAULT_TIMEOUT_MS", "200000"}, {"BASH_MAX_TIMEOUT_MS", "5000"}}), 200000);
    EXPECT_EQ(get_max_bash_timeout_ms({{"BASH_MAX_TIMEOUT_MS", "900000"}}), 900000);
    EXPECT_EQ(get_max_bash_timeout_ms({{"BASH_DEFAULT_TIMEOUT_MS", "700000"}}), 700000);
}

TEST(SlashCommandParsing, ParsesRegularAndMcpSlashCommands) {
    using namespace cc::utils::slash_command_parsing;

    auto regular = parse_slash_command("  /search foo bar  ");
    ASSERT_TRUE(regular.has_value());
    EXPECT_EQ(regular->command_name, "search");
    EXPECT_EQ(regular->args, "foo bar");
    EXPECT_FALSE(regular->is_mcp);

    auto mcp = parse_slash_command("/mcp:tool (MCP) arg1  arg2");
    ASSERT_TRUE(mcp.has_value());
    EXPECT_EQ(mcp->command_name, "mcp:tool (MCP)");
    EXPECT_EQ(mcp->args, "arg1  arg2");
    EXPECT_TRUE(mcp->is_mcp);

    EXPECT_FALSE(parse_slash_command("search foo").has_value());
    EXPECT_FALSE(parse_slash_command("/").has_value());
    auto spaced = parse_slash_command("/cmd  two-spaces");
    ASSERT_TRUE(spaced.has_value());
    EXPECT_EQ(spaced->args, " two-spaces");
}
