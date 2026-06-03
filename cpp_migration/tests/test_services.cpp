/// @file test_services.cpp
/// @brief Service layer smoke tests aligned with current C++ module APIs.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

import cc.config.config;
import cc.services.api.client;
import cc.services.api.errors;
import cc.services.api.streaming;
import cc.services.memory.sessionMemory;
import cc.services.mcp.types;
import cc.services.rate_limit;
import cc.services.telemetry;
import cc.services.token_estimation;

namespace fs = std::filesystem;

TEST(ApiErrors, ClassifiesHttpStatusCodes) {
    using cc::services::api::errors::ApiErrorCategory;
    using cc::services::api::errors::ErrorClassifier;

    EXPECT_EQ(ErrorClassifier::classify_status(401), ApiErrorCategory::Authentication);
    EXPECT_EQ(ErrorClassifier::classify_status(429), ApiErrorCategory::RateLimited);
    EXPECT_EQ(ErrorClassifier::classify_status(529), ApiErrorCategory::Overloaded);
    EXPECT_EQ(ErrorClassifier::classify_status(500), ApiErrorCategory::ServerError);
    EXPECT_EQ(ErrorClassifier::classify_status(400), ApiErrorCategory::InvalidRequest);
}

TEST(ApiErrors, RetryDecisionUsesRetryableCategories) {
    using cc::services::api::errors::ApiErrorCategory;
    using cc::services::api::errors::ApiErrorDetails;
    using cc::services::api::errors::ErrorClassifier;

    ApiErrorDetails rate_limited{};
    rate_limited.category = ApiErrorCategory::RateLimited;
    rate_limited.http_status = 429;
    ApiErrorDetails bad_request{};
    bad_request.category = ApiErrorCategory::InvalidRequest;
    bad_request.http_status = 400;

    EXPECT_TRUE(ErrorClassifier::is_retryable(rate_limited));
    EXPECT_FALSE(ErrorClassifier::is_retryable(bad_request));
}

TEST(ApiClient, MessageFromTextCreatesSingleTextBlock) {
    auto message = cc::services::api::Message::from_text("user", "hello");

    ASSERT_EQ(message.role, "user");
    ASSERT_EQ(message.content.size(), 1u);
    EXPECT_EQ(message.content.front().type, cc::services::api::ContentBlockType::Text);
    EXPECT_EQ(message.content.front().text, "hello");
}

TEST(ApiClient, ResponseCombinesTextContentAndTokenUsage) {
    cc::services::api::CreateMessageResponse response;
    cc::services::api::ContentBlock first;
    first.type = cc::services::api::ContentBlockType::Text;
    first.text = "hello ";
    response.content.push_back(first);
    cc::services::api::ContentBlock second;
    second.type = cc::services::api::ContentBlockType::Text;
    second.text = "world";
    response.content.push_back(second);
    response.usage.input_tokens = 3;
    response.usage.output_tokens = 5;
    response.usage.cache_creation_tokens = 7;
    response.usage.cache_read_tokens = 11;

    EXPECT_EQ(response.get_text_content(), "hello world");
    EXPECT_EQ(response.usage.total(), 8);
    EXPECT_EQ(response.usage.total_with_cache(), 26);
}

TEST(ApiStreaming, SseBufferExtractsCompleteEvents) {
    cc::services::api::SseBuffer buffer;
    buffer.append("event: ping\ndata: {}\n\n");
    auto event = buffer.next_event();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->first, "ping");
    EXPECT_EQ(event->second, "{}");
}

TEST(ApiStreaming, StreamParserAccumulatesTextDeltas) {
    cc::services::api::StreamParser parser;
    parser.start();
    parser.feed("event: content_block_delta\n");
    parser.feed("data: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n");

    auto event = parser.next_event();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->has_value());
    EXPECT_EQ((*event)->type, cc::services::api::StreamEventType::ContentBlockDelta);
    EXPECT_EQ(parser.full_text(), "hi");
    EXPECT_EQ(parser.statistics().total_events, 1);
}

TEST(McpTypes, JsonRpcSerializationIncludesParams) {
    auto request = cc::services::mcp::make_request(
        int64_t{7},
        "tools/call",
        std::optional<std::string>{R"({"name":"echo","arguments":{"value":"hello"}})"});

    const auto serialized = cc::services::mcp::serialize_request(request);

    EXPECT_NE(serialized.find(R"("method":"tools/call")"), std::string::npos);
    EXPECT_NE(serialized.find(R"("params":{"name":"echo","arguments":{"value":"hello"}})"), std::string::npos);

    auto notification = cc::services::mcp::make_notification(
        "notifications/initialized",
        std::optional<std::string>{R"({"ready":true})"});

    const auto serialized_notification = cc::services::mcp::serialize_notification(notification);
    EXPECT_NE(serialized_notification.find(R"("params":{"ready":true})"), std::string::npos);
}

TEST(ConfigManager, PersistsMcpServerSettings) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_config_test_" + std::to_string(suffix));
    fs::create_directories(root);

    cc::core::ConfigManager manager(root / "global.json", root / "project.json");
    auto& settings = manager.settings_mut();
    settings.mcp_servers.push_back(cc::core::McpServerConfig{
        .name = "echo",
        .command = "node",
        .args = {"server.js", "--flag"},
        .env = {{"FOO", "bar"}},
    });

    ASSERT_TRUE(manager.save(cc::core::ConfigSource::ProjectConfig).has_value());

    cc::core::ConfigManager loaded(root / "global.json", root / "project.json");
    ASSERT_TRUE(loaded.load().has_value());
    ASSERT_EQ(loaded.settings().mcp_servers.size(), 1u);
    EXPECT_EQ(loaded.settings().mcp_servers.front().name, "echo");
    EXPECT_EQ(loaded.settings().mcp_servers.front().command, "node");
    ASSERT_EQ(loaded.settings().mcp_servers.front().args.size(), 2u);
    EXPECT_EQ(loaded.settings().mcp_servers.front().args[0], "server.js");
    EXPECT_EQ(loaded.settings().mcp_servers.front().args[1], "--flag");
    EXPECT_EQ(loaded.settings().mcp_servers.front().env.at("FOO"), "bar");

    fs::remove_all(root);
}

TEST(RateLimitManager, UpdatesStateFromHeadersAndWarnsNearLimits) {
    cc::services::RateLimitManager manager;
    manager.update_from_headers({
        {"x-ratelimit-remaining-requests", "3"},
        {"x-ratelimit-remaining-tokens", "9000"},
        {"retry-after", "2"},
    });

    const auto& state = manager.get_state();
    EXPECT_EQ(state.current_info.requests_remaining, 3);
    EXPECT_EQ(state.current_info.tokens_remaining, 9000);
    EXPECT_EQ(state.current_info.retry_after, std::chrono::milliseconds(2000));
    EXPECT_TRUE(manager.get_warning_message().has_value());
}

TEST(RateLimitManager, MockRateLimitControlsLimitedState) {
    cc::services::RateLimitManager manager;
    manager.mock_rate_limit({.simulate_429 = true});
    EXPECT_TRUE(manager.is_rate_limited());

    manager.clear_mock();
    EXPECT_FALSE(manager.is_rate_limited());
}

TEST(SessionMemoryService, StoresSearchesAndDeletesMemoryItems) {
    cc::services::memory::SessionMemoryService service;
    const auto now = std::chrono::system_clock::now();
    cc::services::memory::MemoryItem item{
        .id = "mem-1",
        .content = "remember project migration details",
        .type = "note",
        .created_at = now,
        .updated_at = now,
        .importance = 7,
    };

    ASSERT_TRUE(service.add_memory(item).has_value());

    auto loaded = service.get_memory("mem-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->content, item.content);

    auto matches = service.search_memories("migration");
    ASSERT_TRUE(matches.has_value());
    ASSERT_EQ(matches->size(), 1u);
    EXPECT_EQ(matches->front().id, "mem-1");

    ASSERT_TRUE(service.delete_memory("mem-1").has_value());
    EXPECT_FALSE(service.get_memory("mem-1").has_value());
}

TEST(TokenEstimator, EstimatesTextImagesToolsAndModelLimits) {
    using cc::services::ImageDetail;
    using cc::services::TokenEstimator;

    EXPECT_GT(TokenEstimator::estimate_text("Hello world"), 0u);
    EXPECT_EQ(TokenEstimator::estimate_text(""), 1u);
    EXPECT_EQ(TokenEstimator::estimate_image(512, 512, ImageDetail::low), 85u);
    EXPECT_GT(TokenEstimator::estimate_tool_use("Read", R"({"file_path":"main.cpp"})"), 50u);
    EXPECT_EQ(TokenEstimator::get_model_limit("claude-3-5-sonnet"), 200000u);
    EXPECT_TRUE(TokenEstimator::fits_in_context(1000, "claude-3-5-sonnet"));
}

TEST(TelemetryManager, TracksEventsAndFlushesConfiguredEndpoint) {
    cc::services::TelemetryConfig config;
    config.enabled = true;
    config.send_to_server = true;
    config.endpoint = "https://telemetry.example.test";
    config.max_buffer_size = 2;
    cc::services::TelemetryManager telemetry(config);
    telemetry.set_session("session-1");

    telemetry.track_command("help");
    EXPECT_EQ(telemetry.get_event_count(), 1u);
    EXPECT_EQ(telemetry.get_buffer_size(), 1u);

    telemetry.track_tool_use("Read", 12.5);
    EXPECT_EQ(telemetry.get_buffer_size(), 0u);
    EXPECT_EQ(telemetry.get_last_flush_endpoint(), "https://telemetry.example.test");
    EXPECT_EQ(telemetry.get_last_flush_count(), 2u);
}

TEST(TelemetryManager, SpanGuardRecordsCompletedSpanOnDestruction) {
    cc::services::TelemetryManager telemetry;
    {
        auto span = telemetry.start_span("compile", "trace-1");
        span.set_attribute("target", "test_services");
    }

    ASSERT_EQ(telemetry.get_spans().size(), 1u);
    EXPECT_EQ(telemetry.get_spans().front().name, "compile");
    EXPECT_EQ(telemetry.get_spans().front().trace_id, "trace-1");
    EXPECT_TRUE(telemetry.get_spans().front().end_time.has_value());
}
