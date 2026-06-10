/// @file test_sse_mock.cpp
/// @brief Phase 3-E2E: Real HTTP Mock Server + SseClient E2E tests

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <httplib.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import cc.services.api.sse;

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
