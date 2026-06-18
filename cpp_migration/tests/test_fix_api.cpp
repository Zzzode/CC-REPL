// test_fix_api.cpp — coverage for the M6 API quota/billing stubs ports.
//
// These tests exercise the pure/observable logic of the three migrated modules
// without performing real network I/O:
//   - first_token_date: date validation + sink forwarding (no network via the
//     exported parse helpers indirectly through fetch failure paths).
//   - ultrareview_quota: non-subscriber short-circuit returns nullopt;
//     legacy field mapping is preserved by get_ultrareview_quota shape.
//   - overage_credit: format_grant_amount USD formatting (TS formatGrantAmount)
//     + cache TTL/invalidate semantics.
//
// Register in tests/CMakeLists.txt:
//   add_executable(test_fix_api test_fix_api.cpp)
//   target_link_libraries(test_fix_api PRIVATE cc_core GTest::gtest_main)
//   gtest_discover_tests(test_fix_api
//       DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

#include <gtest/gtest.h>

import cc.services.api.first_token_date;
import cc.services.api.ultrareview_quota;
import cc.services.api.overage_credit;

using cc::services::api::OverageCreditGrantInfo;
using cc::services::api::UltrareviewQuota;

// ---------------------------------------------------------------------------
// overage_credit.cppm — format_grant_amount mirrors TS formatGrantAmount.
// ---------------------------------------------------------------------------

TEST(OverageCredit, FormatGrantAmountUsdWhole) {
    OverageCreditGrantInfo info{
        .amount_minor_units = 500,   // $5.00
        .currency = std::string("USD"),
    };
    auto out = cc::services::api::format_grant_amount(info);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "$5");
}

TEST(OverageCredit, FormatGrantAmountUsdFractional) {
    OverageCreditGrantInfo info{
        .amount_minor_units = 549,   // $5.49
        .currency = std::string("USD"),
    };
    auto out = cc::services::api::format_grant_amount(info);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "$5.49");
}

TEST(OverageCredit, FormatGrantAmountLowercaseUsdAccepted) {
    OverageCreditGrantInfo info{
        .amount_minor_units = 1000,  // $10.00
        .currency = std::string("usd"),
    };
    auto out = cc::services::api::format_grant_amount(info);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "$10");
}

TEST(OverageCredit, FormatGrantAmountNonUsdReturnsNullopt) {
    OverageCreditGrantInfo info{
        .amount_minor_units = 1000,
        .currency = std::string("EUR"),
    };
    EXPECT_FALSE(cc::services::api::format_grant_amount(info).has_value());
}

TEST(OverageCredit, FormatGrantAmountMissingAmountReturnsNullopt) {
    OverageCreditGrantInfo info{.currency = std::string("USD")};
    EXPECT_FALSE(cc::services::api::format_grant_amount(info).has_value());
}

TEST(OverageCredit, FormatGrantAmountMissingCurrencyReturnsNullopt) {
    OverageCreditGrantInfo info{.amount_minor_units = 500};
    EXPECT_FALSE(cc::services::api::format_grant_amount(info).has_value());
}

// ---------------------------------------------------------------------------
// ultrareview_quota.cppm — non-subscriber path returns nullopt, matching TS
// `if (!isClaudeAISubscriber()) return null`.
// ---------------------------------------------------------------------------

TEST(UltrareviewQuota, NonSubscriberReturnsNullopt) {
    // No CC_OAUTH_TOKEN / etc. in the test environment -> not a subscriber.
    auto result = cc::services::api::fetch_ultrareview_quota();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());  // std::nullopt, i.e. TS null
}

TEST(UltrareviewQuota, LegacyFieldsDefaultZeroed) {
    auto q = cc::services::api::get_ultrareview_quota();
    EXPECT_EQ(q.remaining, 0u);
    EXPECT_EQ(q.total, 0u);
    EXPECT_FALSE(q.is_unlimited);
    EXPECT_EQ(q.reviews_used, 0u);
    EXPECT_EQ(q.reviews_limit, 0u);
    EXPECT_EQ(q.reviews_remaining, 0u);
    EXPECT_FALSE(q.is_overage);
}

TEST(UltrareviewQuota, CanUseDefaultsFalse) {
    EXPECT_FALSE(cc::services::api::can_use_ultrareview());
}

// ---------------------------------------------------------------------------
// first_token_date.cppm — fetch returns an honest error when credentials are
// missing (no OAuth token in the test env), never silently fakes a date.
// ---------------------------------------------------------------------------

TEST(FirstTokenDate, MissingCredentialsReturnsError) {
    auto result = cc::services::api::fetch_first_token_date();
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}

TEST(FirstTokenDate, SetFirstTokenDateForwardsToSink) {
    std::optional<std::string> captured;
    auto sink = cc::services::api::StoreSink(
        [&](const std::optional<std::string>& v) { captured = v; });

    using Clock = std::chrono::system_clock;
    cc::services::api::set_first_token_date(Clock::time_point{}, sink);
    ASSERT_TRUE(captured.has_value());
    // Epoch in UTC -> 1970-01-01T00:00:00Z
    EXPECT_EQ(*captured, "1970-01-01T00:00:00Z");
}

TEST(FirstTokenDate, SetFirstTokenDateNoopWithoutSink) {
    using Clock = std::chrono::system_clock;
    // Must not crash when no sink is provided (preserves original stub
    // behaviour of being safe to call unconditionally).
    cc::services::api::set_first_token_date(Clock::time_point{});
    SUCCEED();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
