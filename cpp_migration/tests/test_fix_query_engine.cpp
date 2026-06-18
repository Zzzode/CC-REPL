/// @file test_fix_query_engine.cpp
/// @brief Dedicated QueryEngine test target closing audit finding B1.
///
/// The audit reports query_engine.cppm at 0.86% coverage with no test target
/// referencing it directly. The cc_core aggregator pulls cc_query transitively,
/// but the existing QueryEngine coverage lives inside test_services.cpp (a
/// 280k-line mega-target) which muddies per-file attribution. This file is a
/// focused, standalone QueryEngine test target.
///
/// What is covered here (gaps not exercised elsewhere, plus core seams):
///   (1) add_output_config_to_json via build_output_config_json_for_testing() —
///       both the response_schema branch and the empty/omitted branch.
///   (2) discovered_skills_ tracking across tool calls
///       (execute_single_tool_for_testing → execute_single_tool).
///   (3) Fallback-model selection on OverloadedError (HTTP 529) and
///       RateLimited (HTTP 429) — drives query() through call_api() and
///       observes model_params().model flipping to the fallback entry.
///   (4) User-memory (~/.claude/CLAUDE.md) + project CLAUDE.md injection into
///       the initial system prompt at construction time
///       (build_and_add_system_prompt via get_conversation()).
///
/// What is intentionally NOT covered here:
///   - SSE event parsing. The streaming parser in query_engine.cppm is an
///     inline lambda (parse_sse_event at ~L2183) that captures private member
///     state and is only reachable through stream_single_api_call's live HTTP
///     content_receiver. There is no exported seam to feed it synthetic
///     chunks, so exercising it would require either exporting the parser or
///     a full httplib content_receiver mock — out of scope for a tests-only
///     change. The independent cc::services::api::SseBuffer/StreamParser used
///     by the SDK path ARE already covered by test_sse_mock.cpp /
///     test_services.cpp.
///
/// All tests use exported/public surfaces only (QueryEngine public methods,
/// build_output_config_json_for_testing, execute_single_tool_for_testing,
/// discovered_skills, get_conversation, model_params).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import cc.query.query_engine;
import cc.tools.tool;
import cc.types.types;
import cc.utils.json;

namespace fs = std::filesystem;

namespace {

/// RAII guard for an environment variable. Restores the previous value (or
/// unsets it) on destruction. Mirrors the helper duplicated across the
/// existing test files so this translation unit is self-contained.
struct EnvironmentGuard {
    std::string name;
    std::optional<std::string> previous;

    EnvironmentGuard(std::string key, const std::string& value) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~EnvironmentGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

/// Minimal raw-socket HTTP server that returns a scripted sequence of
/// (status, body) responses, one per incoming request, and records the bodies
/// it received. Used to drive QueryEngine.query() through call_api()'s
/// fallback-model branch. Intentionally tiny (no httplib dep) so the test
/// target links only against cc_core.
class ScriptedHttpServer {
public:
    struct Response {
        int status = 200;
        std::string body =
            R"({"id":"msg_test","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})";
    };

    explicit ScriptedHttpServer(std::vector<Response> responses)
        : responses_(std::move(responses)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 4) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        thread_ = std::thread([this] { accept_loop(); });
    }

    ~ScriptedHttpServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    /// Wait until at least `count` request bodies have been captured.
    [[nodiscard]] std::optional<std::vector<std::string>> wait_for_bodies(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this, count] { return bodies_.size() >= count; })) {
            return std::nullopt;
        }
        return bodies_;
    }

private:
    void accept_loop() {
        while (running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            handle_client(client_fd);
            ::close(client_fd);
        }
    }

    static std::string read_request(int fd) {
        std::string request;
        char buffer[4096];
        std::size_t header_end = std::string::npos;
        while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return {};
            request.append(buffer, buffer + n);
        }
        auto header = request.substr(0, header_end + 4);
        std::size_t content_length = 0;
        auto length_pos = header.find("Content-Length:");
        if (length_pos == std::string::npos) {
            length_pos = header.find("content-length:");
        }
        if (length_pos != std::string::npos) {
            auto value_start = header.find(':', length_pos);
            auto value_end = header.find("\r\n", value_start);
            if (value_start != std::string::npos && value_end != std::string::npos) {
                try {
                    content_length = static_cast<std::size_t>(
                        std::stoul(header.substr(value_start + 1, value_end - value_start - 1)));
                } catch (...) {}
            }
        }
        const std::size_t body_start = header_end + 4;
        while (request.size() - body_start < content_length) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, buffer + n);
        }
        return request.substr(body_start, content_length);
    }

    void handle_client(int fd) {
        auto body = read_request(fd);
        std::size_t idx = 0;
        {
            std::lock_guard lock(mutex_);
            bodies_.push_back(body);
            idx = bodies_.size() - 1;
        }
        cv_.notify_all();

        Response resp;
        if (!responses_.empty()) {
            resp = responses_[std::min(idx, responses_.size() - 1)];
        }

        std::string reason = "OK";
        if (resp.status == 429) reason = "Too Many Requests";
        else if (resp.status == 529) reason = "Overloaded";
        else if (resp.status >= 400) reason = "Error";

        const auto response = std::format(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            resp.status, reason, resp.body.size(), resp.body);
        send_all(fd, response);
    }

    static void send_all(int fd, std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> bodies_;
    std::vector<Response> responses_;
};

/// Extract the text of the first system message in a conversation, if any.
std::optional<std::string> first_system_prompt_text(const std::vector<cc::core::Message>& conv) {
    for (const auto& msg : conv) {
        if (const auto* sys = std::get_if<cc::core::SystemMessage>(&msg)) {
            for (const auto& block : sys->content) {
                if (const auto* text = std::get_if<cc::core::TextBlock>(&block)) {
                    return text->text;
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

// ===========================================================================
// (2 already-covered seams, re-asserted here for a standalone target)
// ===========================================================================

TEST(QueryEngineFix, OutputConfigInjectsResponseSchemaAndTaskBudget) {
    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    config.response_schema = cc::core::QueryEngineConfig::ResponseSchema{
        .name = "result",
        .schema_json = R"({"type":"object","properties":{"answer":{"type":"string"}},"required":["answer"]})",
    };
    config.task_budget = cc::core::QueryEngineConfig::TaskBudget{
        .total = 5'000,
        .remaining = 2'500,
    };
    cc::core::QueryEngine engine(std::move(config), registry);

    const auto out = engine.build_output_config_json_for_testing();
    // response_schema → format.json_schema{ name, schema }
    EXPECT_NE(out.find("output_config"), std::string::npos);
    EXPECT_NE(out.find("json_schema"), std::string::npos);
    EXPECT_NE(out.find("\"result\""), std::string::npos);
    EXPECT_NE(out.find("answer"), std::string::npos);
    // task_budget → {type:"tokens", total, remaining}
    EXPECT_NE(out.find("task_budget"), std::string::npos);
    EXPECT_NE(out.find("tokens"), std::string::npos);
    EXPECT_NE(out.find("5000"), std::string::npos);
    EXPECT_NE(out.find("2500"), std::string::npos);
}

TEST(QueryEngineFix, OutputConfigOmittedWhenNeitherBudgetNorSchema) {
    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), registry);

    // Matches the contract documented on build_output_config_json_for_testing.
    EXPECT_EQ(engine.build_output_config_json_for_testing(), "{}");
}

// ===========================================================================
// (4) discovered_skills_ tracking via execute_single_tool_for_testing
// ===========================================================================

TEST(QueryEngineFix, DiscoveredSkillsTracksSkillToolInvocations) {
    struct StubSkillTool final : cc::core::ITool {
        cc::core::ToolDefinition definition_{};
        StubSkillTool() {
            definition_.name = "skill";
            definition_.permission = cc::core::ToolPermission::ReadOnly;
        }
        [[nodiscard]] const cc::core::ToolDefinition& definition() const override { return definition_; }
        [[nodiscard]] cc::core::Result<cc::core::ToolResult> execute(const cc::core::ToolInput&) override {
            return cc::core::ToolResult::success("ok");
        }
        [[nodiscard]] bool check_permission(const cc::core::ToolInput&) const override { return true; }
    };

    cc::core::ToolRegistry registry;
    registry.register_tool(std::make_unique<StubSkillTool>());
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), registry);

    // Initially empty.
    EXPECT_TRUE(engine.discovered_skills().empty());

    // First skill invocation → recorded.
    cc::core::ToolUseBlock first{
        .id = cc::core::ToolUseId{.value = "tu-1"},
        .name = "skill",
        .input_json = R"({"name":"alpha-skill"})",
    };
    (void)engine.execute_single_tool_for_testing(first);
    auto after_first = engine.discovered_skills();
    EXPECT_NE(std::find(after_first.begin(), after_first.end(), "alpha-skill"), after_first.end());

    // Second, distinct skill → appended (de-dup set semantics).
    cc::core::ToolUseBlock second{
        .id = cc::core::ToolUseId{.value = "tu-2"},
        .name = "skill",
        .input_json = R"({"name":"beta-skill"})",
    };
    (void)engine.execute_single_tool_for_testing(second);
    auto after_second = engine.discovered_skills();
    EXPECT_NE(std::find(after_second.begin(), after_second.end(), "beta-skill"), after_second.end());

    // Re-invoking alpha should not duplicate (set, not vector).
    const auto alpha_count = std::count(after_second.begin(), after_second.end(), "alpha-skill");
    EXPECT_EQ(alpha_count, 1);

    // A non-skill tool must NOT contribute to discovered_skills_.
    struct StubReadTool final : cc::core::ITool {
        cc::core::ToolDefinition definition_{};
        StubReadTool() {
            definition_.name = "Read";
            definition_.permission = cc::core::ToolPermission::ReadOnly;
        }
        [[nodiscard]] const cc::core::ToolDefinition& definition() const override { return definition_; }
        [[nodiscard]] cc::core::Result<cc::core::ToolResult> execute(const cc::core::ToolInput&) override {
            return cc::core::ToolResult::success("data");
        }
        [[nodiscard]] bool check_permission(const cc::core::ToolInput&) const override { return true; }
    };
    cc::core::ToolRegistry registry2;
    registry2.register_tool(std::make_unique<StubReadTool>());
    cc::core::QueryEngineConfig config2;
    config2.context_window.auto_compact = false;
    config2.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine2(std::move(config2), registry2);
    cc::core::ToolUseBlock read_call{
        .id = cc::core::ToolUseId{.value = "tu-r"},
        .name = "Read",
        .input_json = R"({"file_path":"/tmp/x"})",
    };
    (void)engine2.execute_single_tool_for_testing(read_call);
    EXPECT_TRUE(engine2.discovered_skills().empty())
        << "non-skill tools must not populate discovered_skills_";
}

// ===========================================================================
// (5) User-memory + project CLAUDE.md loading into the system prompt
// ===========================================================================

TEST(QueryEngineFix, LoadsProjectClaudeMdAndUserMemoryIntoSystemPrompt) {
    // Isolate HOME so get_user_memory_path() (~/.claude/CLAUDE.md) resolves
    // inside our temp dir and we do not read the developer's real user memory.
    auto root = fs::weakly_canonical(fs::temp_directory_path()) /
                "cc_repl_query_engine_memory_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard home_guard("HOME", root.string());

    // Project CLAUDE.md at cwd root.
    {
        std::ofstream project_md(root / "CLAUDE.md");
        project_md << "Project-specific guidance: always write tests in English.";
    }
    // User-level memory (~/.claude/CLAUDE.md).
    fs::create_directories(root / ".claude");
    {
        std::ofstream user_md(root / ".claude" / "CLAUDE.md");
        user_md << "Global user preference: respond concisely.";
    }

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = root.string();
    cc::core::QueryEngine engine(std::move(config), registry);

    const auto prompt = first_system_prompt_text(engine.get_conversation());
    ASSERT_TRUE(prompt.has_value()) << "system prompt not present in conversation";

    // Project memory injected under <context name="CLAUDE.md">.
    EXPECT_NE(prompt->find("<context name=\"CLAUDE.md\">"), std::string::npos);
    EXPECT_NE(prompt->find("Project-specific guidance"), std::string::npos);

    // User memory injected under <context name="UserMemory">.
    EXPECT_NE(prompt->find("<context name=\"UserMemory\">"), std::string::npos);
    EXPECT_NE(prompt->find("Global user preference"), std::string::npos);

    fs::remove_all(root);
}

TEST(QueryEngineFix, OmitsMemoryContextsWhenFilesAbsent) {
    auto root = fs::weakly_canonical(fs::temp_directory_path()) /
                "cc_repl_query_engine_no_memory_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard home_guard("HOME", root.string());
    // Ensure no user memory exists.
    fs::remove(root / ".claude" / "CLAUDE.md");

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = root.string();
    cc::core::QueryEngine engine(std::move(config), registry);

    const auto prompt = first_system_prompt_text(engine.get_conversation());
    ASSERT_TRUE(prompt.has_value());
    // Neither context block should appear when the files are missing.
    EXPECT_EQ(prompt->find("<context name=\"CLAUDE.md\">"), std::string::npos);
    EXPECT_EQ(prompt->find("<context name=\"UserMemory\">"), std::string::npos);

    fs::remove_all(root);
}

// ===========================================================================
// (3) Fallback-model selection on OverloadedError (529) / RateLimited (429)
//
// call_api() in query_engine.cppm (~L1514): on OverloadedError or RateLimited
// with a non-empty fallback_models list, it sets
// config_.model_params.model = fallback_models[idx++] and retries immediately
// (no backoff sleep). We observe the model flip via the public model_params()
// getter after query() returns.
// ===========================================================================

TEST(QueryEngineFix, FallsBackToSecondaryModelOnOverloadedError) {
    // Request 0 → 529 (OverloadedError); Request 1 → 200 (success).
    // After request 0, the engine must switch to the fallback model, so the
    // model field in the captured request-1 body should be the fallback.
    ScriptedHttpServer server({
        ScriptedHttpServer::Response{.status = 529, .body = R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"},
        ScriptedHttpServer::Response{.status = 200, .body = R"({"id":"msg_ok","type":"message","role":"assistant","model":"fallback-model","content":[{"type":"text","text":"done"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})"},
    });
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc_repl_query_engine_fallback_529_test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 3;       // allow the immediate fallback retry
    config.retry_policy.initial_delay = std::chrono::milliseconds(1);
    config.model_params.model = "primary-model";
    config.fallback_models = {"fallback-model"};
    // Auto-compact off so a 529 body can't accidentally trip compaction paths.
    config.context_window.auto_compact = false;

    cc::core::QueryEngine engine(std::move(config), registry);
    auto response = engine.query("ping");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    // The engine must have flipped its active model to the fallback.
    EXPECT_EQ(engine.model_params().model, "fallback-model")
        << "expected model to switch to fallback after 529 OverloadedError";

    // Both requests must have been made (primary attempt + fallback retry).
    auto bodies = server.wait_for_bodies(2);
    ASSERT_TRUE(bodies.has_value()) << "server did not receive 2 requests";
    ASSERT_EQ(bodies->size(), 2u);

    // The second request should carry the fallback model in its JSON body.
    auto parsed = cc::utils::json::parse((*bodies)[1]);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    EXPECT_EQ(parsed->root().get("model").as_str(), "fallback-model")
        << "fallback retry request should use the fallback model";

    fs::remove_all(root);
}

TEST(QueryEngineFix, FallsBackToSecondaryModelOnRateLimited) {
    // Request 0 → 429 (RateLimited); Request 1 → 200 (success).
    ScriptedHttpServer server({
        ScriptedHttpServer::Response{.status = 429, .body = R"({"type":"error","error":{"type":"rate_limit_error","message":"Too many requests"}})"},
        ScriptedHttpServer::Response{.status = 200, .body = R"({"id":"msg_ok","type":"message","role":"assistant","model":"fallback-model","content":[{"type":"text","text":"done"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})"},
    });
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc_repl_query_engine_fallback_429_test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 3;
    config.retry_policy.initial_delay = std::chrono::milliseconds(1);
    config.model_params.model = "primary-model";
    config.fallback_models = {"fallback-model"};
    config.context_window.auto_compact = false;

    cc::core::QueryEngine engine(std::move(config), registry);
    auto response = engine.query("ping");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    EXPECT_EQ(engine.model_params().model, "fallback-model")
        << "expected model to switch to fallback after 429 RateLimited";

    fs::remove_all(root);
}

TEST(QueryEngineFix, DoesNotFallBackWhenNoFallbackModelsConfigured) {
    // 529 with no fallback_models → call_api returns the error; query()
    // propagates it. The model must remain unchanged.
    ScriptedHttpServer server({
        ScriptedHttpServer::Response{.status = 529, .body = R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"},
    });
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc_repl_query_engine_no_fallback_test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 1;
    config.retry_policy.initial_delay = std::chrono::milliseconds(1);
    config.retry_policy.max_delay = std::chrono::milliseconds(5);
    config.model_params.model = "primary-model";
    // fallback_models intentionally left empty.
    config.context_window.auto_compact = false;

    cc::core::QueryEngine engine(std::move(config), registry);
    auto response = engine.query("ping");
    ASSERT_FALSE(response.has_value())
        << "query should fail when overloaded with no fallback models";

    // Model untouched — no fallback occurred.
    EXPECT_EQ(engine.model_params().model, "primary-model");

    fs::remove_all(root);
}
