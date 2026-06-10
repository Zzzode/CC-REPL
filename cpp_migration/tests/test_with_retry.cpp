/// @file test_with_retry.cpp
/// @brief Phase 3-E2E: WithRetry / BackoffDelay / RunWithRetry GoogleTest unit tests
///
/// Coverage:
///   1. BackoffDelay: exponential growth, cap truncation, non-zero +/- jitter
///   2. WithRetry<fn()->int>: 0/2xx returns directly; 429/5xx retries by count; 400/non-retry -> immediate failure
///   3. WithRetry<fn()->expected<T,string>>: value returns directly; unexpected retries
///   4. fn throws std::exception / captured as unexpected
///   5. RunWithRetry: transport errors (<0) retry; final failure returns last negative
///   6. max_attempts=1 never retries

#include <atomic>
#include <chrono>
#include <cmath>
#include <expected>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

import cc.services.api.with_retry;
import cc.services.api.with_retry_simple;

// Do not use "using namespace cc::services::api" — it contains another RetryConfig
// (Phase 2 full version) which conflicts with with_retry_simple::RetryConfig.
using namespace cc::services::api::with_retry_simple;

// Dispatch-required interface lives under cc::services::api (WithRetry / RetryConfigLite); explicit alias.
namespace api = cc::services::api;
using RetryConfigLite  = cc::services::api::RetryConfigLite;
template <typename F>
auto WithRetryLite(const RetryConfigLite& cfg, F&& fn) {
    return api::WithRetry(cfg, std::forward<F>(fn));
}

namespace {

// ---------------------------------------------------------------------------
// Simple high-resolution timer (avoids CI-affected std::chrono drift)
// ---------------------------------------------------------------------------
template <typename F>
std::chrono::milliseconds MeasureMs(F&& f) {
    auto start = std::chrono::steady_clock::now();
    std::invoke(std::forward<F>(f));
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
}

} // namespace

// ===========================================================================
// BackoffDelay (int contract)
// ===========================================================================
TEST(BackoffDelay, ExponentialGrowth) {
    RetryConfig cfg;
    cfg.base_delay = std::chrono::milliseconds(100);
    cfg.max_delay  = std::chrono::milliseconds(1000);
    cfg.jitter_ratio = 0.0;  // disable jitter for strict 2^n test

    auto d0 = BackoffDelay(cfg, 0).count();
    auto d1 = BackoffDelay(cfg, 1).count();
    auto d2 = BackoffDelay(cfg, 2).count();
    auto d3 = BackoffDelay(cfg, 3).count();
    // base=100ms × 2^attempt
    EXPECT_EQ(d0, 100);
    EXPECT_EQ(d1, 200);
    EXPECT_EQ(d2, 400);
    EXPECT_EQ(d3, 800);

    // cap at 1000ms: attempt=4 -> 1600 gets capped to 1000
    EXPECT_EQ(BackoffDelay(cfg, 4).count(), 1000);
    EXPECT_EQ(BackoffDelay(cfg, 10).count(), 1000);
}

TEST(BackoffDelay, JitterRange) {
    RetryConfig cfg;
    cfg.base_delay    = std::chrono::milliseconds(200);
    cfg.max_delay     = std::chrono::milliseconds(5000);
    cfg.jitter_ratio  = 0.2;

    // sample multiple times, assert each falls in [160, 240] ms range (base=200, +/-20%)
    bool any_different = false;
    long long prev = -1;
    for (int i = 0; i < 50; ++i) {
        auto ms = BackoffDelay(cfg, 0).count();
        EXPECT_GE(ms, 160);
        EXPECT_LE(ms, 240);
        if (prev != -1 && prev != ms) any_different = true;
        prev = ms;
    }
    // statistically, at least one should differ
    EXPECT_TRUE(any_different);
}

// ===========================================================================
// RunWithRetry (int contract, phase 3-A style)
// ===========================================================================
TEST(RunWithRetry, SuccessOnFirstTry) {
    RetryConfig cfg;
    cfg.max_attempts = 5;
    int calls = 0;
    int rc = RunWithRetry(cfg, [&](int attempt) {
        ++calls;
        EXPECT_EQ(attempt, 0);
        return 0;       // success
    });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(calls, 1);
}

TEST(RunWithRetry, Http2xxPassthrough) {
    RetryConfig cfg;
    int rc = RunWithRetry(cfg, [&](int) { return 204; });
    EXPECT_EQ(rc, 204);
}

TEST(RunWithRetry, TransportErrorRetriesThenFails) {
    RetryConfig cfg;
    cfg.max_attempts = 4;
    cfg.base_delay   = std::chrono::milliseconds(5);
    cfg.max_delay    = std::chrono::milliseconds(20);
    cfg.jitter_ratio = 0.0;

    int calls = 0;
    auto elapsed = MeasureMs([&] {
        int rc = RunWithRetry(cfg, [&](int) {
            ++calls;
            return -7;  // CURLE_COULDNT_CONNECT style transport error
        });
        EXPECT_EQ(rc, -7);
    });

    EXPECT_EQ(calls, 4);
    // total backoff: attempt 0,1,2 -> 5ms + 10ms + 20ms = 35ms. 5ms tolerance.
    EXPECT_GE(elapsed.count(), 30);
    EXPECT_LE(elapsed.count(), 60);
}

TEST(RunWithRetry, Http429RetryThen400Abort) {
    RetryConfig cfg;
    cfg.max_attempts = 5;
    cfg.base_delay   = std::chrono::milliseconds(3);
    cfg.max_delay    = std::chrono::milliseconds(20);
    cfg.jitter_ratio = 0.0;
    cfg.retry_on_http = {429, 500, 502, 503, 504};

    std::vector<int> attempts;
    int rc = RunWithRetry(cfg, [&](int att) -> int {
        attempts.push_back(att);
        // first two return 429 -> retry; third returns 400 (not in retry whitelist)
        if (attempts.size() <= 2) return 429;
        return 400;
    });

    EXPECT_EQ(rc, 400);
    ASSERT_EQ(attempts.size(), 3u);
    EXPECT_EQ(attempts[0], 0);
    EXPECT_EQ(attempts[1], 1);
    EXPECT_EQ(attempts[2], 2);
}

TEST(RunWithRetry, MaxAttempts1NoRetry) {
    RetryConfig cfg;
    cfg.max_attempts = 1;
    int calls = 0;
    int rc = RunWithRetry(cfg, [&](int att) {
        ++calls;
        EXPECT_EQ(att, 0);
        return 500;
    });
    EXPECT_EQ(rc, 500);
    EXPECT_EQ(calls, 1);
}

TEST(RunWithRetry, EventuallySucceedsAfterRetries) {
    RetryConfig cfg;
    cfg.max_attempts = 5;
    cfg.base_delay   = std::chrono::milliseconds(2);
    cfg.max_delay    = std::chrono::milliseconds(20);
    cfg.jitter_ratio = 0.0;

    int calls = 0;
    int rc = RunWithRetry(cfg, [&](int att) {
        ++calls;
        if (att < 3) return 502;  // first 3 attempts return 502 gateway error
        return 0;
    });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(calls, 4);
}

// ===========================================================================
// WithRetry (expected<T, string> — dispatch-required version)
// ===========================================================================
TEST(WithRetryIntConvention, ZeroMeansSuccess) {
    RetryConfigLite cfg;
    cfg.max_attempts = 3;
    cfg.base_delay_ms = 2;
    cfg.max_delay_ms  = 10;
    auto result = WithRetry(cfg, []() -> int { return 0; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST(WithRetryIntConvention, RetryOnListedHttpStatus) {
    RetryConfigLite cfg;
    cfg.max_attempts = 4;
    cfg.base_delay_ms = 1;
    cfg.max_delay_ms  = 10;
    int calls = 0;
    auto result = WithRetry(cfg, [&]() -> int {
        ++calls;
        if (calls < 4) return 500;
        return 200;
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 200);
    EXPECT_EQ(calls, 4);
}

TEST(WithRetryIntConvention, NonRetryStatus) {
    RetryConfigLite cfg;
    cfg.max_attempts = 5;
    cfg.base_delay_ms = 1;
    cfg.max_delay_ms  = 10;
    int calls = 0;
    auto result = WithRetry(cfg, [&]() -> int {
        ++calls;
        return 401;   // not in default retry_on_http list
    });
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result.error(), "rc=401");
}

TEST(WithRetryExpectedStyle, ValueReturnedDirectly) {
    RetryConfigLite cfg;
    auto result = WithRetry(cfg, []() -> std::expected<std::string, std::string> {
        return std::expected<std::string, std::string>("hello");
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello");
}

TEST(WithRetryExpectedStyle, RetriesOnUnexpected) {
    RetryConfigLite cfg;
    cfg.max_attempts = 3;
    cfg.base_delay_ms = 1;
    cfg.max_delay_ms  = 5;
    int calls = 0;
    auto result = WithRetry(cfg, [&]() -> std::expected<int, std::string> {
        ++calls;
        if (calls < 3) return std::unexpected(std::string("not yet"));
        return std::expected<int, std::string>(42);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
    EXPECT_EQ(calls, 3);
}

TEST(WithRetryExpectedStyle, ExhaustedRetainsLastError) {
    RetryConfigLite cfg;
    cfg.max_attempts = 2;
    cfg.base_delay_ms = 1;
    cfg.max_delay_ms  = 5;
    auto result = WithRetry(cfg, []() -> std::expected<int, std::string> {
        return std::unexpected(std::string("boom"));
    });
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "boom");
}

TEST(WithRetryExceptionCaptured, StdException) {
    RetryConfigLite cfg;
    cfg.max_attempts = 1;
    auto result = WithRetry(cfg, []() -> int {
        throw std::runtime_error("oh no");
    });
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("oh no"), std::string::npos);
}

TEST(WithRetryExceptionCaptured, NonStdException) {
    RetryConfigLite cfg;
    cfg.max_attempts = 1;
    auto result = WithRetry(cfg, []() -> int {
        throw 42;
    });
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "exception: unknown");
}

// ===========================================================================
// Lite backoff semantics: base_delay_ms * 2^attempt
// ===========================================================================
TEST(WithRetryConfigLite, BackoffMatchesExpectation) {
    RetryConfigLite cfg;
    cfg.max_attempts = 1;
    cfg.base_delay_ms = 10;
    cfg.max_delay_ms = 1000;

    int calls = 0;
    auto elapsed = MeasureMs([&] {
        // 1 failed call, executes 1 attempt immediately, no backoff (sleep only after attempt=0)
        auto result = WithRetry(cfg, [&]() -> int {
            ++calls;
            return -1;
        });
        EXPECT_FALSE(result.has_value());
    });

    // max_attempts = 1 -> only 1 fn invocation, no backoff in between
    EXPECT_EQ(calls, 1);
    EXPECT_LT(elapsed.count(), 100);
}
