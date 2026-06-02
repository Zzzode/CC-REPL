/// @file test_bridge.cpp
/// @brief Bridge module smoke tests aligned with current C++ module APIs.

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

import cc.bridge.api;
import cc.bridge.config;
import cc.bridge.messages;
import cc.bridge.session_id_compat;
import cc.bridge.transport;

namespace {

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name) : name_(name) {
        if (auto* value = std::getenv(name)) previous_ = value;
        unsetenv(name);
    }
    ~ScopedEnvVar() {
        if (previous_) setenv(name_, previous_->c_str(), 1);
        else unsetenv(name_);
    }
    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    const char* name_;
    std::optional<std::string> previous_;
};

std::filesystem::path unique_temp_file(std::string_view suffix) {
    return std::filesystem::temp_directory_path() / (cc::bridge::generate_session_id() + std::string(suffix));
}

} // namespace

TEST(BridgeMessages, DetectsAndNormalizesMalformedBase64Images) {
    cc::bridge::ContentBlock malformed = cc::bridge::ImageBlock{
        .type = cc::bridge::ContentBlockType::Image,
        .source = {.media_type = "", .data = "iVBORw0KGgo="},
    };

    EXPECT_TRUE(cc::bridge::is_malformed_base64_image(malformed));
    EXPECT_EQ(cc::bridge::detect_image_format_from_base64("iVBORw0KGgo="), "image/png");

    auto normalized = cc::bridge::normalize_image_blocks({malformed});
    ASSERT_EQ(normalized.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::bridge::ImageBlock>(normalized.front()));
    EXPECT_EQ(std::get<cc::bridge::ImageBlock>(normalized.front()).source.media_type, "image/png");
}

TEST(BridgeMessages, ExtractsOnlyUserMessagesWithContent) {
    cc::bridge::SDKMessage ignored;
    ignored.type = "assistant";
    EXPECT_FALSE(cc::bridge::extract_inbound_message_fields(ignored).has_value());

    cc::bridge::SDKMessage user;
    user.type = "user";
    user.message.content = std::string("hello bridge");
    user.uuid = "uuid-1";

    auto extracted = cc::bridge::extract_inbound_message_fields(user);
    ASSERT_TRUE(extracted.has_value());
    ASSERT_TRUE(std::holds_alternative<std::string>(extracted->content));
    EXPECT_EQ(std::get<std::string>(extracted->content), "hello bridge");
    ASSERT_TRUE(extracted->uuid.has_value());
    EXPECT_EQ(*extracted->uuid, "uuid-1");
}

TEST(BridgeMessages, FlushGateBuffersUntilOpened) {
    std::vector<std::string> handled;
    cc::bridge::FlushGate gate([&handled](const cc::bridge::SDKMessage& msg) {
        handled.push_back(msg.type);
    });

    cc::bridge::SDKMessage message;
    message.type = "user";
    gate.enqueue(message);
    EXPECT_EQ(gate.buffer().size(), 1u);
    EXPECT_TRUE(handled.empty());

    gate.open();
    EXPECT_TRUE(gate.is_open());
    EXPECT_TRUE(gate.buffer().empty());
    ASSERT_EQ(handled.size(), 1u);
    EXPECT_EQ(handled.front(), "user");
}

TEST(BridgeConfig, LoadsDefaultsEnvironmentAndJsonFile) {
    ScopedEnvVar clear_port("CC_BRIDGE_PORT");
    ScopedEnvVar clear_host("CC_BRIDGE_HOST");
    ScopedEnvVar clear_token("CC_BRIDGE_TOKEN");

    cc::bridge::BridgeConfigLoader loader;
    auto defaults = loader.load();
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->host, "localhost");
    EXPECT_EQ(defaults->port, 7860u);
    EXPECT_EQ(defaults->transport, cc::bridge::TransportType::websocket);

    auto path = unique_temp_file("_bridge_config.json");
    {
        std::ofstream out(path);
        out << R"({"transport":"http-polling","host":"127.0.0.1","port":9000,"path":"/x","auth_token":"tok","debug_mode":true,"auto_connect":false})";
    }

    auto loaded = loader.load_from_file(path.string());
    std::filesystem::remove(path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->transport, cc::bridge::TransportType::http_polling);
    EXPECT_EQ(loaded->host, "127.0.0.1");
    EXPECT_EQ(loaded->port, 9000u);
    EXPECT_EQ(loaded->path, "/x");
    ASSERT_TRUE(loaded->auth_token.has_value());
    EXPECT_EQ(*loaded->auth_token, "tok");
    EXPECT_TRUE(loaded->debug_mode);
    EXPECT_FALSE(loaded->auto_connect);
}

TEST(BridgeConfig, RejectsMissingOrInvalidConfigFiles) {
    cc::bridge::BridgeConfigLoader loader;
    EXPECT_FALSE(loader.load_from_file("/definitely/not/present/bridge.json").has_value());

    auto path = unique_temp_file("_bridge_config_invalid.json");
    {
        std::ofstream out(path);
        out << R"({"transport":"stdio","port":70000})";
    }

    auto loaded = loader.load_from_file(path.string());
    std::filesystem::remove(path);
    EXPECT_FALSE(loaded.has_value());
}

TEST(BridgeTransport, WebSocketConnectSendsAndDisconnects) {
    cc::bridge::WebSocketTransport transport;
    std::vector<cc::bridge::TransportState> transitions;
    std::vector<std::string> received;
    transport.on_state_change([&transitions](cc::bridge::TransportState, cc::bridge::TransportState next) {
        transitions.push_back(next);
    });
    transport.on_message([&received](cc::bridge::BridgeMessage msg) {
        received.push_back(msg.id);
    });

    ASSERT_TRUE(transport.connect("ws://localhost:7860/bridge", std::nullopt).has_value());
    EXPECT_TRUE(transport.is_connected());

    cc::bridge::BridgeMessage message{
        .id = "msg-1",
        .type = "request",
        .method = "ping",
        .payload = R"({"ok":true})",
        .priority = cc::bridge::MessagePriority::high,
        .timestamp = std::chrono::system_clock::now(),
        .correlation_id = "corr-1",
    };
    ASSERT_TRUE(transport.send(message).has_value());
    ASSERT_EQ(transport.sent_frames().size(), 1u);
    EXPECT_NE(transport.sent_frames().front().find("msg-1"), std::string::npos);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received.front(), "msg-1");

    transport.disconnect();
    EXPECT_FALSE(transport.is_connected());
    ASSERT_FALSE(transitions.empty());
    EXPECT_EQ(transitions.back(), cc::bridge::TransportState::disconnected);
}

TEST(BridgeTransport, FlushGateQueuesUntilOpened) {
    cc::bridge::TransportFlushGate gate;
    std::vector<std::string> sent;
    gate.set_sender([&sent](cc::bridge::BridgeMessage msg) { sent.push_back(msg.id); });

    gate.close();
    cc::bridge::BridgeMessage queued;
    queued.id = "queued";
    queued.type = "event";
    gate.enqueue(queued);
    EXPECT_EQ(gate.pending_count(), 1u);
    EXPECT_TRUE(sent.empty());

    gate.open();
    EXPECT_EQ(gate.pending_count(), 0u);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent.front(), "queued");
}

TEST(BridgeTransport, CapacityWakeInvokesCallbackOnReleaseBelowCapacity) {
    cc::bridge::CapacityWake capacity;
    bool notified = false;
    capacity.set_capacity(2);
    capacity.on_available([&] { notified = true; });

    capacity.record_usage(2);
    EXPECT_FALSE(capacity.has_capacity());

    capacity.release(1);
    EXPECT_TRUE(capacity.has_capacity());
    EXPECT_TRUE(notified);
}

TEST(BridgeApi, ValidatesSafeIdsAndNormalizesSessionIds) {
    EXPECT_TRUE(cc::bridge::is_safe_bridge_id("env_abc-123"));
    EXPECT_FALSE(cc::bridge::is_safe_bridge_id("env/abc"));

    const std::string legacy = "A1B2C3D4-E5F6-7890-ABCD-EF1234567890";
    EXPECT_TRUE(cc::bridge::is_legacy_session_id("a1b2c3d4-e5f6-7890-abcd-ef1234567890"));
    EXPECT_EQ(cc::bridge::normalize_session_id(legacy), "ses_a1b2c3d4e5f67890abcdef1234567890");

    auto generated = cc::bridge::generate_session_id();
    EXPECT_EQ(generated.size(), 36u);
    EXPECT_EQ(generated.rfind("ses_", 0), 0u);
}

TEST(BridgeApi, RejectsUnsafeIdsBeforeNetworkCalls) {
    cc::bridge::BridgeApiClient client(cc::bridge::BridgeApiConfig{
        .base_url = "https://bridge.example.test",
        .access_token = "token",
        .runner_version = "test",
        .trusted_device_token = std::nullopt,
    });

    EXPECT_FALSE(client.poll_for_work("bad/id", "secret").has_value());
    EXPECT_FALSE(client.acknowledge_work("env", "bad/id", "session").has_value());
    EXPECT_FALSE(client.archive_session("bad/id").has_value());
}
