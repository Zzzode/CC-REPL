/// @file test_sse_mock.cpp
/// @brief Phase 3-E2E: Real HTTP Mock Server + SseClient E2E tests

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <httplib.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import cc.services.api.sse;
import cc.cli.sse_transport;

using namespace cc::services::api::sse;

namespace {

int StartMockAndGetPort(httplib::Server& svr) {
    int port = svr.bind_to_any_port("127.0.0.1");
    return port >= 0 ? port : -1;
}

std::string BuildAnthropicSseBody() {
    std::string out;
    out += "event: message_start\n"
           "data: {\"message\":{\"id\":\"msg_1\",\"type\":\"message\","
           "\"role\":\"assistant\",\"model\":\"claude-sonnet-4-20250514\","
           "\"content\":[],\"stop_reason\":null,\"stop_sequence\":null,"
           "\"usage\":{\"input_tokens\":10,\"output_tokens\":0}}}\n\n";
    out += "event: content_block_start\n"
           "data: {\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
    out += "event: content_block_delta\n"
           "data: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello \"}}\n\n";
    out += "event: content_block_delta\n"
           "data: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"beautiful \"}}\n\n";
    out += "event: content_block_delta\n"
           "data: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"wonderful \"}}\n\n";
    out += "event: content_block_delta\n"
           "data: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"world!?\"}}\n\n";
    out += "event: content_block_stop\n"
           "data: {\"index\":0}\n\n";
    out += "event: message_delta\n"
           "data: {\"type\":\"message_delta\",\"delta\":"
           "{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},"
           "\"usage\":{\"output_tokens\":28}}\n\n";
    out += "event: message_stop\n"
           "data: {\"type\":\"message_stop\"}\n\n";
    return out;
}

void WriteAnthropicSseResponse(const httplib::Request&, httplib::Response& res) {
    static const std::string kBody = BuildAnthropicSseBody();
    res.set_header("Cache-Control", "no-cache");
    res.set_content(kBody, "text/event-stream");
}

// RAII: start server on background thread; stop+join on destruction.
struct MockServerRAII {
    httplib::Server svr;
    std::thread    th;
    int            port = -1;

    explicit MockServerRAII(httplib::Server::Handler handler) {
        svr.Post("/v1/messages", std::move(handler));
        port = StartMockAndGetPort(svr);
        if (port < 0) return;
        std::atomic<bool> up{false};
        th = std::thread([&, this] {
            up.store(true, std::memory_order_release);
            svr.listen_after_bind();
        });
        while (!up.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // macOS localhost loopback sometimes needs extra time to enter accept loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~MockServerRAII() {
        if (port >= 0) {
            svr.stop();
            if (th.joinable()) th.join();
        }
    }
};

}  // namespace

// ===========================================================================
// TEST 1: Dry-run (empty api_key) → zero network, instant on_final
// ===========================================================================
TEST(SseClientE2E, DryRunDoesNotGoToNetwork) {
    SseClient client(ApiConfig{.base_url = "http://127.0.0.1:1/",
                               .api_key = "",
                               .model_id = "x"});
    std::vector<TextMessage> msgs = {{Role::User, "ping"}};

    int  final_http   = -1;
    bool final_fired  = false;
    int  delta_calls  = 0;
    bool abort_polled = false;

    StreamingCallbacks cbs;
    cbs.on_text_delta = [&](std::string_view) { ++delta_calls; };
    cbs.should_abort  = [&]() { abort_polled = true; return false; };
    cbs.on_final = [&](int http, std::string_view stop,
                       std::string_view accum, std::string_view err) {
        final_http  = http;
        final_fired = true;
        EXPECT_EQ(stop, "dry_run");
        EXPECT_EQ(accum, "");
        EXPECT_EQ(err, "[dry-run] no api key provided");
    };

    int rc = client.PostMessagesStream("system", msgs, std::move(cbs));
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(final_fired);
    EXPECT_EQ(final_http, 0);
    EXPECT_EQ(delta_calls, 0);
    EXPECT_FALSE(abort_polled);
}

// ===========================================================================
// TEST 2: Localhost mock → StreamingCallbacks
// ===========================================================================
TEST(SseClientE2E, LocalhostMockStreamingCallbacks) {
    MockServerRAII mock(&WriteAnthropicSseResponse);
    ASSERT_GE(mock.port, 0);

    ApiConfig cfg;
    cfg.base_url = std::string("http://127.0.0.1:") + std::to_string(mock.port) + "/v1";
    cfg.api_key  = "sk-fake-key";
    cfg.model_id = "claude-sonnet-4-20250514";
    SseClient client(std::move(cfg));

    std::vector<TextMessage> msgs;
    msgs.push_back({Role::User, "say hi"});
    msgs.push_back({Role::Assistant, "ok"});
    msgs.push_back({Role::User, "say hello"});

    std::vector<std::string>   deltas;
    std::vector<SseEventType>  events;
    int                        final_http = -1;
    std::string                stop_reason;
    std::string                accum_text;
    std::string                err_reason;

    StreamingCallbacks cbs;
    cbs.on_sse_event  = [&](SseEventType t, std::string_view) { events.push_back(t); };
    cbs.on_text_delta = [&](std::string_view d) { deltas.emplace_back(d); };
    cbs.on_final = [&](int http, std::string_view stop,
                       std::string_view accum, std::string_view err) {
        final_http  = http;
        stop_reason = std::string(stop);
        accum_text  = std::string(accum);
        err_reason  = std::string(err);
    };

    int rc = client.PostMessagesStream("you are helpful", msgs, std::move(cbs));

    // Core invariants: HTTP 200, SSE parser delivered the complete text.
    EXPECT_EQ(rc, 200);
    EXPECT_EQ(final_http, 200);
    EXPECT_EQ(err_reason, "");
    EXPECT_FALSE(accum_text.empty());
    EXPECT_NE(accum_text.find("Hello "),     std::string::npos);
    EXPECT_NE(accum_text.find("beautiful "), std::string::npos);
    EXPECT_NE(accum_text.find("wonderful "), std::string::npos);
    EXPECT_NE(accum_text.find("world!?"),   std::string::npos);
    EXPECT_EQ(stop_reason, "end_turn");
    // Sanity counts: parser must have seen at least the first message_start,
    // the first content_block_start, and ≥1 delta.
    EXPECT_GE(deltas.size(), 1u);
    EXPECT_GE(events.size(), 3u);
}

// ===========================================================================
// TEST 3: Localhost mock → 4-field SseCallbacks (dispatch-compatible API)
// ===========================================================================
TEST(SseClientE2E, LocalhostMockDispatchStyleCallbacks) {
    MockServerRAII mock(&WriteAnthropicSseResponse);
    ASSERT_GE(mock.port, 0);

    std::vector<std::string> event_names;
    std::vector<std::string> data_samples;
    bool        done       = false;
    int         err_http   = 0;
    std::string err_reason;

    SseCallbacks cbs;
    cbs.on_event = [&](std::string_view ev) { event_names.emplace_back(ev); };
    cbs.on_data  = [&](std::string_view d) {
        if (data_samples.size() < 4) data_samples.emplace_back(d);
    };
    cbs.on_error    = [&](int http, std::string r) { err_http = http; err_reason = std::move(r); };
    cbs.on_complete = [&] { done = true; };

    std::vector<Message> msgs = {Message{.role = "user", .text_content = "hello"}};
    const std::string base_url =
        std::string("http://127.0.0.1:") + std::to_string(mock.port) + "/v1";
    int rc = PostMessagesStream(base_url, "sk-fake", "system", msgs, std::move(cbs));

    EXPECT_EQ(rc, 200);
    EXPECT_TRUE(done);
    EXPECT_EQ(err_http, 0);
    EXPECT_TRUE(err_reason.empty());

    // Every mandatory Anthropic SSE event type must appear somewhere.
    auto has = [&](const char* n) {
        for (const auto& s : event_names) if (s == n) return true;
        return false;
    };
    EXPECT_TRUE(has("message_start"))       << "missing message_start";
    EXPECT_TRUE(has("content_block_start")) << "missing content_block_start";
    EXPECT_TRUE(has("content_block_delta")) << "missing content_block_delta";
    EXPECT_TRUE(has("content_block_stop"))  << "missing content_block_stop";
    EXPECT_TRUE(has("message_delta"))       << "missing message_delta";
    EXPECT_TRUE(has("message_stop"))        << "missing message_stop";

    bool found_msg1 = false;
    for (const auto& s : data_samples) {
        if (s.find("msg_1") != std::string::npos) { found_msg1 = true; break; }
    }
    EXPECT_TRUE(found_msg1);
}

// ===========================================================================
// TEST 4: HTTP 429 → on_error fires, on_complete does not
// ===========================================================================
TEST(SseClientE2E, MockHttp429TriggersError) {
    MockServerRAII mock([](const httplib::Request&, httplib::Response& r) {
        r.status = 429;
        r.set_content(
            "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\","
            "\"message\":\"Requests are rate-limited.\"}}",
            "application/json");
    });
    ASSERT_GE(mock.port, 0);

    SseCallbacks cbs;
    int         err_http   = 0;
    std::string err_reason;
    bool        complete   = false;
    cbs.on_error    = [&](int http, std::string r) { err_http = http; err_reason = std::move(r); };
    cbs.on_complete = [&] { complete = true; };

    const std::string base_url =
        std::string("http://127.0.0.1:") + std::to_string(mock.port) + "/v1";
    std::vector<Message> msgs = {Message{.role = "user", .text_content = "x"}};
    int rc = PostMessagesStream(base_url, "sk-fake", "", msgs, std::move(cbs));

    EXPECT_EQ(rc, 429);
    EXPECT_EQ(err_http, 429);
    EXPECT_FALSE(complete);
    // A non-SSE body (application/json) may or may not surface the inner JSON
    // message here; the authoritative signal is the HTTP status.
    EXPECT_TRUE(err_reason.empty() ||
                err_reason.find("rate-limited") != std::string::npos ||
                err_reason.find("HTTP 429") != std::string::npos);
}

// ===========================================================================
// TEST 5: FeedParser cross-chunk split + ResetParser (unit, no network)
// ===========================================================================
TEST(SseParser, CrossChunkEventSplit) {
    SseClient client(ApiConfig{.api_key = "unused"});
    StreamingCallbacks cbs;

    std::vector<std::pair<SseEventType, std::string>> events;
    std::string accum;
    cbs.on_sse_event  = [&](SseEventType t, std::string_view d) { events.emplace_back(t, std::string(d)); };
    cbs.on_text_delta = [&](std::string_view d) { accum.append(d); };

    // Split inside the JSON payload: "Hel" + "lo " within the `text` field.
    // The SSE event line is whole (so the parser can identify the event).
    std::string_view a = "event: content_block_delta\n"
                         "data: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hel";
    std::string_view b = "lo \"}}\n\n";

    client.FeedParser(a, cbs);
    client.FeedParser(b, cbs);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].first, SseEventType::ContentBlockDelta);
    EXPECT_NE(events[0].second.find("Hello "), std::string::npos);
    EXPECT_EQ(accum, "Hello ");

    client.ResetParser();
    events.clear();
    client.FeedParser("event: ping\ndata: hello\n\n", cbs);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].first, SseEventType::Ping);
    EXPECT_EQ(events[0].second, "hello");
}

// ===========================================================================
// SSETransport TLS: an https:// endpoint must perform a real TLS handshake.
// Regression guard — the transport previously parsed "https://" but
// connected with raw BSD sockets, so no handshake ever happened.
// ===========================================================================
namespace {

// Resolve tests/fixtures/<name> relative to THIS file so the test works
// regardless of ctest's working directory.
std::string sse_fixture_path(const char* name) {
    std::filesystem::path here(__FILE__);
    return (here.parent_path() / "fixtures" / name).string();
}

// Self-signed fixture certificate is issued for IP 127.0.0.1; pointing the
// process CA bundle at it lets hostname/IP verification succeed.
struct CaBundleGuard {
    std::string previous;
    bool had_previous = false;
    explicit CaBundleGuard(const std::string& path) {
        if (const char* v = std::getenv("LOOM_CA_BUNDLE"); v) {
            previous = v;
            had_previous = true;
        }
        setenv("LOOM_CA_BUNDLE", path.c_str(), 1);
    }
    ~CaBundleGuard() {
        if (had_previous) setenv("LOOM_CA_BUNDLE", previous.c_str(), 1);
        else unsetenv("LOOM_CA_BUNDLE");
    }
};

}  // namespace

TEST(SseTransportTls, HttpsEndpointCompletesHandshakeAndStreamsEvents) {
    const std::string cert = sse_fixture_path("sse_cert.pem");
    const std::string key = sse_fixture_path("sse_key.pem");
    ASSERT_TRUE(std::filesystem::exists(cert)) << cert;
    ASSERT_TRUE(std::filesystem::exists(key)) << key;

    httplib::SSLServer svr(cert.c_str(), key.c_str());
    ASSERT_TRUE(svr.is_valid());

    svr.Get("/stream", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_content("event: hello\ndata: {\"n\":1}\n\n", "text/event-stream");
    });

    const int port = svr.bind_to_any_port("127.0.0.1");
    ASSERT_GE(port, 0);
    std::atomic<bool> up{false};
    std::thread th([&] {
        up.store(true, std::memory_order_release);
        svr.listen_after_bind();
    });
    while (!up.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Trust the fixture CA for this test only.
    CaBundleGuard ca_guard(cert);

    cc::cli::SSETransport transport;
    std::vector<cc::cli::SSEEvent> received;
    std::vector<std::string> errors;
    transport.on_event([&](const cc::cli::SSEEvent& e) { received.push_back(e); });
    transport.on_error([&](std::string_view e) { errors.push_back(std::string(e)); });

    const std::string url =
        "https://127.0.0.1:" + std::to_string(port) + "/stream";
    auto connected = transport.connect(url);
    ASSERT_TRUE(connected.has_value()) << connected.error();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    transport.close();
    svr.stop();
    th.join();

    ASSERT_FALSE(received.empty())
        << "no SSE event over TLS; errors: "
        << (errors.empty() ? std::string{"(none)"} : errors.front());
    EXPECT_EQ(received.front().event, "hello");
    EXPECT_NE(received.front().data.find("\"n\":1"), std::string::npos);

    // No handshake/certificate error may have been reported.
    for (const auto& err : errors) {
        EXPECT_EQ(err.find("TLS"), std::string::npos) << err;
    }
}

TEST(SseTransportTls, HttpsWithUntrustedCertificateIsRejected) {
    const std::string cert = sse_fixture_path("sse_cert.pem");
    const std::string key = sse_fixture_path("sse_key.pem");
    ASSERT_TRUE(std::filesystem::exists(cert)) << cert;
    ASSERT_TRUE(std::filesystem::exists(key)) << key;

    httplib::SSLServer svr(cert.c_str(), key.c_str());
    ASSERT_TRUE(svr.is_valid());
    svr.Get("/stream", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("event: hello\ndata: x\n\n", "text/event-stream");
    });

    const int port = svr.bind_to_any_port("127.0.0.1");
    ASSERT_GE(port, 0);
    std::atomic<bool> up{false};
    std::thread th([&] {
        up.store(true, std::memory_order_release);
        svr.listen_after_bind();
    });
    while (!up.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Point verification at the SYSTEM bundle only: the self-signed fixture
    // is not in it, so verification must fail and no event may arrive.
    CaBundleGuard ca_guard("/etc/ssl/certs/ca-certificates.crt");
    unsetenv("SSL_CERT_FILE");

    cc::cli::SSETransport transport(cc::cli::SSEReconnectPolicy{
        .initial_delay = std::chrono::milliseconds(10),
        .max_delay = std::chrono::milliseconds(20),
        .backoff_multiplier = 1.0,
        .jitter_factor = 0.0,
        .max_retries = 1,
        .liveness_timeout = std::chrono::seconds(2),
    });
    std::vector<cc::cli::SSEEvent> received;
    std::vector<std::string> errors;
    transport.on_event([&](const cc::cli::SSEEvent& e) { received.push_back(e); });
    transport.on_error([&](std::string_view e) { errors.push_back(std::string(e)); });

    const std::string url =
        "https://127.0.0.1:" + std::to_string(port) + "/stream";
    auto connected = transport.connect(url);
    ASSERT_TRUE(connected.has_value()) << connected.error();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (errors.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    transport.close();
    svr.stop();
    th.join();

    EXPECT_TRUE(received.empty()) << "untrusted certificate must not yield data";
    ASSERT_FALSE(errors.empty());
    bool saw_tls_error = false;
    for (const auto& err : errors) {
        if (err.find("TLS") != std::string::npos) saw_tls_error = true;
    }
    EXPECT_TRUE(saw_tls_error) << errors.front();
}
