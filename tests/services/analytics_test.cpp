#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>

import std;
import cc.services.analytics;
import cc.utils.json;

namespace fs = std::filesystem;

namespace {

/// Temp directory with $LOOM_ANALYTICS_DISABLED cleared, cleaned up on
/// destruction including on early return.
class TempLogDir {
public:
    TempLogDir() {
        root_ = fs::temp_directory_path() /
                ("loom-analytics-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter_++));
        fs::create_directories(root_);
        if (const char* prev = std::getenv("LOOM_ANALYTICS_DISABLED")) {
            prev_disabled_ = prev;
        }
        ::unsetenv("LOOM_ANALYTICS_DISABLED");
    }

    ~TempLogDir() {
        if (prev_disabled_) ::setenv("LOOM_ANALYTICS_DISABLED", prev_disabled_->c_str(), 1);
        else ::unsetenv("LOOM_ANALYTICS_DISABLED");
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempLogDir(const TempLogDir&) = delete;
    TempLogDir& operator=(const TempLogDir&) = delete;

    [[nodiscard]] fs::path log() const { return root_ / "analytics.ndjson"; }
    [[nodiscard]] fs::path root() const { return root_; }

private:
    fs::path root_;
    std::optional<std::string> prev_disabled_;
    static inline int counter_ = 0;
};

/// Read the log back as lines (empty when the file does not exist).
[[nodiscard]] std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

}  // namespace

TEST(LocalAnalytics, WritesOneJsonObjectPerEvent) {
    TempLogDir dir;
    cc::services::analytics::LocalAnalytics log(dir.log());
    EXPECT_TRUE(log.log_event("session_start"));
    EXPECT_TRUE(log.log_event("tool_use", {{"tool", "Bash"}}));

    const auto lines = read_lines(dir.log());
    ASSERT_EQ(lines.size(), 2u);
    for (const auto& line : lines) {
        auto parsed = cc::utils::json::parse(line);
        ASSERT_TRUE(parsed) << "every line must be valid JSON: " << line;
    }
}

TEST(LocalAnalytics, EventCarriesNameTimestampAndProperties) {
    TempLogDir dir;
    cc::services::analytics::LocalAnalytics log(dir.log());
    ASSERT_TRUE(log.log_event("tool_use", {{"tool", "Read"}, {"ok", "1"}}));

    const auto lines = read_lines(dir.log());
    ASSERT_EQ(lines.size(), 1u);
    auto parsed = cc::utils::json::parse(lines[0]);
    ASSERT_TRUE(parsed);
    const auto root = parsed->root();
    EXPECT_EQ(root.get("event").as_str(), std::string_view("tool_use"));
    EXPECT_TRUE(root.get("ts_ms").is_num()) << "a timestamp must be recorded";
    auto props = root.get("properties");
    ASSERT_TRUE(props.is_obj());
    EXPECT_EQ(props.get("tool").as_str(), std::string_view("Read"));
    EXPECT_EQ(props.get("ok").as_str(), std::string_view("1"));
}

// Escaping is the reason this goes through the JSON builder instead of
// string concatenation. A property containing a quote or a newline must not
// be able to produce a broken line or a second, forged line.
TEST(LocalAnalytics, QuotesAndNewlinesDoNotBreakTheLine) {
    TempLogDir dir;
    cc::services::analytics::LocalAnalytics log(dir.log());
    ASSERT_TRUE(log.log_event("prompt",
                              {{"text", "he said \"hi\"\nsecond line\\end"}}));

    const auto lines = read_lines(dir.log());
    ASSERT_EQ(lines.size(), 1u)
        << "an embedded newline must not create a second line";

    auto parsed = cc::utils::json::parse(lines[0]);
    ASSERT_TRUE(parsed) << "the line must still parse: " << lines[0];
    EXPECT_EQ(parsed->root().get("properties").get("text").as_str(),
              std::string_view("he said \"hi\"\nsecond line\\end"))
        << "the value must round-trip exactly";
}

TEST(LocalAnalytics, CreatesMissingParentDirectories) {
    TempLogDir dir;
    const auto nested = dir.root() / "deep" / "nested" / "analytics.ndjson";
    cc::services::analytics::LocalAnalytics log(nested);
    EXPECT_TRUE(log.log_event("first_write"));
    EXPECT_TRUE(fs::exists(nested));
}

TEST(LocalAnalytics, AppendsRatherThanTruncating) {
    TempLogDir dir;
    cc::services::analytics::LocalAnalytics log(dir.log());
    ASSERT_TRUE(log.log_event("one"));
    ASSERT_TRUE(log.log_event("two"));

    // A second instance over the same path must not clobber the first run.
    cc::services::analytics::LocalAnalytics reopened(dir.log());
    ASSERT_TRUE(reopened.log_event("three"));

    EXPECT_EQ(read_lines(dir.log()).size(), 3u);
}

// Telemetry must never be load-bearing: an unwritable destination drops the
// event and reports false rather than throwing or failing the session.
TEST(LocalAnalytics, UnwritableDestinationReportsFalseInsteadOfThrowing) {
    TempLogDir dir;
    const auto blocked = dir.root() / "blocked";
    std::ofstream(blocked) << "not a directory\n";

    cc::services::analytics::LocalAnalytics log(blocked / "analytics.ndjson");
    EXPECT_FALSE(log.log_event("should_be_dropped"));
}

TEST(LocalAnalytics, DisabledSwitchSuppressesWritesEntirely) {
    TempLogDir dir;
    cc::services::analytics::LocalAnalytics log(dir.log());
    ::setenv("LOOM_ANALYTICS_DISABLED", "1", 1);
    EXPECT_FALSE(log.log_event("suppressed"));
    ::unsetenv("LOOM_ANALYTICS_DISABLED");
    EXPECT_FALSE(fs::exists(dir.log()))
        << "nothing should have been written while disabled";
}

TEST(LocalAnalytics, DisabledSwitchAcceptsTheUsualTruthySpellings) {
    using cc::services::analytics::analytics_enabled;
    for (const char* truthy : {"1", "true", "TRUE", "yes"}) {
        ::setenv("LOOM_ANALYTICS_DISABLED", truthy, 1);
        EXPECT_FALSE(analytics_enabled()) << "expected disabled for " << truthy;
    }
    for (const char* falsy : {"0", "false", "no", ""}) {
        ::setenv("LOOM_ANALYTICS_DISABLED", falsy, 1);
        EXPECT_TRUE(analytics_enabled()) << "expected enabled for " << falsy;
    }
    ::unsetenv("LOOM_ANALYTICS_DISABLED");
    EXPECT_TRUE(analytics_enabled());
}

// The default path must land under the state dir, not in the config dir or
// the current directory -- and LOOM_ANALYTICS_PATH must override it, which is
// what keeps tests off the real log.
TEST(LocalAnalytics, DefaultPathIsUnderStateDirAndOverrideWins) {
    ::unsetenv("LOOM_ANALYTICS_PATH");
    const auto def = cc::services::analytics::analytics_log_path();
    EXPECT_EQ(def.filename(), "analytics.ndjson");
    EXPECT_EQ(def.parent_path().filename(), "loom")
        << "should live in a loom-named state subdirectory";

    TempLogDir dir;
    ::setenv("LOOM_ANALYTICS_PATH", dir.log().c_str(), 1);
    EXPECT_EQ(cc::services::analytics::analytics_log_path(), dir.log());
    ::unsetenv("LOOM_ANALYTICS_PATH");
}

// The invariant that matters most for a module like this: local-only. A
// runtime test cannot observe "did not open a socket", so the check lives on
// the source text -- if someone adds an HTTP import to this module, this
// fails and forces the review conversation.
TEST(LocalAnalytics, ModuleSourceImportsNoHttpClient) {
    const auto src = fs::path(__FILE__).parent_path().parent_path().parent_path() /
                     "src" / "services" / "analytics.cppm";
    ASSERT_TRUE(fs::exists(src)) << "cannot find the module source at " << src;

    std::ifstream in(src);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const auto text = buffer.str();

    for (const char* forbidden : {"cc.utils.http", "cc.services.api", "curl",
                                  "httplib", "socket", "getaddrinfo"}) {
        EXPECT_EQ(text.find(forbidden), std::string::npos)
            << "analytics.cppm must stay local-only, but mentions '" << forbidden
            << "'; if network reporting is genuinely wanted, that is a separate "
               "decision, not a flag on this module";
    }
}
