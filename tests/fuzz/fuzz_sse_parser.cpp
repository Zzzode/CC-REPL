// Fuzz target: feeds random bytes into SseClient::FeedParser
// Build with -fsanitize=fuzzer when ENABLE_FUZZING is ON.

import cc.services.api.sse;

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace cc::services::api::sse;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Create a minimal SseClient with dry-run config (no network).
    ApiConfig cfg;
    cfg.api_key = "fuzz-key";  // non-empty so constructor doesn't short-circuit

    SseClient client(std::move(cfg));

    // Set up no-op callbacks to exercise the parser path.
    StreamingCallbacks cbs{};
    cbs.on_sse_event = [](SseEventType, std::string_view) {};
    cbs.on_text_delta = [](std::string_view) {};
    cbs.on_final = [](int, std::string, std::string, std::string) {};
    cbs.should_abort = []() { return false; };

    // Feed the random input into the SSE line parser.
    std::string accum;
    std::string stop_reason;
    std::string error_reason;

    std::string_view input(reinterpret_cast<const char*>(data), size);
    client.FeedParser(input, cbs, accum, stop_reason, error_reason);

    return 0;
}
