/// @file bench_core.cpp
/// @brief Google Benchmark micro-benchmark suite for CC-REPL core subsystems.
///
/// Benchmarks:
///   1. SSE parser      - Feed a pre-built SSE body through SseClient::FeedParser
///   2. JSON serialize  - Measure yyjson-based message body construction
///   3. Tool dispatch   - Measure registry.execute("Read", ...) on a small file
///   4. Backoff calc    - Measure BackoffDelay computation

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

import cc.services.api.sse;
import cc.services.api.with_retry_simple;
import cc.tools.tool;
import cc.tools.file_read;
import cc.utils.error;

namespace fs = std::filesystem;

using namespace cc::services::api::sse;
using namespace cc::services::api::with_retry_simple;

// =========================================================================
// Helpers
// =========================================================================

namespace {

/// Build a realistic multi-delta SSE body (mimics Anthropic streaming response)
std::string BuildBenchmarkSseBody() {
    std::string out;
    out.reserve(4096);
    out += "event: message_start\n"
           "data: {\"message\":{\"id\":\"msg_bench\",\"type\":\"message\","
           "\"role\":\"assistant\",\"model\":\"claude-sonnet-4-20250514\","
           "\"content\":[],\"stop_reason\":null,\"stop_sequence\":null,"
           "\"usage\":{\"input_tokens\":25,\"output_tokens\":0}}}\n\n";
    out += "event: content_block_start\n"
           "data: {\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";

    // Simulate 20 content_block_delta events (typical streaming response)
    for (int i = 0; i < 20; ++i) {
        out += "event: content_block_delta\n"
               "data: {\"index\":0,\"delta\":{\"type\":\"text_delta\","
               "\"text\":\"The quick brown fox jumps over the lazy dog. \"}}\n\n";
    }

    out += "event: content_block_stop\n"
           "data: {\"index\":0}\n\n";
    out += "event: message_delta\n"
           "data: {\"type\":\"message_delta\",\"delta\":"
           "{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},"
           "\"usage\":{\"output_tokens\":200}}\n\n";
    out += "event: message_stop\n"
           "data: {\"type\":\"message_stop\"}\n\n";
    return out;
}

/// Create a temporary small file for the file-read benchmark
struct TempFile {
    fs::path path;

    TempFile() {
        path = fs::temp_directory_path() / "bench_core_tmp.txt";
        std::ofstream ofs(path);
        for (int i = 1; i <= 50; ++i) {
            ofs << "Line " << i << ": Lorem ipsum dolor sit amet, consectetur.\n";
        }
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

} // namespace

// =========================================================================
// BM_SseParser: Feed a pre-built SSE body through FeedParser
// =========================================================================

static void BM_SseParser(benchmark::State& state) {
    const std::string sse_body = BuildBenchmarkSseBody();
    ApiConfig cfg;
    cfg.api_key = "";  // dry-run, but we only use FeedParser directly
    SseClient client(std::move(cfg));

    for (auto _ : state) {
        client.ResetParser();
        std::string accum, stop, err;
        StreamingCallbacks cbs;
        cbs.on_text_delta = [](std::string_view) {};
        client.FeedParser(sse_body, cbs, accum, stop, err);
        benchmark::DoNotOptimize(accum);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(sse_body.size()));
}
BENCHMARK(BM_SseParser);

// =========================================================================
// BM_JsonSerialize: Measure yyjson-based message body construction
// (Uses PostMessagesStream dry-run path which calls BuildMessagesBody internally)
// =========================================================================

static void BM_JsonSerialize(benchmark::State& state) {
    ApiConfig cfg;
    cfg.api_key = "";  // dry-run: BuildMessagesBody is still called before the gate
    cfg.model_id = "claude-sonnet-4-20250514";
    cfg.max_tokens = 4096;
    SseClient client(cfg);

    // Build a realistic message array
    std::vector<TextMessage> messages;
    messages.push_back(TextMessage{
        .role = Role::User,
        .content = "Explain the theory of general relativity in simple terms, "
                   "covering spacetime curvature, gravitational lensing, and time dilation."
    });
    messages.push_back(TextMessage{
        .role = Role::Assistant,
        .content = "General relativity, proposed by Einstein in 1915, describes gravity "
                   "not as a force but as curvature of spacetime caused by mass and energy."
    });
    messages.push_back(TextMessage{
        .role = Role::User,
        .content = "Can you provide a mathematical formulation using the Einstein field equations?"
    });

    const std::string_view system_prompt =
        "You are a helpful physics tutor specializing in general relativity.";

    for (auto _ : state) {
        // PostMessagesStream in dry-run mode still exercises BuildMessagesBody
        StreamingCallbacks cbs;
        std::string final_text;
        cbs.on_final = [&](int, std::string, std::string, std::string) {};
        int rc = client.PostMessagesStream(system_prompt, messages, std::move(cbs));
        benchmark::DoNotOptimize(rc);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_JsonSerialize);

// =========================================================================
// BM_ToolDispatch: Measure registry.execute("Read", ...) on a small file
// =========================================================================

static void BM_ToolDispatch(benchmark::State& state) {
    using namespace cc::core;
    using namespace cc::tools::file_read;

    // Create a temporary file
    TempFile tmp;

    // Adapt FileReadTool to ITool interface (template register_tool doesn't
    // work across module boundaries due to concept constraint checking)
    struct ReadAdapter final : ITool {
        FileReadTool tool_;
        ToolDefinition def_ = FileReadTool::definition();

        const ToolDefinition& definition() const override { return def_; }
        std::expected<ToolResult, Error> execute(const ToolInput& input) override {
            auto result = tool_.execute(input);
            if (result) return std::move(*result);
            return std::unexpected(Error::make(
                ErrorCode::ToolExecutionFailed, result.error().format()));
        }
        bool check_permission(const ToolInput& input) const override {
            return tool_.check_permission(input);
        }
    };

    // Set up a registry with the FileReadTool adapter
    ToolRegistry registry;
    registry.register_tool(std::make_unique<ReadAdapter>());

    // Build the JSON input for file read
    const std::string input_json = std::string(R"({"file_path":")") +
                                   tmp.path.string() + R"(","limit":10})";
    ToolInput input = ToolInput::from_json(input_json);

    for (auto _ : state) {
        auto result = registry.execute("Read", input);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ToolDispatch);

// =========================================================================
// BM_BackoffDelay: Measure BackoffDelay computation
// =========================================================================

static void BM_BackoffDelay(benchmark::State& state) {
    RetryConfig cfg{
        .max_attempts = 5,
        .base_delay = std::chrono::milliseconds{500},
        .max_delay = std::chrono::milliseconds{30000},
        .retry_on_http = {429, 500, 502, 503, 504},
        .jitter_ratio = 0.2
    };

    int attempt = 0;
    for (auto _ : state) {
        auto delay = BackoffDelay(cfg, attempt % 5);
        benchmark::DoNotOptimize(delay);
        ++attempt;
    }
}
BENCHMARK(BM_BackoffDelay);

// =========================================================================
// BM_BackoffDelayNoJitter: Measure BackoffDelay without jitter (deterministic path)
// =========================================================================

static void BM_BackoffDelayNoJitter(benchmark::State& state) {
    RetryConfig cfg{
        .max_attempts = 5,
        .base_delay = std::chrono::milliseconds{500},
        .max_delay = std::chrono::milliseconds{30000},
        .retry_on_http = {429, 500, 502, 503, 504},
        .jitter_ratio = 0.0  // no jitter -> pure exponential + cap
    };

    int attempt = 0;
    for (auto _ : state) {
        auto delay = BackoffDelay(cfg, attempt % 5);
        benchmark::DoNotOptimize(delay);
        ++attempt;
    }
}
BENCHMARK(BM_BackoffDelayNoJitter);
