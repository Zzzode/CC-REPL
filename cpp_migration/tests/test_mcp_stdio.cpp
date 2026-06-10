/// @file test_mcp_stdio.cpp
/// @brief Phase 3-D: Unit tests for cc.services.mcp.stdio (StdioTransport).
///
/// Uses a real POSIX fork() subprocess (/bin/sh echo loop) as the mock MCP
/// server so the Start → SendJsonRpc → on_incoming_message → Stop lifecycle
/// is exercised end to end.  yyjson is used to build request bodies and
/// inspect parsed replies.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <errno.h>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

import cc.services.mcp.stdio;

using namespace cc::services::mcp::stdio;
using namespace std::chrono_literals;

namespace {

// Small helper: record every message + exit event, with a CV to wait on N
// messages being delivered.
struct Recorder {
    std::mutex                                 mtx;
    std::condition_variable                    cv;
    std::vector<JsonRpcMessage>                messages;
    bool                                       exit_fired = false;
    int                                        exit_code  = -9999;
    std::string                                exit_error;

    StdioTransport make_transport(size_t wait_for_n = 1,
                                  std::chrono::milliseconds max_wait = 5s) {
        StdioTransport t;
        t.on_incoming_message = [this, wait_for_n](JsonRpcMessage m) {
            std::lock_guard<std::mutex> l(mtx);
            messages.push_back(std::move(m));
            if (messages.size() >= wait_for_n) cv.notify_all();
        };
        t.on_exit = [this](int c, std::string e) {
            std::lock_guard<std::mutex> l(mtx);
            exit_fired = true;
            exit_code  = c;
            exit_error = std::move(e);
            cv.notify_all();
        };
        (void)max_wait;
        return t;
    }

    bool wait_messages(size_t n, std::chrono::milliseconds timeout = 3s) {
        std::unique_lock<std::mutex> l(mtx);
        return cv.wait_for(l, timeout, [&]{ return messages.size() >= n; });
    }
};

} // namespace

// ===========================================================================
// TEST 1: Mock echo server — Start + SendJsonRpc + parse response
// ===========================================================================
TEST(McpStdio, EchoServerRoundTrip) {
    Recorder rec;
    StdioTransport t = rec.make_transport(1);
    ASSERT_TRUE(t.Start(MockEchoSpec()));
    ASSERT_TRUE(t.IsRunning());
    EXPECT_GT(t.ChildPid(), 0);

    // Build a minimal JSON-RPC request.
    const std::string body = cc::services::mcp::stdio::BuildInitializeRequest(
        42, "cc-repl-cpp", "1.0.0");
    ASSERT_TRUE(t.SendJsonRpc(body));

    // The echo server echoes back the line verbatim.
    ASSERT_TRUE(rec.wait_messages(1, 3s)) << "timeout waiting for echo";

    ASSERT_EQ(rec.messages.size(), 1u);
    const auto& m = rec.messages[0];
    EXPECT_TRUE(m.is_valid);
    EXPECT_EQ(m.id, 42);
    EXPECT_EQ(m.method, "initialize");
    EXPECT_FALSE(m.params_json.empty());
    // params should contain clientInfo + name.
    EXPECT_NE(m.params_json.find("cc-repl-cpp"), std::string::npos);
    EXPECT_NE(m.params_json.find("1.0.0"), std::string::npos);

    t.Stop();
    EXPECT_FALSE(t.IsRunning());
}

// ===========================================================================
// TEST 2: SendJsonRpc with raw JSON, numeric id round-trip
// ===========================================================================
TEST(McpStdio, SendRawJsonRoundTrip) {
    Recorder rec;
    StdioTransport t = rec.make_transport(1);
    ASSERT_TRUE(t.Start(MockEchoSpec()));

    const std::string req = R"({"jsonrpc":"2.0","id":7,"method":"ping","params":{"hello":"world"}})";
    ASSERT_TRUE(t.SendJsonRpc(req));
    ASSERT_TRUE(rec.wait_messages(1, 3s));

    const auto& m = rec.messages[0];
    EXPECT_TRUE(m.is_valid);
    EXPECT_EQ(m.id, 7);
    EXPECT_EQ(m.method, "ping");
    EXPECT_NE(m.params_json.find("hello"), std::string::npos);
    EXPECT_NE(m.params_json.find("world"), std::string::npos);
    t.Stop();
}

// ===========================================================================
// TEST 3: Multiple messages — 3 pings arrive in order
// ===========================================================================
TEST(McpStdio, ThreeMessagesInOrder) {
    Recorder rec;
    StdioTransport t = rec.make_transport(3);
    ASSERT_TRUE(t.Start(MockEchoSpec()));

    for (int i = 1; i <= 3; ++i) {
        char body[128];
        std::snprintf(body, sizeof(body),
                      R"({"jsonrpc":"2.0","id":%d,"method":"m%d"})", i, i);
        ASSERT_TRUE(t.SendJsonRpc(std::string_view(body)));
        // Short yield to allow the reader thread to drain (not strictly
        // required but keeps macOS CI from coalescing the writes).
        std::this_thread::sleep_for(5ms);
    }

    ASSERT_TRUE(rec.wait_messages(3, 3s));
    ASSERT_EQ(rec.messages.size(), 3u);
    EXPECT_EQ(rec.messages[0].id, 1);
    EXPECT_EQ(rec.messages[0].method, "m1");
    EXPECT_EQ(rec.messages[1].id, 2);
    EXPECT_EQ(rec.messages[1].method, "m2");
    EXPECT_EQ(rec.messages[2].id, 3);
    EXPECT_EQ(rec.messages[2].method, "m3");
    t.Stop();
}

// ===========================================================================
// TEST 4: Stop() reaps the child — no zombies
// ===========================================================================
TEST(McpStdio, StopKillsChildAndFiresExit) {
    Recorder rec;
    StdioTransport t = rec.make_transport(0);
    ASSERT_TRUE(t.Start(MockEchoSpec()));
    const pid_t pid = t.ChildPid();
    ASSERT_GT(pid, 0);

    // Send nothing; the echo loop is blocked waiting for stdin.
    // Stop must send SIGTERM (→ child's exit == -SIGTERM on Linux or 143
    // depending on implementation).  On macOS, the shell's read gets EINTR
    // when SIGTERM arrives, the loop terminates, and the shell exits 0.
    t.Stop();

    // Poll the exit signal with a timeout.
    {
        std::unique_lock<std::mutex> l(rec.mtx);
        EXPECT_TRUE(rec.cv.wait_for(l, 2s, [&]{ return rec.exit_fired; }))
            << "on_exit should fire during Stop";
    }
    EXPECT_TRUE(rec.exit_fired);

    // Reap check: kill(pid, 0) with ESRCH means the pid is gone.
    int ret = ::kill(pid, 0);
    EXPECT_TRUE((ret < 0 && errno == ESRCH) || (ret == 0))
        << "child should be reaped (got ret=" << ret << " errno=" << errno << ")";
}

// ===========================================================================
// TEST 5: Watchdog timeout kills a child that produces no output
// ===========================================================================
TEST(McpStdio, WatchdogKillsSilentChild) {
    Recorder rec;
    StdioTransport t = rec.make_transport(0);
    StdioServerSpec spec;
    // Sleep far longer than the watchdog.  Watchdog should end it in 200ms.
    spec.command = "/bin/sleep";
    spec.args    = {"60"};
    spec.timeout_ms = 200;

    auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t.Start(spec));

    {
        std::unique_lock<std::mutex> l(rec.mtx);
        rec.cv.wait_for(l, 5s, [&]{ return rec.exit_fired; });
    }
    auto dt = std::chrono::steady_clock::now() - t0;

    EXPECT_TRUE(rec.exit_fired);
    // Should have fired long before the 60s sleep completed.
    EXPECT_LT(dt, 5s);
    // Either the watchdog fired on_exit(-ETIMEDOUT) or the sleep was killed.
    if (rec.exit_code == -ETIMEDOUT) {
        EXPECT_NE(rec.exit_error.find("watchdog"), std::string::npos);
    } else {
        // Accept any non-9999 termination code.
        EXPECT_NE(rec.exit_code, -9999);
    }
    EXPECT_FALSE(t.IsRunning());
    t.Stop();
}

// ===========================================================================
// TEST 6: Invalid JSON lines still trigger on_incoming_message (is_valid=false)
// ===========================================================================
TEST(McpStdio, InvalidJsonSurfacedAsInvalidMessage) {
    Recorder rec;
    StdioTransport t = rec.make_transport(2);
    // Echo server repeats anything we send.
    ASSERT_TRUE(t.Start(MockEchoSpec()));

    // Send a valid line first, then garbage.
    (void)t.SendJsonRpc(R"({"jsonrpc":"2.0","id":1,"method":"good"})");
    std::this_thread::sleep_for(10ms);
    (void)t.SendJsonRpc("this is not json at all !!!");
    std::this_thread::sleep_for(10ms);

    ASSERT_TRUE(rec.wait_messages(2, 3s));
    ASSERT_EQ(rec.messages.size(), 2u);
    EXPECT_TRUE(rec.messages[0].is_valid);
    EXPECT_EQ(rec.messages[0].id, 1);
    EXPECT_FALSE(rec.messages[1].is_valid);
    EXPECT_TRUE(rec.messages[1].error_message.has_value());
    t.Stop();
}

// ===========================================================================
// TEST 7: BuildToolsListRequest → parsed id/method match
// ===========================================================================
TEST(McpStdio, BuildToolsListRequestRoundTrip) {
    Recorder rec;
    StdioTransport t = rec.make_transport(1);
    ASSERT_TRUE(t.Start(MockEchoSpec()));
    ASSERT_TRUE(t.SendJsonRpc(BuildToolsListRequest(99)));
    ASSERT_TRUE(rec.wait_messages(1, 3s));
    ASSERT_EQ(rec.messages.size(), 1u);
    EXPECT_EQ(rec.messages[0].id, 99);
    EXPECT_EQ(rec.messages[0].method, "tools/list");
    t.Stop();
}

// ===========================================================================
// TEST 8: StdioServerSpec env override reaches the child
// ===========================================================================
TEST(McpStdio, EnvOverrideInheritedByChild) {
    Recorder rec;
    StdioTransport t = rec.make_transport(1);
    StdioServerSpec spec;
    // `env` overrides then `printenv MCP_TEST_VAR`
    spec.command = "/bin/sh";
    spec.args    = {"-c", "echo \"{\\\"jsonrpc\\\":\\\"2.0\\\",\\\"id\\\":1,\\\"method\\\":\\\"echo\\\",\\\"params\\\":{\\\"v\\\":\\\"$MCP_TEST_VAR\\\"}}\""};
    spec.env     = {{"MCP_TEST_VAR", "hello_from_env"}};

    ASSERT_TRUE(t.Start(spec));
    // The shell script echoes a single JSON line then exits.
    ASSERT_TRUE(rec.wait_messages(1, 3s));
    ASSERT_EQ(rec.messages.size(), 1u);
    EXPECT_TRUE(rec.messages[0].is_valid);
    EXPECT_NE(rec.messages[0].params_json.find("hello_from_env"), std::string::npos);
    t.Stop();
}

// ===========================================================================
// TEST 9: Double Start() is a no-op returning false
// ===========================================================================
TEST(McpStdio, DoubleStartIsNoOp) {
    StdioTransport t;
    ASSERT_TRUE(t.Start(MockEchoSpec()));
    const pid_t first = t.ChildPid();
    EXPECT_FALSE(t.Start(MockEchoSpec()));
    EXPECT_EQ(t.ChildPid(), first);
    t.Stop();
}

// ===========================================================================
// TEST 10: SendJsonRpc before Start → returns false, no crash
// ===========================================================================
TEST(McpStdio, SendBeforeStartFailsGracefully) {
    StdioTransport t;
    EXPECT_FALSE(t.SendJsonRpc(R"({"id":1})"));
    EXPECT_FALSE(t.IsRunning());
}
