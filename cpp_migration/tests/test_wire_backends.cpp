/// @file test_wire_backends.cpp
/// @brief Tests for the wire-protocol backend seam (cc.query.wire_protocol)
///        and its two implementations.
///
/// The seam exists so the agent loop can talk to either an Anthropic-shaped
/// /v1/messages endpoint or any OpenAI-compatible /v1/chat/completions
/// endpoint. These tests pin the CONTRACT each backend must honour:
///   * request URL/body shape,
///   * tool serialization (incl. the computer-use difference),
///   * response parsing into vendor-neutral ContentBlock types,
///   * streaming event decoding into StreamDelta,
///   * stop-reason vocabulary.
///
/// They intentionally assert on parsed/again-serialized JSON rather than raw
/// substrings where possible, so formatting changes do not break them.

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

import cc.query.wire_protocol;
import cc.query.wire_openai;
import cc.query.wire_anthropic;
import cc.utils.json;
import cc.types.types;
import cc.tools.tool;

namespace {

namespace wire = cc::query::wire;
using wire::StreamDelta;
using wire::WireApi;

// ─── helpers ───────────────────────────────────────────────────────────────

cc::core::ToolDefinition make_tool(std::string name, std::string description) {
    cc::core::ToolDefinition def;
    def.name = std::move(name);
    def.description = std::move(description);
    def.input_schema = cc::core::InputSchema{};
    def.permission = cc::core::ToolPermission::ReadOnly;
    return def;
}

cc::core::UserMessage make_user(std::string text) {
    cc::core::UserMessage msg{};
    msg.id.value = "u1";
    msg.timestamp = std::chrono::system_clock::now();
    msg.content.push_back(cc::core::TextBlock{std::move(text)});
    return msg;
}

cc::core::AssistantMessage make_assistant_with_tool_use(
    std::string tool_id, std::string tool_name, std::string input_json) {
    cc::core::AssistantMessage msg{};
    msg.id.value = "a1";
    msg.timestamp = std::chrono::system_clock::now();
    cc::core::ToolUseBlock tub;
    tub.id.value = std::move(tool_id);
    tub.name = std::move(tool_name);
    tub.input_json = std::move(input_json);
    msg.content.push_back(std::move(tub));
    return msg;
}

/// Parse a string into a JSON document, failing the test on malformed input.
cc::utils::json::JsonDoc parse_or_fail(const std::string& text) {
    auto doc = cc::utils::json::parse(text);
    EXPECT_TRUE(doc.has_value()) << "not valid JSON: " << text;
    return doc.has_value() ? std::move(*doc) : cc::utils::json::JsonDoc{};
}

}  // namespace

// ===========================================================================
// Wire-api resolution
// ===========================================================================

TEST(WireProtocol, ApiNameResolution) {
    EXPECT_EQ(*wire::wire_api_from_string("anthropic"), WireApi::Anthropic);
    EXPECT_EQ(*wire::wire_api_from_string("openai"), WireApi::OpenAi);
    EXPECT_EQ(*wire::wire_api_from_string("openai-compatible"), WireApi::OpenAi);
    EXPECT_EQ(*wire::wire_api_from_string("openai_compat"), WireApi::OpenAi);
    EXPECT_FALSE(wire::wire_api_from_string("nonsense").has_value());
    EXPECT_FALSE(wire::wire_api_from_string("").has_value());
}

TEST(WireProtocol, ProviderNameResolution) {
    // Anthropic-shaped proxies all speak /v1/messages.
    EXPECT_EQ(*wire::wire_api_from_provider("anthropic"), WireApi::Anthropic);
    EXPECT_EQ(*wire::wire_api_from_provider("bedrock"), WireApi::Anthropic);
    EXPECT_EQ(*wire::wire_api_from_provider("vertex"), WireApi::Anthropic);
    EXPECT_EQ(*wire::wire_api_from_provider("foundry"), WireApi::Anthropic);
    EXPECT_EQ(*wire::wire_api_from_provider("openai_compat"), WireApi::OpenAi);
    EXPECT_FALSE(wire::wire_api_from_provider("mystery").has_value());
}

TEST(WireProtocol, ApiNamesRoundTrip) {
    EXPECT_EQ(wire::wire_api_name(WireApi::Anthropic), "anthropic");
    EXPECT_EQ(wire::wire_api_name(WireApi::OpenAi), "openai");
}

// ===========================================================================
// OpenAI backend — request serialization
// ===========================================================================

TEST(OpenAiWireBackend, BuildsChatCompletionsRequest) {
    wire::OpenAiWireBackend backend("http://localhost:8080",
                                    {{"Authorization", "Bearer sk-test"}});

    wire::RequestInput input;
    input.model = "qwen2.5";
    input.max_tokens = 1234;
    input.stream = true;
    input.system_prompt = "You are a helpful agent.";
    input.messages.push_back(cc::core::Message{make_user("hello")});

    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value()) << prepared.error();
    EXPECT_EQ(prepared->url, "http://localhost:8080/v1/chat/completions");

    auto doc = parse_or_fail(prepared->body);
    const auto root = doc.root();
    EXPECT_EQ(std::string(root.get("model").as_str()), "qwen2.5");
    EXPECT_EQ(root.get("max_tokens").as_int(), 1234);
    EXPECT_TRUE(root.get("stream").as_bool());

    // System prompt is a message, not a top-level field.
    EXPECT_FALSE(root.get("system").valid());
    const auto messages = root.get("messages");
    ASSERT_TRUE(messages.is_arr());
    ASSERT_GE(messages.size(), 2u);
    EXPECT_EQ(std::string(messages.at(0).get("role").as_str()), "system");
    EXPECT_EQ(std::string(messages.at(1).get("role").as_str()), "user");

    // Credential is forwarded.
    bool saw_auth = false;
    for (const auto& [k, v] : prepared->headers) {
        if (k == "Authorization" && v == "Bearer sk-test") saw_auth = true;
    }
    EXPECT_TRUE(saw_auth);
}

TEST(OpenAiWireBackend, TrailingSlashBaseUrlIsNormalized) {
    wire::OpenAiWireBackend backend("http://localhost:8080/");
    wire::RequestInput input;
    input.model = "m";
    input.messages.push_back(cc::core::Message{make_user("hi")});
    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->url, "http://localhost:8080/v1/chat/completions");
}

TEST(OpenAiWireBackend, EmptyBaseUrlFailsLoudly) {
    wire::OpenAiWireBackend backend("");
    wire::RequestInput input;
    input.model = "m";
    auto prepared = backend.prepare(input);
    EXPECT_FALSE(prepared.has_value());
}

TEST(OpenAiWireBackend, ToolsUseFunctionShapeWithVerbatimSchema) {
    wire::OpenAiWireBackend backend("http://h");
    wire::RequestInput input;
    input.model = "m";
    input.messages.push_back(cc::core::Message{make_user("hi")});
    input.tools.push_back(make_tool("Read", "Read a file"));
    // A verbatim MCP schema must win over the simplified one.
    input.tool_schemas.emplace_back(
        "Read", R"({"type":"object","properties":{"nested":{"type":"object"}}})");

    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value());
    auto doc = parse_or_fail(prepared->body);
    const auto tools = doc.root().get("tools");
    ASSERT_TRUE(tools.is_arr());
    ASSERT_EQ(tools.size(), 1u);

    const auto fn = tools.at(0).get("function");
    ASSERT_TRUE(fn.is_obj());
    EXPECT_EQ(std::string(fn.get("name").as_str()), "Read");
    // OpenAI nests the schema under function.parameters (not input_schema).
    const auto params = fn.get("parameters");
    ASSERT_TRUE(params.is_obj());
    EXPECT_TRUE(params.get("properties").get("nested").is_obj())
        << "verbatim schema was not preserved: " << prepared->body;
}

TEST(OpenAiWireBackend, ComputerUseStaysAnOrdinaryFunctionTool) {
    // Unlike Anthropic, OpenAI has no native computer tool. The capability
    // must still be exposed — as a normal function tool.
    wire::OpenAiWireBackend backend("http://h");
    wire::RequestInput input;
    input.model = "m";
    input.messages.push_back(cc::core::Message{make_user("hi")});
    input.native_computer_tool = true;  // must be ignored by this backend
    input.tools.push_back(make_tool("computer_use", "Control the computer"));

    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value());
    auto doc = parse_or_fail(prepared->body);
    const auto tool = doc.root().get("tools").at(0);
    // OpenAI's own tool shape carries a top-level type of "function"; what
    // must NOT appear is Anthropic's native "computer_20241022" tool type.
    EXPECT_EQ(std::string(tool.get("type").as_str()), "function");
    EXPECT_NE(std::string(tool.get("type").as_str()), "computer_20241022");
    ASSERT_TRUE(tool.get("function").is_obj());
    EXPECT_EQ(std::string(tool.get("function").get("name").as_str()),
              "computer_use");
}

TEST(OpenAiWireBackend, ToolResultsBecomeRoleToolMessages) {
    wire::OpenAiWireBackend backend("http://h");
    wire::RequestInput input;
    input.model = "m";
    input.messages.push_back(cc::core::Message{
        make_assistant_with_tool_use("toolu_1", "Read", R"({"file_path":"/x"})")});

    cc::core::UserMessage tool_msg{};
    tool_msg.id.value = "u2";
    tool_msg.timestamp = std::chrono::system_clock::now();
    cc::core::ToolResultBlock trb;
    trb.tool_use_id.value = "toolu_1";
    // content is a variant<string, vector<ToolResultContentItem>>.
    trb.content = std::vector<cc::core::ToolResultContentItem>{
        cc::core::ToolResultContentItem{
            .type = "text",
            .text = "file body",
            .media_type = {},
            .data = {},
        },
    };
    tool_msg.content.push_back(std::move(trb));
    input.messages.push_back(cc::core::Message{std::move(tool_msg)});

    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value());
    auto doc = parse_or_fail(prepared->body);
    const auto messages = doc.root().get("messages");

    bool saw_assistant_tool_calls = false;
    bool saw_tool_role = false;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto m = messages.at(i);
        const std::string role(m.get("role").as_str());
        if (role == "assistant") {
            const auto tc = m.get("tool_calls");
            if (tc.is_arr() && tc.size() > 0) {
                saw_assistant_tool_calls = true;
                EXPECT_EQ(std::string(tc.at(0).get("id").as_str()), "toolu_1");
                EXPECT_EQ(std::string(tc.at(0).get("function").get("name").as_str()),
                          "Read");
            }
        } else if (role == "tool") {
            saw_tool_role = true;
            EXPECT_EQ(std::string(m.get("tool_call_id").as_str()), "toolu_1");
        }
    }
    EXPECT_TRUE(saw_assistant_tool_calls)
        << "assistant tool_use not mapped to tool_calls: " << prepared->body;
    EXPECT_TRUE(saw_tool_role)
        << "tool_result not mapped to a role:tool message: " << prepared->body;
}

// ===========================================================================
// OpenAI backend — response parsing
// ===========================================================================

TEST(OpenAiWireBackend, ParsesChatCompletionWithToolCalls) {
    wire::OpenAiWireBackend backend("http://h");
    const std::string body = R"({
      "id":"chatcmpl-1","model":"qwen2.5",
      "choices":[{"message":{"role":"assistant","content":"thinking...",
        "tool_calls":[{"id":"call_1","type":"function",
          "function":{"name":"Read","arguments":"{\"file_path\":\"/x\"}"}}]},
        "finish_reason":"tool_calls"}],
      "usage":{"prompt_tokens":11,"completion_tokens":22}
    })";

    auto parsed = backend.parse_response(body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->usage.input_tokens, 11u);
    EXPECT_EQ(parsed->usage.output_tokens, 22u);
    // finish_reason "tool_calls" must map onto the engine's "tool_use".
    ASSERT_TRUE(parsed->message.stop_reason.has_value());
    EXPECT_EQ(*parsed->message.stop_reason, "tool_use");
    EXPECT_TRUE(backend.stop_reason_is_tool_use(*parsed->message.stop_reason));

    bool saw_text = false, saw_tool = false;
    for (const auto& block : parsed->message.content) {
        if (std::get_if<cc::core::TextBlock>(&block)) saw_text = true;
        if (auto* tu = std::get_if<cc::core::ToolUseBlock>(&block)) {
            saw_tool = true;
            EXPECT_EQ(tu->id.value, "call_1");
            EXPECT_EQ(tu->name, "Read");
            EXPECT_NE(tu->input_json.find("/x"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_text);
    EXPECT_TRUE(saw_tool);
}

TEST(OpenAiWireBackend, MapsStopReasonVocabulary) {
    wire::OpenAiWireBackend backend("http://h");
    // "length" must become "max_tokens" so the engine's recovery branch fires.
    const std::string body = R"({
      "id":"c","model":"m",
      "choices":[{"message":{"role":"assistant","content":"x"},"finish_reason":"length"}],
      "usage":{"prompt_tokens":1,"completion_tokens":1}
    })";
    auto parsed = backend.parse_response(body);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->message.stop_reason.has_value());
    EXPECT_EQ(*parsed->message.stop_reason, "max_tokens");
    EXPECT_FALSE(backend.stop_reason_is_tool_use(*parsed->message.stop_reason));

    const std::string stop_body = R"({
      "id":"c","model":"m",
      "choices":[{"message":{"role":"assistant","content":"x"},"finish_reason":"stop"}],
      "usage":{"prompt_tokens":1,"completion_tokens":1}
    })";
    auto parsed2 = backend.parse_response(stop_body);
    ASSERT_TRUE(parsed2.has_value());
    EXPECT_EQ(*parsed2->message.stop_reason, "end_turn");
}

TEST(OpenAiWireBackend, MalformedResponseIsAnError) {
    wire::OpenAiWireBackend backend("http://h");
    EXPECT_FALSE(backend.parse_response("not json").has_value());
}

// ===========================================================================
// OpenAI backend — streaming
// ===========================================================================

TEST(OpenAiWireBackend, DecodesContentDelta) {
    wire::OpenAiWireBackend backend("http://h");
    const std::string frame =
        R"({"choices":[{"delta":{"content":"Hello"},"index":0}]})";
    auto deltas = backend.parse_stream_event("", frame);
    ASSERT_FALSE(deltas.empty());
    EXPECT_EQ(deltas.front().kind, StreamDelta::Kind::TextDelta);
    EXPECT_EQ(deltas.front().text, "Hello");
}

TEST(OpenAiWireBackend, DecodesToolCallStartThenArgumentFragments) {
    wire::OpenAiWireBackend backend("http://h");
    // First chunk carries id + name.
    auto start = backend.parse_stream_event(
        "", R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_9",
             "function":{"name":"Bash","arguments":""}}]}}]})");
    ASSERT_FALSE(start.empty());
    bool saw_start = false;
    for (const auto& d : start) {
        if (d.kind == StreamDelta::Kind::ToolUseStart) {
            saw_start = true;
            EXPECT_EQ(d.tool_id, "call_9");
            EXPECT_EQ(d.tool_name, "Bash");
        }
    }
    EXPECT_TRUE(saw_start);

    // Later chunks carry only argument fragments.
    auto frag = backend.parse_stream_event(
        "", R"({"choices":[{"delta":{"tool_calls":[{"index":0,
             "function":{"arguments":"{\"cmd\":"}}]}}]})");
    bool saw_frag = false;
    for (const auto& d : frag) {
        if (d.kind == StreamDelta::Kind::ToolUseInputDelta) {
            saw_frag = true;
            EXPECT_EQ(d.partial_json, R"({"cmd":)");
        }
    }
    EXPECT_TRUE(saw_frag);
}

TEST(OpenAiWireBackend, DoneSentinelEndsTheStream) {
    wire::OpenAiWireBackend backend("http://h");
    auto deltas = backend.parse_stream_event("", "[DONE]");
    ASSERT_FALSE(deltas.empty());
    bool saw_stop = false;
    for (const auto& d : deltas) {
        if (d.kind == StreamDelta::Kind::MessageStop) saw_stop = true;
    }
    EXPECT_TRUE(saw_stop);
}

TEST(OpenAiWireBackend, FinishReasonEmitsMessageStop) {
    wire::OpenAiWireBackend backend("http://h");
    auto deltas = backend.parse_stream_event(
        "", R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})");
    bool saw_stop = false;
    for (const auto& d : deltas) {
        if (d.kind == StreamDelta::Kind::MessageStop) {
            saw_stop = true;
            EXPECT_EQ(d.stop_reason, "tool_use");
        }
    }
    EXPECT_TRUE(saw_stop);
}

TEST(OpenAiWireBackend, GarbageFrameReportsError) {
    wire::OpenAiWireBackend backend("http://h");
    auto deltas = backend.parse_stream_event("", "{not json");
    ASSERT_FALSE(deltas.empty());
    EXPECT_EQ(deltas.front().kind, StreamDelta::Kind::Error);
}

// ===========================================================================
// Anthropic backend — this is the format the engine grew up on, so the tests
// pin the /v1/messages shape and the native computer_20241022 tool.
// ===========================================================================

TEST(AnthropicWireBackend, BuildsMessagesRequest) {
    wire::AnthropicWireBackend backend("https://api.anthropic.com");
    wire::RequestInput input;
    input.model = "claude-sonnet-4-20250514";
    input.max_tokens = 2048;
    input.stream = false;
    input.system_prompt = "You are an agent.";
    input.messages.push_back(cc::core::Message{make_user("hello")});

    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value()) << prepared.error();
    EXPECT_NE(prepared->url.find("/v1/messages"), std::string::npos)
        << prepared->url;

    auto doc = parse_or_fail(prepared->body);
    const auto root = doc.root();
    EXPECT_EQ(std::string(root.get("model").as_str()), "claude-sonnet-4-20250514");
    EXPECT_EQ(root.get("max_tokens").as_int(), 2048);
    // Anthropic carries the system prompt as a TOP-LEVEL field.
    EXPECT_TRUE(root.get("system").is_str());
    EXPECT_EQ(std::string(root.get("system").as_str()), "You are an agent.");
    EXPECT_TRUE(root.get("messages").is_arr());
}

TEST(AnthropicWireBackend, SendsAnthropicVersionHeader) {
    wire::AnthropicWireBackend backend("https://api.anthropic.com");
    wire::RequestInput input;
    input.model = "m";
    input.messages.push_back(cc::core::Message{make_user("hi")});

    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value());
    bool saw_version = false;
    for (const auto& [k, v] : prepared->headers) {
        if (k == "anthropic-version" && !v.empty()) saw_version = true;
    }
    EXPECT_TRUE(saw_version);
}

TEST(AnthropicWireBackend, NativeComputerToolUsesComputer20241022) {
    wire::AnthropicWireBackend backend("https://api.anthropic.com");
    wire::RequestInput input;
    input.model = "m";
    input.messages.push_back(cc::core::Message{make_user("hi")});
    input.native_computer_tool = true;
    input.computer_display_width = 1920;
    input.computer_display_height = 1080;
    input.computer_display_number = 1;
    input.tools.push_back(make_tool("computer_use", "Control the computer"));

    auto prepared = backend.prepare(input);
    ASSERT_TRUE(prepared.has_value()) << prepared.error();
    auto doc = parse_or_fail(prepared->body);
    const auto tools = doc.root().get("tools");
    ASSERT_TRUE(tools.is_arr());
    ASSERT_EQ(tools.size(), 1u);
    const auto tool = tools.at(0);
    EXPECT_EQ(std::string(tool.get("type").as_str()), "computer_20241022");
    // The wire name is "computer", not the registry's "computer_use".
    EXPECT_EQ(std::string(tool.get("name").as_str()), "computer");
    EXPECT_EQ(tool.get("display_width_px").as_int(), 1920);
    EXPECT_EQ(tool.get("display_height_px").as_int(), 1080);
    EXPECT_EQ(tool.get("display_number").as_int(), 1);
    // The native tool has no input_schema (params are fixed by the API).
    EXPECT_FALSE(tool.get("input_schema").valid());
}

TEST(AnthropicWireBackend, ParsesMessageResponse) {
    wire::AnthropicWireBackend backend("https://api.anthropic.com");
    const std::string body = R"({
      "id":"msg_1","model":"claude-sonnet-4-20250514","role":"assistant",
      "stop_reason":"tool_use",
      "content":[{"type":"text","text":"let me look"},
                 {"type":"tool_use","id":"toolu_1","name":"Read",
                  "input":{"file_path":"/x"}}],
      "usage":{"input_tokens":10,"output_tokens":5,
               "cache_read_input_tokens":3}
    })";

    auto parsed = backend.parse_response(body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->usage.input_tokens, 10u);
    EXPECT_EQ(parsed->usage.output_tokens, 5u);
    EXPECT_EQ(parsed->usage.cache_read_tokens, 3u);
    ASSERT_TRUE(parsed->message.stop_reason.has_value());
    EXPECT_EQ(*parsed->message.stop_reason, "tool_use");
    EXPECT_TRUE(backend.stop_reason_is_tool_use("tool_use"));
    EXPECT_FALSE(backend.stop_reason_is_tool_use("end_turn"));

    bool saw_text = false, saw_tool = false;
    for (const auto& block : parsed->message.content) {
        if (std::get_if<cc::core::TextBlock>(&block)) saw_text = true;
        if (auto* tu = std::get_if<cc::core::ToolUseBlock>(&block)) {
            saw_tool = true;
            EXPECT_EQ(tu->id.value, "toolu_1");
            EXPECT_EQ(tu->name, "Read");
        }
    }
    EXPECT_TRUE(saw_text);
    EXPECT_TRUE(saw_tool);
}

TEST(AnthropicWireBackend, DecodesContentBlockDelta) {
    wire::AnthropicWireBackend backend("https://api.anthropic.com");
    auto deltas = backend.parse_stream_event(
        "content_block_delta",
        R"({"index":0,"delta":{"type":"text_delta","text":"Hello"}})");
    ASSERT_FALSE(deltas.empty());
    bool saw_text = false;
    for (const auto& d : deltas) {
        if (d.kind == StreamDelta::Kind::TextDelta) {
            saw_text = true;
            EXPECT_EQ(d.text, "Hello");
        }
    }
    EXPECT_TRUE(saw_text);
}

TEST(AnthropicWireBackend, DecodesMessageDeltaStopReason) {
    wire::AnthropicWireBackend backend("https://api.anthropic.com");
    auto deltas = backend.parse_stream_event(
        "message_delta",
        R"({"delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})");
    bool saw_stop = false;
    for (const auto& d : deltas) {
        if (d.kind == StreamDelta::Kind::MessageStop) {
            saw_stop = true;
            EXPECT_EQ(d.stop_reason, "end_turn");
        }
    }
    EXPECT_TRUE(saw_stop);
}

TEST(AnthropicWireBackend, PingIsIgnoredNotAnError) {
    wire::AnthropicWireBackend backend("https://api.anthropic.com");
    auto deltas = backend.parse_stream_event("ping", R"({"type":"ping"})");
    for (const auto& d : deltas) {
        EXPECT_NE(d.kind, StreamDelta::Kind::Error);
    }
}
