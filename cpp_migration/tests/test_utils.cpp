/// @file test_utils.cpp



#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <httplib.h>

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
import cc.utils.plugin_loader;
import cc.utils.argument_substitution;
import cc.utils.semantic_boolean;
import cc.utils.semantic_number;
import cc.ui.terminal_io;
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
import cc.utils.diff_utils;
import cc.utils.shell_providers;
import cc.utils.git_diff;
import cc.utils.proxy_utils;
import cc.utils.github_utils;
import cc.plugins.marketplace;
import core.memdir;
import core.screens;

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name) : name_(name) {
        if (const char* value = std::getenv(name)) {
            previous_ = std::string(value);
        }
    }

    ~ScopedEnvVar() {
        if (previous_) {
            setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    void set(const char* value) const {
        setenv(name_.c_str(), value, 1);
    }

    void unset() const {
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class CurrentPathGuard {
public:
    explicit CurrentPathGuard(const std::filesystem::path& next) : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }

    ~CurrentPathGuard() {
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
    }

private:
    std::filesystem::path previous_;
};

std::string shell_quote_for_test(const std::filesystem::path& path) {
    const auto value = path.string();
    if (value.empty()) return "''";
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

bool command_available_for_test(std::string_view command) {
    const auto check = "command -v " + std::string(command) + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}

void run_shell_ok_for_test(const std::string& command) {
    ASSERT_EQ(std::system(command.c_str()), 0) << command;
}

class LocalGitHubApiServer {
public:
    std::atomic<int> user_requests{0};
    std::atomic<int> issue_comment_requests{0};
    std::atomic<int> review_comment_requests{0};
    std::atomic<int> user_status{200};
    std::atomic<bool> paginate_comments{false};

    LocalGitHubApiServer() {
        server_.Get("/user", [&](const httplib::Request& req, httplib::Response& res) {
            ++user_requests;
            capture_headers(req);
            if (user_status.load() != 200) {
                res.status = user_status.load();
                res.set_content(R"({"message":"auth failed"})", "application/json");
                return;
            }
            res.set_content(R"({
              "login": "octocat",
              "name": "The Octocat",
              "email": "octocat@example.test",
              "avatar_url": "https://avatars.example.test/octocat.png"
            })", "application/json");
        });
        server_.Get(R"(/repos/([^/]+)/([^/]+)/issues/([0-9]+)/comments)", [&](const httplib::Request& req, httplib::Response& res) {
            ++issue_comment_requests;
            capture_headers(req);
            if (paginate_comments.load()) {
                const auto page = req.has_param("page") ? req.get_param_value("page") : "1";
                if (page == "2") {
                    res.set_content(R"([
                      {
                        "id": 102,
                        "body": "issue conversation comment page two",
                        "user": {"login": "reviewer-page-two"}
                      }
                    ])", "application/json");
                    return;
                }
                res.set_header("Link", "<" + base_url() + req.path + "?page=2>; rel=\"next\"");
            }
            res.set_content(R"([
              {
                "id": 101,
                "body": "issue conversation comment",
                "user": {"login": "reviewer-a"}
              }
            ])", "application/json");
        });
        server_.Get(R"(/repos/([^/]+)/([^/]+)/pulls/([0-9]+)/comments)", [&](const httplib::Request& req, httplib::Response& res) {
            ++review_comment_requests;
            capture_headers(req);
            if (paginate_comments.load()) {
                const auto page = req.has_param("page") ? req.get_param_value("page") : "1";
                if (page == "2") {
                    res.set_content(R"([
                      {
                        "id": 203,
                        "body": "inline review comment page two",
                        "path": "src/next.cpp",
                        "line": 7,
                        "user": {"login": "reviewer-page-two"}
                      }
                    ])", "application/json");
                    return;
                }
                res.set_header("Link", "<" + base_url() + req.path + "?page=2>; rel=\"next\"");
            }
            res.set_content(R"([
              {
                "id": 202,
                "body": "inline review comment",
                "path": "src/main.cpp",
                "line": 42,
                "user": {"login": "reviewer-b"}
              }
            ])", "application/json");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ > 0) {
            worker_ = std::jthread([this](std::stop_token) {
                server_.listen_after_bind();
            });
        }
    }

    ~LocalGitHubApiServer() {
        server_.stop();
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] bool ready() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    [[nodiscard]] std::string authorization() const {
        std::lock_guard lock(mutex_);
        return last_authorization_;
    }

    [[nodiscard]] std::string accept() const {
        std::lock_guard lock(mutex_);
        return last_accept_;
    }

private:
    void capture_headers(const httplib::Request& req) {
        std::lock_guard lock(mutex_);
        last_authorization_ = req.get_header_value("Authorization");
        last_accept_ = req.get_header_value("Accept");
    }

    httplib::Server server_;
    int port_{0};
    std::jthread worker_;
    mutable std::mutex mutex_;
    std::string last_authorization_;
    std::string last_accept_;
};

// ═══════════════════════════════════════════════════════════════════════════════

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

TEST(Screens, ParsePrIdentifierUsesStrictNumbersAndGithubPullUrls) {
    EXPECT_EQ(screens::parsePrIdentifier("42"), std::optional<int>(42));
    EXPECT_EQ(screens::parsePrIdentifier("  https://github.com/org/repo/pull/123/files?diff=split  "), std::optional<int>(123));
    EXPECT_EQ(screens::parsePrIdentifier("github.com/org/repo/pull/77#discussion_r1"), std::optional<int>(77));

    EXPECT_FALSE(screens::parsePrIdentifier("123abc").has_value());
    EXPECT_FALSE(screens::parsePrIdentifier("https://example.com/org/repo/pull/123").has_value());
    EXPECT_FALSE(screens::parsePrIdentifier("https://github.com/org/repo/issues/123").has_value());
    EXPECT_FALSE(screens::parsePrIdentifier("0").has_value());
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

TEST(Memdir, TeamMemoryCanBeEnabledAtRuntime) {
    ScopedEnvVar disable_auto("CLAUDE_CODE_DISABLE_AUTO_MEMORY");
    ScopedEnvVar enable_team("CLAUDE_CODE_ENABLE_TEAM_MEMORY");
    ScopedEnvVar cc_sync_url("CC_TEAM_MEMORY_SYNC_URL");
    ScopedEnvVar ts_sync_url("TEAM_MEMORY_SYNC_URL");

    disable_auto.unset();
    enable_team.unset();
    cc_sync_url.unset();
    ts_sync_url.unset();
    EXPECT_FALSE(memdir::is_team_memory_enabled());

    enable_team.set("true");
    EXPECT_TRUE(memdir::is_team_memory_enabled());

    disable_auto.set("1");
    EXPECT_FALSE(memdir::is_team_memory_enabled());

    disable_auto.unset();
    enable_team.set("false");
    cc_sync_url.set("https://team-memory.example");
    EXPECT_FALSE(memdir::is_team_memory_enabled());

    enable_team.unset();
    EXPECT_TRUE(memdir::is_team_memory_enabled());

    cc_sync_url.unset();
    ts_sync_url.set("https://team-memory.example");
    EXPECT_TRUE(memdir::is_team_memory_enabled());
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

TEST(ProxyUtils, MirrorsTypeScriptEnvironmentPriorityAndNoProxyRules) {
    ScopedEnvVar https_upper("HTTPS_PROXY");
    ScopedEnvVar https_lower("https_proxy");
    ScopedEnvVar http_upper("HTTP_PROXY");
    ScopedEnvVar http_lower("http_proxy");
    ScopedEnvVar all_upper("ALL_PROXY");
    ScopedEnvVar all_lower("all_proxy");
    ScopedEnvVar no_proxy_upper("NO_PROXY");
    ScopedEnvVar no_proxy_lower("no_proxy");

    https_upper.unset();
    https_lower.unset();
    http_upper.unset();
    http_lower.unset();
    all_upper.unset();
    all_lower.unset();
    no_proxy_upper.unset();
    no_proxy_lower.unset();

    https_upper.set("http://upper-proxy:8080");
    https_lower.set("http://lower-proxy:8080");
    http_lower.set("http://http-lower-proxy:8081");
    no_proxy_upper.set("*");
    no_proxy_lower.set("localhost, .example.com api.local:8443 127.0.0.1");

    auto resolved = cc::utils::resolve_proxy();
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->url, "http://lower-proxy:8080");
    ASSERT_EQ(resolved->no_proxy.size(), 4u);

    EXPECT_FALSE(cc::utils::should_use_proxy("http://localhost:3000"));
    EXPECT_FALSE(cc::utils::should_use_proxy("https://127.0.0.1/status"));
    EXPECT_FALSE(cc::utils::should_use_proxy("https://example.com/path"));
    EXPECT_FALSE(cc::utils::should_use_proxy("https://sub.example.com/path"));
    EXPECT_TRUE(cc::utils::should_use_proxy("https://notexample.com/path"));
    EXPECT_FALSE(cc::utils::should_use_proxy("https://api.local:8443/path"));
    EXPECT_TRUE(cc::utils::should_use_proxy("https://api.local:443/path"));

    auto proxy = cc::utils::get_proxy_for_url("http://service.test/path");
    ASSERT_TRUE(proxy.has_value());
    EXPECT_EQ(*proxy, "http://lower-proxy:8080");

    no_proxy_lower.set("*");
    EXPECT_FALSE(cc::utils::should_use_proxy("https://service.test/path"));
    EXPECT_FALSE(cc::utils::get_proxy_for_url("https://service.test/path").has_value());
}

TEST(GitHubUtils, ParsesCommonGitHubRemoteUrlForms) {
    EXPECT_EQ(
        cc::utils::github_detail::parse_repo_full_name("git@github.com:openai/codex.git"),
        std::optional<std::string>{"openai/codex"}
    );
    EXPECT_EQ(
        cc::utils::github_detail::parse_repo_full_name("https://github.com/openai/codex.git\n"),
        std::optional<std::string>{"openai/codex"}
    );
    EXPECT_EQ(
        cc::utils::github_detail::parse_repo_full_name("ssh://git@github.com/openai/codex"),
        std::optional<std::string>{"openai/codex"}
    );
    EXPECT_FALSE(cc::utils::github_detail::parse_repo_full_name("https://gitlab.com/openai/codex.git").has_value());
}

TEST(GitHubUtils, FetchesAuthenticatedUserFromGitHubApi) {
    LocalGitHubApiServer server;
    ASSERT_TRUE(server.ready());

    ScopedEnvVar gh_token("GH_TOKEN");
    ScopedEnvVar github_token("GITHUB_TOKEN");
    ScopedEnvVar github_user("GITHUB_USER");
    ScopedEnvVar github_api_base("CC_REPL_GITHUB_API_BASE_URL");
    gh_token.unset();
    github_token.unset();
    github_user.unset();
    github_api_base.set(server.base_url().c_str());

    cc::utils::GitHubUtils missing;
    EXPECT_EQ(missing.check_auth(), cc::utils::GitHubAuthStatus::not_configured);
    EXPECT_FALSE(missing.is_authenticated());
    EXPECT_FALSE(missing.get_current_user().has_value());

    github_token.set("github-token-for-test");
    cc::utils::GitHubUtils configured;
    EXPECT_EQ(configured.check_auth(), cc::utils::GitHubAuthStatus::authenticated);
    EXPECT_TRUE(configured.is_authenticated());
    auto current = configured.get_current_user();
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->login, "octocat");
    EXPECT_EQ(current->name, "The Octocat");
    EXPECT_EQ(current->email, "octocat@example.test");
    EXPECT_EQ(current->avatar_url, "https://avatars.example.test/octocat.png");
    EXPECT_EQ(server.authorization(), "Bearer github-token-for-test");
    EXPECT_EQ(server.accept(), "application/vnd.github+json");

    gh_token.set("gh-token-for-test");
    cc::utils::GitHubUtils gh_configured;
    EXPECT_EQ(gh_configured.check_auth(), cc::utils::GitHubAuthStatus::authenticated);
    EXPECT_TRUE(gh_configured.is_authenticated());
    auto gh_current = gh_configured.get_current_user();
    ASSERT_TRUE(gh_current.has_value());
    EXPECT_EQ(server.authorization(), "Bearer gh-token-for-test");
}

TEST(GitHubUtils, MapsApiAuthFailuresToAuthStatus) {
    LocalGitHubApiServer server;
    ASSERT_TRUE(server.ready());

    ScopedEnvVar gh_token("GH_TOKEN");
    ScopedEnvVar github_token("GITHUB_TOKEN");
    ScopedEnvVar github_api_base("CC_REPL_GITHUB_API_BASE_URL");
    gh_token.set("gh-token-for-auth-failure");
    github_token.unset();
    github_api_base.set(server.base_url().c_str());

    server.user_status.store(401);
    cc::utils::GitHubUtils expired;
    EXPECT_FALSE(expired.get_current_user().has_value());
    EXPECT_EQ(expired.auth_status(), cc::utils::GitHubAuthStatus::token_expired);
    EXPECT_FALSE(expired.is_authenticated());

    server.user_status.store(403);
    cc::utils::GitHubUtils forbidden;
    EXPECT_FALSE(forbidden.get_current_user().has_value());
    EXPECT_EQ(forbidden.auth_status(), cc::utils::GitHubAuthStatus::rate_limited);
    EXPECT_FALSE(forbidden.is_authenticated());

    server.user_status.store(429);
    cc::utils::GitHubUtils rate_limited;
    EXPECT_FALSE(rate_limited.get_current_user().has_value());
    EXPECT_EQ(rate_limited.auth_status(), cc::utils::GitHubAuthStatus::rate_limited);
    EXPECT_FALSE(rate_limited.is_authenticated());
}

TEST(GitHubUtils, DetectRepoReadsLocalGitHubRemoteAndDefaultBranch) {
    auto root = std::filesystem::temp_directory_path() / "cc_repl_github_detect_repo_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        CurrentPathGuard cwd(root);
        ASSERT_EQ(std::system("git init -q"), 0);
        ASSERT_EQ(std::system("git remote add origin git@github.com:openai/codex.git"), 0);
        ASSERT_EQ(std::system("git symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/trunk"), 0);

        auto repo = cc::utils::GitHubUtils::detect_repo();
        ASSERT_TRUE(repo.has_value());
        EXPECT_EQ(repo->full_name, "openai/codex");
        EXPECT_EQ(repo->default_branch, "trunk");
    }

    std::filesystem::remove_all(root);
}

TEST(GitHubUtils, FetchesIssueAndReviewCommentsForDetectedRepo) {
    LocalGitHubApiServer server;
    ASSERT_TRUE(server.ready());

    ScopedEnvVar gh_token("GH_TOKEN");
    ScopedEnvVar github_token("GITHUB_TOKEN");
    ScopedEnvVar github_api_base("CC_REPL_GITHUB_API_BASE_URL");
    gh_token.set("gh-token-for-comments");
    github_token.unset();
    github_api_base.set(server.base_url().c_str());

    auto root = std::filesystem::temp_directory_path() / "cc_repl_github_pr_comments_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        CurrentPathGuard cwd(root);
        ASSERT_EQ(std::system("git init -q"), 0);
        ASSERT_EQ(std::system("git remote add origin https://github.com/openai/codex.git"), 0);

        cc::utils::GitHubUtils github;
        ASSERT_EQ(github.check_auth(), cc::utils::GitHubAuthStatus::authenticated);
        auto comments = github.get_pr_comments(123);
        ASSERT_EQ(comments.size(), 2u);
        EXPECT_EQ(comments[0].id, 101);
        EXPECT_EQ(comments[0].author, "reviewer-a");
        EXPECT_EQ(comments[0].body, "issue conversation comment");
        EXPECT_EQ(comments[1].id, 202);
        EXPECT_EQ(comments[1].author, "reviewer-b");
        EXPECT_EQ(comments[1].body, "inline review comment");
        EXPECT_EQ(comments[1].path, "src/main.cpp");
        EXPECT_EQ(comments[1].line, 42);
    }

    EXPECT_EQ(server.issue_comment_requests.load(), 1);
    EXPECT_EQ(server.review_comment_requests.load(), 1);
    EXPECT_EQ(server.authorization(), "Bearer gh-token-for-comments");
    std::filesystem::remove_all(root);
}

TEST(GitHubUtils, PaginatesIssueAndReviewCommentsForDetectedRepo) {
    LocalGitHubApiServer server;
    ASSERT_TRUE(server.ready());
    server.paginate_comments.store(true);

    ScopedEnvVar gh_token("GH_TOKEN");
    ScopedEnvVar github_token("GITHUB_TOKEN");
    ScopedEnvVar github_api_base("CC_REPL_GITHUB_API_BASE_URL");
    gh_token.set("gh-token-for-paginated-comments");
    github_token.unset();
    github_api_base.set(server.base_url().c_str());

    auto root = std::filesystem::temp_directory_path() / "cc_repl_github_pr_comments_pagination_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        CurrentPathGuard cwd(root);
        ASSERT_EQ(std::system("git init -q"), 0);
        ASSERT_EQ(std::system("git remote add origin https://github.com/openai/codex.git"), 0);

        cc::utils::GitHubUtils github;
        auto comments = github.get_pr_comments(456);
        ASSERT_EQ(comments.size(), 4u);
        EXPECT_EQ(comments[0].id, 101);
        EXPECT_EQ(comments[1].id, 102);
        EXPECT_EQ(comments[2].id, 202);
        EXPECT_EQ(comments[3].id, 203);
        EXPECT_EQ(comments[3].path, "src/next.cpp");
        EXPECT_EQ(comments[3].line, 7);
    }

    EXPECT_EQ(server.issue_comment_requests.load(), 2);
    EXPECT_EQ(server.review_comment_requests.load(), 2);
    std::filesystem::remove_all(root);
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

// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonUtils, ParseValidJson) {
    auto doc = cc::utils::json::parse(R"({"name":"test","value":42})");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->get_string("name"), "test");
    EXPECT_EQ(doc->get_int("value"), 42);
}

TEST(JsonUtils, AsDoubleReadsIntegerAndRealNumbers) {
    auto doc = cc::utils::json::parse(R"({"whole":1,"fractional":0.5})");
    ASSERT_TRUE(doc.has_value());
    EXPECT_DOUBLE_EQ(doc->root().get("whole").as_double(), 1.0);
    EXPECT_DOUBLE_EQ(doc->root().get("fractional").as_double(), 0.5);
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


    buf.push_back(4);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf.front(), 2);
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

TEST(PluginLoader, CreatePluginFromPathLoadsManifestAndComponentPaths) {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "cc_repl_plugin_loader_manifest_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude-plugin");
    fs::create_directories(root / "manifest");
    fs::create_directories(root / "agents");
    fs::create_directories(root / "output-styles");

    {
        std::ofstream manifest(root / ".claude-plugin" / "plugin.json");
        manifest << R"JSON({
  "name": "fixture-plugin",
  "version": "1.2.3",
  "description": "Fixture plugin",
  "author": {"name": "Ada", "email": "ada@example.test"},
  "commands": ["manifest/command.md"],
  "agents": ["agents/reviewer.md"],
  "outputStyles": ["output-styles/concise.md"]
})JSON";
    }
    {
        std::ofstream command(root / "manifest" / "command.md");
        command << "Run the fixture command\n";
    }
    {
        std::ofstream agent(root / "agents" / "reviewer.md");
        agent << "Review the fixture\n";
    }
    {
        std::ofstream style(root / "output-styles" / "concise.md");
        style << "Be concise\n";
    }

    auto loaded = cc::utils::plugin_loader::create_plugin_from_path(
        root,
        "fixture-plugin@inline",
        true,
        "fallback-plugin"
    );

    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    const auto& plugin = loaded->first;
    EXPECT_EQ(plugin.name, "fixture-plugin");
    EXPECT_EQ(plugin.manifest.version, std::optional<std::string>{"1.2.3"});
    ASSERT_TRUE(plugin.manifest.author.has_value());
    EXPECT_EQ(plugin.manifest.author->name, "Ada");
    EXPECT_TRUE(plugin.enabled);
    ASSERT_TRUE(plugin.commands_paths.has_value());
    ASSERT_EQ(plugin.commands_paths->size(), 1u);
    EXPECT_EQ(plugin.commands_paths->front(), root / "manifest" / "command.md");
    ASSERT_TRUE(plugin.agents_paths.has_value());
    EXPECT_EQ(plugin.agents_paths->front(), root / "agents" / "reviewer.md");
    ASSERT_TRUE(plugin.output_styles_paths.has_value());
    EXPECT_EQ(plugin.output_styles_paths->front(), root / "output-styles" / "concise.md");
    EXPECT_TRUE(loaded->second.empty());

    fs::remove_all(root);
}

TEST(PluginLoader, CacheOnlyLoadsMarkdownCommandsAgentsAndOutputStyles) {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "cc_repl_plugin_loader_cache_test";
    auto plugin_root = root / "cache-plugin";
    fs::remove_all(root);
    fs::create_directories(plugin_root / ".claude-plugin");
    fs::create_directories(plugin_root / "commands");
    fs::create_directories(plugin_root / "agents");
    fs::create_directories(plugin_root / "output-styles");
    ScopedEnvVar plugin_cache("CLAUDE_CODE_PLUGIN_CACHE_DIR");
    plugin_cache.set(root.string().c_str());

    {
        std::ofstream manifest(plugin_root / ".claude-plugin" / "plugin.json");
        manifest << R"JSON({
  "name": "cache-plugin",
  "version": "0.1.0",
  "description": "Cache plugin"
})JSON";
    }
    {
        std::ofstream command(plugin_root / "commands" / "build.md");
        command << "---\ndescription: Build fixture\n---\nBuild the fixture\n";
    }
    {
        std::ofstream agent(plugin_root / "agents" / "reviewer.md");
        agent << "---\ndescription: Review fixture\n---\nReview the fixture\n";
    }
    {
        std::ofstream style(plugin_root / "output-styles" / "concise.md");
        style << "---\ndescription: Concise style\n---\nAnswer briefly\n";
    }

    auto loaded = cc::utils::plugin_loader::load_all_plugins_cache_only();
    ASSERT_EQ(loaded.plugins.size(), 1u);
    EXPECT_TRUE(loaded.errors.empty());
    EXPECT_EQ(loaded.plugins.front().name, "cache-plugin");
    ASSERT_TRUE(loaded.plugins.front().commands_path.has_value());
    EXPECT_EQ(*loaded.plugins.front().commands_path, plugin_root / "commands");
    ASSERT_TRUE(loaded.plugins.front().agents_path.has_value());
    ASSERT_TRUE(loaded.plugins.front().output_styles_path.has_value());

    auto markdown = cc::utils::plugin_loader::walk_plugin_markdown(plugin_root / "commands");
    ASSERT_EQ(markdown.size(), 1u);
    EXPECT_EQ(markdown.front().name, "build");
    EXPECT_EQ(markdown.front().frontmatter.at("description"), "Build fixture");
    EXPECT_EQ(markdown.front().content, "Build the fixture\n");

    auto commands = cc::utils::plugin_loader::load_plugin_commands();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands.front().name, "cache-plugin:build");
    EXPECT_EQ(commands.front().content, "Build the fixture\n");

    auto agents = cc::utils::plugin_loader::load_plugin_agents();
    ASSERT_EQ(agents.size(), 1u);
    EXPECT_EQ(agents.front().name, "cache-plugin:reviewer");
    EXPECT_EQ(agents.front().content, "Review the fixture\n");

    auto styles = cc::utils::plugin_loader::load_plugin_output_styles();
    ASSERT_EQ(styles.size(), 1u);
    EXPECT_EQ(styles.front().name, "cache-plugin:concise");
    EXPECT_EQ(styles.front().content, "Answer briefly\n");

    fs::remove_all(root);
}

TEST(PluginLoader, CachePluginClonesGitUrlAndLoadsManifest) {
    namespace fs = std::filesystem;
    if (!command_available_for_test("git")) GTEST_SKIP() << "git is not available";

    auto root = fs::temp_directory_path() / "cc_repl_plugin_loader_git_cache_test";
    auto repo = root / "repo";
    auto cache_root = root / "plugins";
    fs::remove_all(root);
    fs::create_directories(repo / ".claude-plugin");
    fs::create_directories(repo / "commands");
    ScopedEnvVar plugin_cache("CLAUDE_CODE_PLUGIN_CACHE_DIR");
    plugin_cache.set(cache_root.string().c_str());

    {
        std::ofstream manifest(repo / ".claude-plugin" / "plugin.json");
        manifest << R"JSON({"name":"git-cache-plugin","version":"1.0.0","commands":["commands/run.md"]})JSON";
    }
    {
        std::ofstream command(repo / "commands" / "run.md");
        command << "Run from git\n";
    }

    run_shell_ok_for_test("git init --template= --initial-branch main " + shell_quote_for_test(repo) + " >/dev/null");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " config user.email test@example.invalid");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " config user.name Test");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " add .");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " commit --no-verify -m init >/dev/null");

    cc::utils::plugin_loader::PluginSource source =
        cc::utils::plugin_loader::GitUrlSource{.url = "file://" + repo.string()};
    auto cached = cc::utils::plugin_loader::cache_plugin(source);

    ASSERT_TRUE(cached.has_value()) << cached.error();
    EXPECT_EQ(cached->manifest.name, "git-cache-plugin");
    ASSERT_TRUE(cached->git_commit_sha.has_value());
    EXPECT_TRUE(fs::exists(cached->path / ".claude-plugin" / "plugin.json"));
    EXPECT_TRUE(fs::exists(cached->path / "commands" / "run.md"));

    auto loaded = cc::utils::plugin_loader::create_plugin_from_path(cached->path, "git-cache-plugin@test", true, "fallback");
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    EXPECT_EQ(loaded->first.name, "git-cache-plugin");
    ASSERT_TRUE(loaded->first.commands_paths.has_value());
    EXPECT_EQ(loaded->first.commands_paths->front(), cached->path / "commands" / "run.md");

    fs::remove_all(root);
}

TEST(PluginLoader, CachePluginExtractsGitSubdirAndRecordsSha) {
    namespace fs = std::filesystem;
    if (!command_available_for_test("git")) GTEST_SKIP() << "git is not available";

    auto root = fs::temp_directory_path() / "cc_repl_plugin_loader_git_subdir_test";
    auto repo = root / "repo";
    auto plugin_dir = repo / "packages" / "plugin";
    auto cache_root = root / "plugins";
    fs::remove_all(root);
    fs::create_directories(plugin_dir / ".claude-plugin");
    fs::create_directories(plugin_dir / "commands");
    ScopedEnvVar plugin_cache("CLAUDE_CODE_PLUGIN_CACHE_DIR");
    plugin_cache.set(cache_root.string().c_str());

    {
        std::ofstream manifest(plugin_dir / ".claude-plugin" / "plugin.json");
        manifest << R"JSON({"name":"subdir-cache-plugin","version":"2.0.0","commands":["commands/sub.md"]})JSON";
    }
    {
        std::ofstream command(plugin_dir / "commands" / "sub.md");
        command << "Run from subdir\n";
    }

    run_shell_ok_for_test("git init --template= --initial-branch main " + shell_quote_for_test(repo) + " >/dev/null");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " config user.email test@example.invalid");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " config user.name Test");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " add .");
    run_shell_ok_for_test("git -C " + shell_quote_for_test(repo) + " commit --no-verify -m init >/dev/null");

    cc::utils::plugin_loader::PluginSource source =
        cc::utils::plugin_loader::GitSubdirSource{
            .url = "file://" + repo.string(),
            .path = "packages/plugin",
        };
    auto cached = cc::utils::plugin_loader::cache_plugin(source);

    ASSERT_TRUE(cached.has_value()) << cached.error();
    EXPECT_EQ(cached->manifest.name, "subdir-cache-plugin");
    ASSERT_TRUE(cached->git_commit_sha.has_value());
    EXPECT_TRUE(fs::exists(cached->path / "commands" / "sub.md"));
    EXPECT_FALSE(fs::exists(cached->path / ".git"));

    fs::remove_all(root);
}

TEST(PluginLoader, CachePluginInstallsNpmPackageFromLocalSpec) {
    namespace fs = std::filesystem;
    if (!command_available_for_test("npm")) GTEST_SKIP() << "npm is not available";

    auto root = fs::temp_directory_path() / "cc_repl_plugin_loader_npm_cache_test";
    auto package_dir = root / "npm-plugin";
    auto cache_root = root / "plugins";
    fs::remove_all(root);
    fs::create_directories(package_dir / "commands");
    ScopedEnvVar plugin_cache("CLAUDE_CODE_PLUGIN_CACHE_DIR");
    plugin_cache.set(cache_root.string().c_str());

    {
        std::ofstream package_json(package_dir / "package.json");
        package_json << R"JSON({
  "name": "cc-repl-npm-plugin-fixture",
  "version": "1.0.0",
  "files": ["plugin.json", "commands"]
})JSON";
    }
    {
        std::ofstream manifest(package_dir / "plugin.json");
        manifest << R"JSON({"name":"npm-cache-plugin","version":"1.0.0","commands":["commands/npm.md"]})JSON";
    }
    {
        std::ofstream command(package_dir / "commands" / "npm.md");
        command << "Run from npm\n";
    }

    cc::utils::plugin_loader::PluginSource source =
        cc::utils::plugin_loader::NpmSource{.package_name = package_dir.string()};
    auto cached = cc::utils::plugin_loader::cache_plugin(source);

    ASSERT_TRUE(cached.has_value()) << cached.error();
    EXPECT_EQ(cached->manifest.name, "npm-cache-plugin");
    EXPECT_TRUE(fs::exists(cached->path / "commands" / "npm.md"));

    auto loaded = cc::utils::plugin_loader::load_all_plugins_cache_only();
    ASSERT_EQ(loaded.plugins.size(), 1u);
    EXPECT_EQ(loaded.plugins.front().name, "npm-cache-plugin");

    fs::remove_all(root);
}

TEST(PluginLoader, ProbesSeedCacheExactAndAnyVersion) {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "cc_repl_plugin_loader_seed_cache_test";
    auto seed = root / "seed";
    fs::remove_all(root);
    ScopedEnvVar seed_env("CLAUDE_CODE_PLUGIN_SEED_DIR");
    seed_env.set(seed.string().c_str());

    const auto seeded_path = cc::utils::plugin_loader::get_versioned_cache_path_in(
        seed,
        "seed-plugin@market",
        "1.2.3"
    );
    fs::create_directories(seeded_path);
    {
        std::ofstream marker(seeded_path / "marker.txt");
        marker << "seeded\n";
    }

    auto exact = cc::utils::plugin_loader::probe_seed_cache("seed-plugin@market", "1.2.3");
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(*exact, seeded_path);

    auto any = cc::utils::plugin_loader::probe_seed_cache_any_version("seed-plugin@market");
    ASSERT_TRUE(any.has_value());
    EXPECT_EQ(*any, seeded_path);

    auto copied = cc::utils::plugin_loader::copy_plugin_to_versioned_cache(root, "seed-plugin@market", "1.2.3");
    ASSERT_TRUE(copied.has_value()) << copied.error();
    EXPECT_EQ(*copied, seeded_path);

    fs::remove_all(root);
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

TEST(PluginMarketplace, ComputesRealSha256Checksums) {
    EXPECT_EQ(cc::plugins::detail::sha256_hex("hello"),
              "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    EXPECT_EQ(cc::plugins::detail::sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(ShellProviders, BashEvalCommandEscapesSingleQuotes) {
    ScopedEnvVar prefix("CLAUDE_CODE_SHELL_PREFIX");
    prefix.unset();

    auto provider = cc::utils::shell_providers::create_provider("/bin/bash", true);
    auto result = provider->build_exec_command(
        "printf '%s\\n' \"it's ok\"",
        cc::utils::shell_providers::BuildExecOptions{
            .id = "quote-test",
            .sandbox_tmp_dir = std::nullopt,
            .use_sandbox = false});

    EXPECT_NE(
        result.command_string.find("eval 'printf '\\''%s\\n'\\'' \"it'\\''s ok\"'"),
        std::string::npos);
}

TEST(ShellProviders, BashProviderSourcesSnapshotAndInjectsSandboxTmpdir) {
    ScopedEnvVar prefix("CLAUDE_CODE_SHELL_PREFIX");
    prefix.unset();

    auto sandbox = std::filesystem::temp_directory_path() / "cc_repl_shell_provider_sandbox_test";
    std::filesystem::remove_all(sandbox);
    std::filesystem::create_directories(sandbox);

    auto provider = cc::utils::shell_providers::create_provider("/bin/bash", false);
    auto result = provider->build_exec_command(
        "printf '%s' \"$TMPDIR\"",
        cc::utils::shell_providers::BuildExecOptions{
            .id = "snapshot-test",
            .sandbox_tmp_dir = sandbox.string(),
            .use_sandbox = true});

    EXPECT_EQ(result.cwd_file_path, (sandbox / "cwd-snapshot-test").string());
    EXPECT_NE(result.command_string.find("export TMPDIR='" + sandbox.string() + "'"), std::string::npos);
    EXPECT_NE(result.command_string.find("source "), std::string::npos);

    auto args = provider->get_spawn_args(result.command_string);
    ASSERT_GE(args.size(), 2u);
    EXPECT_EQ(args[0], "-c");
    EXPECT_EQ(args.back(), result.command_string);

    auto env = provider->get_environment_overrides("echo ok");
    EXPECT_EQ(env["CLAUDE_CODE_SHELL_PROVIDER"], "native");
    EXPECT_EQ(env["CLAUDE_CODE_SHELL_TYPE"], "bash");
    EXPECT_EQ(env["CLAUDE_CODE_LAST_COMMAND"], "echo ok");
    EXPECT_TRUE(env.contains("CLAUDE_CODE_SHELL_SNAPSHOT"));

    std::filesystem::remove_all(sandbox);
}

TEST(ShellProviders, PowershellProviderTracksCwdInSandboxAndQuotesPath) {
    auto provider = cc::utils::shell_providers::create_provider("pwsh", true);
    auto result = provider->build_exec_command(
        "Write-Output ok",
        cc::utils::shell_providers::BuildExecOptions{
            .id = "ps-test",
            .sandbox_tmp_dir = "/tmp/cc repl's sandbox",
            .use_sandbox = true});

    EXPECT_EQ(result.cwd_file_path, "/tmp/cc repl's sandbox/cwd-ps-test");
    EXPECT_NE(result.command_string.find("Out-File -FilePath '/tmp/cc repl''s sandbox/cwd-ps-test'"), std::string::npos);
    auto args = provider->get_spawn_args(result.command_string);
    ASSERT_EQ(args.size(), 4u);
    EXPECT_EQ(args[0], "-NoProfile");
    EXPECT_EQ(args[1], "-NonInteractive");
    EXPECT_EQ(args[2], "-Command");
    EXPECT_EQ(args[3], result.command_string);

    auto env = provider->get_environment_overrides("Write-Output ok");
    EXPECT_EQ(env["CLAUDE_CODE_SHELL_TYPE"], "powershell");
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

TEST(DiffUtils, ApplyPatchHandlesUnifiedDiffHunks) {
    const std::string content = "one\ntwo\nthree\nfour\n";
    const std::string patch = R"PATCH(--- a/file.txt
+++ b/file.txt
@@ -1,4 +1,5 @@
 one
-two
+TWO
 three
+added
 four
)PATCH";

    auto result = cc::utils::apply_patch(content, patch);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "one\nTWO\nthree\nadded\nfour\n");
}

TEST(DiffUtils, ApplyPatchHandlesMultipleHunks) {
    const std::string content = "a\nb\nc\nd\ne\n";
    const std::string patch = R"PATCH(--- a/file.txt
+++ b/file.txt
@@ -2,2 +2,2 @@
-b
+B
 c
@@ -5,1 +5,2 @@
 e
+f
)PATCH";

    auto result = cc::utils::apply_patch(content, patch);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "a\nB\nc\nd\ne\nf\n");
}

TEST(DiffUtils, ApplyPatchRejectsMismatchedContext) {
    const std::string content = "alpha\nbeta\n";
    const std::string patch = R"PATCH(--- a/file.txt
+++ b/file.txt
@@ -1,2 +1,2 @@
 alpha
-gamma
+delta
)PATCH";

    auto result = cc::utils::apply_patch(content, patch);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Patch context mismatch"), std::string::npos);
}

TEST(GitDiff, GenerateUnifiedDiffPreservesContextAndRoundTripsToStats) {
    auto patch = cc::utils::generate_unified_diff(
        "alpha\nbeta\ngamma\n",
        "alpha\ndelta\ngamma\n",
        "notes.txt");

    EXPECT_NE(patch.find("diff --git a/notes.txt b/notes.txt"), std::string::npos);
    EXPECT_NE(patch.find(" alpha\n"), std::string::npos);
    EXPECT_NE(patch.find("-beta\n"), std::string::npos);
    EXPECT_NE(patch.find("+delta\n"), std::string::npos);
    EXPECT_NE(patch.find(" gamma\n"), std::string::npos);

    auto parsed = cc::utils::parse_unified_diff(patch);
    ASSERT_EQ(parsed.size(), 1u);
    ASSERT_EQ(parsed[0].hunks.size(), 1u);

    auto stats = cc::utils::get_diff_stats(parsed);
    EXPECT_EQ(stats.files_changed, 1);
    EXPECT_EQ(stats.additions, 1);
    EXPECT_EQ(stats.deletions, 1);
}

TEST(GitDiff, GenerateUnifiedDiffReturnsEmptyStringForIdenticalContent) {
    EXPECT_EQ(
        cc::utils::generate_unified_diff("same\ncontent\n", "same\ncontent\n", "same.txt"),
        "");
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

// ===========================================================================
// cc.ui.terminal_io — pure ANSI/CSI/SGR parsing helpers.
// These parser entry points (parse_sgr, strip_ansi, parse_csi, tokenize_ansi)
// had no direct test coverage; the suite below pins their contract.
// ===========================================================================
namespace tio = cc::ui::termio;

TEST(TerminalIO, StripAnsiLeavesPlainText) {
    EXPECT_EQ(tio::strip_ansi("hello world"), "hello world");
}

TEST(TerminalIO, StripAnsiRemovesCsiSGR) {
    // \033[1;31m bold red, \033[0m reset
    std::string in = std::string("\033[1;31m") + "ERR" + "\033[0m";
    EXPECT_EQ(tio::strip_ansi(in), "ERR");
}

TEST(TerminalIO, StripAnsiRemovesCursorMoves) {
    // \033[2A cursor up 2, \033[10G column 10
    std::string in = std::string("a\033[2Ab\033[10Gc");
    EXPECT_EQ(tio::strip_ansi(in), "abc");
}

TEST(TerminalIO, StripAnsiRemovesOscTerminatedByBell) {
    // OSC sequence terminated by BEL (\a)
    std::string in = std::string("\033]0;title\a") + "body";
    EXPECT_EQ(tio::strip_ansi(in), "body");
}

TEST(TerminalIO, StripAnsiRemovesOscTerminatedByST) {
    // OSC sequence terminated by ST (ESC \)
    std::string in = std::string("\033]0;title\033\\") + "body";
    EXPECT_EQ(tio::strip_ansi(in), "body");
}

TEST(TerminalIO, ParseSGREmptyIsReset) {
    auto r = tio::parse_sgr("");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->bold);
    EXPECT_FALSE(r->underline);
    // fg/bg are default (monostate)
    EXPECT_TRUE(std::holds_alternative<std::monostate>(r->fg));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(r->bg));
}

TEST(TerminalIO, ParseSGRBasicAttributes) {
    // 1=bold 3=italic 4=underline 7=inverse 9=strikethrough
    auto r = tio::parse_sgr("1;3;4;7;9");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->bold);
    EXPECT_TRUE(r->italic);
    EXPECT_TRUE(r->underline);
    EXPECT_TRUE(r->inverse);
    EXPECT_TRUE(r->strikethrough);
}

TEST(TerminalIO, ParseSGR16ColorForeground) {
    // 31 = red foreground (Color16 index 1)
    auto r = tio::parse_sgr("31");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tio::Color16>(r->fg));
    EXPECT_EQ(static_cast<int>(std::get<tio::Color16>(r->fg)), 1);
}

TEST(TerminalIO, ParseSGR256Color) {
    // 38;5;202 = 256-color foreground index 202
    auto r = tio::parse_sgr("38;5;202");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tio::Color256>(r->fg));
    EXPECT_EQ(std::get<tio::Color256>(r->fg).index, 202);
}

TEST(TerminalIO, ParseSGRTrueColor) {
    // 38;2;10;20;30 = truecolor foreground
    auto r = tio::parse_sgr("38;2;10;20;30");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tio::TrueColor>(r->fg));
    auto tc = std::get<tio::TrueColor>(r->fg);
    EXPECT_EQ(tc.r, 10);
    EXPECT_EQ(tc.g, 20);
    EXPECT_EQ(tc.b, 30);
}

TEST(TerminalIO, ParseSGRBrightForeground) {
    // 91 = bright red fg (Color16 index 9)
    auto r = tio::parse_sgr("91");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tio::Color16>(r->fg));
    EXPECT_EQ(static_cast<int>(std::get<tio::Color16>(r->fg)), 9);
}

TEST(TerminalIO, ParseSGRResetWithinSequence) {
    // 1;31;0 — the trailing 0 resets everything to defaults.
    auto r = tio::parse_sgr("1;31;0");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->bold);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(r->fg));
}

TEST(TerminalIO, ParseCSIBasicCursorMove) {
    // "2A" → params [2], final 'A' (cursor up)
    auto r = tio::parse_csi("2A");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->params.size(), 1u);
    EXPECT_EQ(r->params[0], 2);
    EXPECT_EQ(r->final_byte, 'A');
    EXPECT_TRUE(r->intermediate.empty());
}

TEST(TerminalIO, ParseCSIDefaultParam) {
    // "H" with no parameter bytes → empty params list (the caller applies the
    // documented default for the final byte, e.g. cursor home).
    auto r = tio::parse_csi("H");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->params.empty());
    EXPECT_EQ(r->final_byte, 'H');
}

TEST(TerminalIO, ParseCSIMultipleParams) {
    // "5;10H" → row 5, col 10
    auto r = tio::parse_csi("5;10H");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->params.size(), 2u);
    EXPECT_EQ(r->params[0], 5);
    EXPECT_EQ(r->params[1], 10);
}

TEST(TerminalIO, ParseCSIMissingFinalByteFails) {
    auto r = tio::parse_csi("12;3");
    EXPECT_FALSE(r.has_value());
}

TEST(TerminalIO, ParseCSIEmptyFails) {
    auto r = tio::parse_csi("");
    EXPECT_FALSE(r.has_value());
}

TEST(TerminalIO, TokenizeAnsiPureText) {
    auto toks = tio::tokenize_ansi("plain text");
    ASSERT_EQ(toks.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<tio::TextToken>(toks[0]));
    EXPECT_EQ(std::get<tio::TextToken>(toks[0]).content, "plain text");
}

TEST(TerminalIO, TokenizeAnsiTextThenSGRThenText) {
    // "hi" + ESC[1m + "!"  → TextToken, SgrToken, TextToken
    std::string in = std::string("hi") + "\033[1m" + "!";
    auto toks = tio::tokenize_ansi(in);
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<tio::TextToken>(toks[0]));
    EXPECT_TRUE(std::holds_alternative<tio::SgrToken>(toks[1]));
    EXPECT_TRUE(std::holds_alternative<tio::TextToken>(toks[2]));
}

TEST(TerminalIO, GenerateCSIBuildsSequence) {
    // generate_csi(params, intermediate, final_byte) → ESC [ params intermediate final
    const int params[] = {2};
    auto seq = tio::generate_csi(params, {}, 'A');
    EXPECT_EQ(seq, std::string("\033[2A"));
}

TEST(TerminalIO, GenerateCSIMultipleParams) {
    const int params[] = {5, 10};
    auto seq = tio::generate_csi(params, {}, 'H');
    EXPECT_EQ(seq, std::string("\033[5;10H"));
}

// ─── cc.utils.json parser coverage (guards the parse/parse_file/to_string/
// chained-get surface used across services) ──────────────────────────────────
TEST(JsonCCUtils, ParsesPrimitivesAndCollections) {
    auto doc_null = cc::utils::json::parse("null");
    ASSERT_TRUE(doc_null.has_value());
    EXPECT_TRUE(doc_null->root().is_null());

    auto doc_b = cc::utils::json::parse("true");
    ASSERT_TRUE(doc_b.has_value());
    EXPECT_TRUE(doc_b->root().is_bool());
    EXPECT_EQ(doc_b->root().as_bool(), true);

    auto doc_i = cc::utils::json::parse("42");
    ASSERT_TRUE(doc_i.has_value());
    EXPECT_EQ(doc_i->root().as_int(), 42);

    auto doc_s = cc::utils::json::parse("\"hello\"");
    ASSERT_TRUE(doc_s.has_value());
    EXPECT_EQ(doc_s->root().as_str(), "hello");

    auto doc_arr = cc::utils::json::parse("[1, 2, 3]");
    ASSERT_TRUE(doc_arr.has_value());
    EXPECT_TRUE(doc_arr->root().is_arr());
    EXPECT_EQ(doc_arr->root().size(), 3u);

    auto doc_obj = cc::utils::json::parse(R"({"a": 1, "b": "x"})");
    ASSERT_TRUE(doc_obj.has_value());
    auto root = doc_obj->root();
    EXPECT_TRUE(root.is_obj());
    EXPECT_EQ(root.get("a").as_int(), 1);
    EXPECT_EQ(root.get("b").as_str(), "x");
}

TEST(JsonCCUtils, ParsesNestedStructures) {
    auto r = cc::utils::json::parse(R"({"list": [1, {"k": true}], "n": null})");
    ASSERT_TRUE(r.has_value());
    auto root = r->root();
    EXPECT_TRUE(root.is_obj());
    auto list = root.get("list");
    EXPECT_TRUE(list.is_arr());
    EXPECT_EQ(list.at(0).as_int(), 1);
    EXPECT_TRUE(list.at(1).get("k").is_bool());
    EXPECT_TRUE(root.get("n").is_null());
}

TEST(JsonCCUtils, ParseFileReturnsErrorOnMissingFile) {
    auto r = cc::utils::json::parse_file("/nonexistent/cc-json-read-test.json");
    EXPECT_FALSE(r.has_value());
}

TEST(JsonCCUtils, ParseFileReturnsErrorOnInvalidJson) {
    auto tmp = std::filesystem::temp_directory_path() / "cc-json-read-test.json";
    { std::ofstream f(tmp); f << "{invalid}"; }
    auto r = cc::utils::json::parse_file(tmp);
    EXPECT_FALSE(r.has_value());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST(JsonCCUtils, ParseFileRoundTripsValidJson) {
    auto tmp = std::filesystem::temp_directory_path() / "cc-json-read-rt.json";
    { std::ofstream f(tmp); f << R"({"key": "value", "n": 5})"; }
    auto r = cc::utils::json::parse_file(tmp);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->root().get("key").as_str(), "value");
    EXPECT_EQ(r->root().get("n").as_int(), 5);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST(JsonCCUtils, RoundTripsThroughJsonToString) {
    auto r = cc::utils::json::parse(R"({"key": "value", "n": 5})");
    ASSERT_TRUE(r.has_value());
    std::string s = cc::utils::json::to_string(*r);
    auto r2 = cc::utils::json::parse(s);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->root().get("key").as_str(), "value");
    EXPECT_EQ(r2->root().get("n").as_int(), 5);
}

TEST(JsonCCUtils, JsonGetPathAccess) {
    auto r = cc::utils::json::parse(R"({"a": {"b": "deep"}})");
    ASSERT_TRUE(r.has_value());
    auto root = r->root();
    EXPECT_EQ(root.get("a").get("b").as_str(), "deep");
    EXPECT_FALSE(root.get("a").get("missing").valid());
    EXPECT_FALSE(root.get("nonexistent").valid());
}
