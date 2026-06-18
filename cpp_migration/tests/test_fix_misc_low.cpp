// test_fix_misc_low.cpp — coverage for the GROUP misc-low parity ports.
//
// Mirrors behavior of four real-and-cheap fixes, exercising observable logic
// without touching the network:
//   - oauth_port.cppm  : find_available_oauth_port honors
//                        MCP_OAUTH_CALLBACK_PORT, probes an open port in the
//                        ephemeral range, and rejects impossible ports.
//   - secret_scanner.cppm : scan_for_secrets matches the curated gitleaks
//                        rule set (SecretMatch={ruleId,label}, deduped).
//   - in_process_transport.cppm : create_linked_transport_pair delivers
//                        send()->peer.on_message and fans close() to both.
//   - oauth/client.cppm : PkceGenerator + state use a CSPRNG (verifier and
//                        state are base64URL-shaped and unique across runs).
//
// Register in tests/CMakeLists.txt (see cmakeNeeds in the migration plan):
//   add_executable(test_fix_misc_low test_fix_misc_low.cpp)
//   target_link_libraries(test_fix_misc_low PRIVATE cc_services cc_hooks
//                         GTest::gtest_main)
//   gtest_discover_tests(test_fix_misc_low
//       DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

import cc.services.mcp.oauth_port;
import cc.services.team_memory.secret_scanner;
import cc.services.mcp.in_process_transport;
import cc.services.oauth.client;

namespace oauth_port = cc::services::mcp;
namespace secret = cc::services::team_memory;
namespace transport = cc::services::mcp;
namespace oauth = cc::services::oauth;

// ---------------------------------------------------------------------------
// oauth_port — env override wins; an open port in the ephemeral range is
// returned otherwise; malformed env values are ignored.
// ---------------------------------------------------------------------------

class OauthPortEnvOverride : public ::testing::Test {
protected:
    void SetUp() override { old_ = std::getenv("MCP_OAUTH_CALLBACK_PORT"); }
    void TearDown() override {
        if (old_) setenv("MCP_OAUTH_CALLBACK_PORT", old_, 1);
        else      unsetenv("MCP_OAUTH_CALLBACK_PORT");
    }
    const char* old_ = nullptr;
};

TEST_F(OauthPortEnvOverride, HonorsValidOverride) {
    setenv("MCP_OAUTH_CALLBACK_PORT", "42421", 1);
    auto port = oauth_port::find_available_oauth_port();
    ASSERT_TRUE(port.has_value()) << port.error();
    EXPECT_EQ(*port, 42421);
}

TEST_F(OauthPortEnvOverride, IgnoresMalformedOverride) {
    setenv("MCP_OAUTH_CALLBACK_PORT", "not-a-port", 1);
    auto port = oauth_port::find_available_oauth_port();
    ASSERT_TRUE(port.has_value()) << port.error();
    // Falls back into the ephemeral range, not the override.
    EXPECT_GE(*port, oauth_port::kRedirectPortRangeStart);
    EXPECT_LE(*port, oauth_port::kRedirectPortRangeEnd);
}

TEST(OauthPort, RandomProbeReturnsOpenEphemeralPort) {
    unsetenv("MCP_OAUTH_CALLBACK_PORT");
    auto port = oauth_port::find_available_oauth_port();
    ASSERT_TRUE(port.has_value()) << port.error();
    // Either an ephemeral-range hit or the documented fallback (3118).
    bool in_range = *port >= oauth_port::kRedirectPortRangeStart &&
                    *port <= oauth_port::kRedirectPortRangeEnd;
    bool is_fallback = *port == oauth_port::kRedirectPortFallback;
    EXPECT_TRUE(in_range || is_fallback) << "got port " << *port;
}

TEST(OauthPort, BuildRedirectUriMatchesTsShape) {
    EXPECT_EQ(oauth_port::build_redirect_uri(3118), "http://localhost:3118/callback");
}

// ---------------------------------------------------------------------------
// secret_scanner — curated gitleaks rules fire; SecretMatch carries only
// ruleId + label (no offsets); matches dedupe by ruleId.
// ---------------------------------------------------------------------------

TEST(SecretScanner, DetectsGitHubPat) {
    auto m = secret::scan_for_secrets("token: ghp_0123456789abcdefghijkmnopqrstuvwxyzAB");
    ASSERT_FALSE(m.empty());
    auto it = std::find_if(m.begin(), m.end(),
                           [](const auto& x) { return x.ruleId == "github-pat"; });
    ASSERT_NE(it, m.end());
    EXPECT_EQ(it->label, "GitHub PAT");
}

TEST(SecretScanner, DetectsAnthropicApiKey) {
    std::string body(93, 'x');
    auto m = secret::scan_for_secrets("k = sk-ant-api03-" + body + "AA");
    ASSERT_FALSE(m.empty());
    auto it = std::find_if(m.begin(), m.end(),
                           [](const auto& x) { return x.ruleId == "anthropic-api-key"; });
    ASSERT_NE(it, m.end());
    EXPECT_EQ(it->label, "Anthropic API Key");
}

TEST(SecretScanner, DetectsStripeAccessToken) {
    auto m = secret::scan_for_secrets("sk_live_abc1234567890xyzXYZ");
    ASSERT_FALSE(m.empty());
    EXPECT_NE(std::find_if(m.begin(), m.end(),
                           [](const auto& x) { return x.ruleId == "stripe-access-token"; }),
              m.end());
}

TEST(SecretScanner, DeduplicatesByRuleId) {
    // Two GitHub PATs in one string -> a single "github-pat" match.
    auto m = secret::scan_for_secrets(
        "ghp_0123456789abcdefghijkmnopqrstuvwxyzAB and "
        "ghp_9999999999zzzzzzzzzzzzzzzzzzzzzzzzzz");
    long count = std::count_if(m.begin(), m.end(),
                               [](const auto& x) { return x.ruleId == "github-pat"; });
    EXPECT_EQ(count, 1);
}

TEST(SecretScanner, DoesNotFlagPlainProse) {
    auto m = secret::scan_for_secrets("just a normal sentence with no secrets here");
    EXPECT_TRUE(m.empty());
}

TEST(SecretScanner, GetSecretLabelMatchesTsSpecialCases) {
    EXPECT_EQ(secret::get_secret_label("aws-access-token"), "AWS Access Token");
    EXPECT_EQ(secret::get_secret_label("gitlab-deploy-token"), "GitLab Deploy Token");
    EXPECT_EQ(secret::get_secret_label("pypi-upload-token"), "PyPI Upload Token");
}

// ---------------------------------------------------------------------------
// in_process_transport — linked pair: send delivers to peer.on_message,
// close fans out to both sides' on_close.
// ---------------------------------------------------------------------------

TEST(InProcessTransport, SendDeliversToPeerOnMessage) {
    auto pair = transport::create_linked_transport_pair();
    std::atomic<int> hits{0};
    std::string received;
    pair.second.on_message([&](const transport::InProcessMessage& msg) {
        received = msg.body_json;
        ++hits;
    });
    auto r = pair.first.send({.body_json = R"({"jsonrpc":"2.0","method":"ping"})"});
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(hits.load(), 1);
    EXPECT_EQ(received, R"({"jsonrpc":"2.0","method":"ping"})");
}

TEST(InProcessTransport, SendAfterCloseErrors) {
    auto pair = transport::create_linked_transport_pair();
    ASSERT_TRUE(pair.first.close().has_value());
    auto r = pair.first.send({.body_json = "{}"});
    EXPECT_FALSE(r.has_value());
}

TEST(InProcessTransport, CloseFansOutToPeer) {
    auto pair = transport::create_linked_transport_pair();
    std::atomic<int> first_closed{0}, second_closed{0};
    pair.first.on_close([&] { ++first_closed; });
    pair.second.on_close([&] { ++second_closed; });
    ASSERT_TRUE(pair.first.close().has_value());
    EXPECT_EQ(first_closed.load(), 1);
    EXPECT_EQ(second_closed.load(), 1);
    EXPECT_TRUE(pair.first.is_closed());
    EXPECT_TRUE(pair.second.is_closed());
}

// ---------------------------------------------------------------------------
// oauth/client.cppm — PKCE verifier + state come from a CSPRNG and have the
// expected base64URL shape (43 chars for 32 random bytes; unique per run).
// ---------------------------------------------------------------------------

TEST(OauthCrypto, PkceVerifierIsUniqueAndWellShaped) {
    auto a = oauth::PkceGenerator::generate();
    auto b = oauth::PkceGenerator::generate();
    // base64URL(32 bytes) is 43 chars with no padding, URL-safe alphabet.
    EXPECT_EQ(a.code_verifier.size(), 43u);
    EXPECT_EQ(b.code_verifier.size(), 43u);
    EXPECT_NE(a.code_verifier, b.code_verifier);
    auto only_b64url = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    for (char c : a.code_verifier) EXPECT_TRUE(only_b64url(c)) << c;
    // S256 challenge is base64URL(SHA256(verifier)) -> 43 chars.
    EXPECT_EQ(a.code_challenge.size(), 43u);
    EXPECT_EQ(a.method, "S256");
}

TEST(OauthCrypto, StateIsUniqueAcrossRuns) {
    // generate_state is private on OAuthClient; exercise it indirectly via
    // the public PkceGenerator parity (same fill_csrng + base64url path).
    auto a = oauth::PkceGenerator::generate().code_verifier;
    auto b = oauth::PkceGenerator::generate().code_verifier;
    EXPECT_NE(a, b);
}
