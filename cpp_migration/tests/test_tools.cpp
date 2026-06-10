/// @file test_tools.cpp
/// @brief Tool registry smoke tests aligned with current C++ modules.

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <httplib.h>
#ifndef _WIN32
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

import cc.tools.bash;
import cc.tools.computer_use;
import cc.tools.powershell;
import cc.tools.web_fetch;
import cc.tools.web_search;
import cc.tools.web_browser;
import cc.tools.mcp;
import cc.tools.agent;
import cc.tools.agent_runtime;
import cc.tools.todo_write;
import cc.tools.notebook;
import cc.tools.registry;
import cc.tools.runtime_registry;
import cc.tools.task;
import cc.tools.team;
import cc.tools.team_create;
import cc.tools.team_delete;
import cc.tools.tool;
import cc.utils.json;
import cc.utils.swarm_backends;
import cc.utils.team_helpers;
import cc.utils.teleport_utils;
import cc.hooks.tool_permissions;
import cc.services.api.client;
import cc.services.mcp.types;

namespace fs = std::filesystem;

namespace {

struct CurrentPathGuard {
    fs::path previous;

    explicit CurrentPathGuard(const fs::path& next) : previous(fs::current_path()) {
        fs::current_path(next);
    }

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

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

struct EnvironmentUnsetGuard {
    std::string name;
    std::optional<std::string> previous;

    explicit EnvironmentUnsetGuard(std::string key) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        unsetenv(name.c_str());
    }

    ~EnvironmentUnsetGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

struct LocalBridgeIngressRequest {
    std::string target_session_id;
    std::string authorization;
    std::string cookie;
    std::string organization_uuid;
    std::string body;
};

class LocalBridgeIngressServer {
public:
    LocalBridgeIngressServer() {
        server_.Post(R"(/v1/sessions/([^/]+)/events)", [&](const httplib::Request& req, httplib::Response& res) {
            LocalBridgeIngressRequest request;
            request.target_session_id = req.matches.size() > 1 ? req.matches[1].str() : std::string{};
            request.authorization = req.get_header_value("Authorization");
            request.cookie = req.get_header_value("Cookie");
            request.organization_uuid = req.get_header_value("X-Organization-Uuid");
            request.body = req.body;
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(std::move(request));
            }
            cv_.notify_all();
            res.status = 201;
            res.set_content(R"({"ok":true})", "application/json");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ > 0) {
            worker_ = std::jthread([this](std::stop_token) {
                server_.listen_after_bind();
            });
        }
    }

    ~LocalBridgeIngressServer() {
        server_.stop();
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] bool ready() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] std::optional<std::vector<LocalBridgeIngressRequest>> wait_for_requests(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this, count] { return requests_.size() >= count; })) {
            return std::nullopt;
        }
        return requests_;
    }

private:
    httplib::Server server_;
    int port_{0};
    std::jthread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<LocalBridgeIngressRequest> requests_;
};

#ifndef _WIN32
class LocalUnixLineServer {
public:
    explicit LocalUnixLineServer(fs::path path) : path_(std::move(path)) {
        fs::remove(path_);
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const auto path_text = path_.string();
        if (path_text.size() >= sizeof(addr.sun_path)) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        std::memcpy(addr.sun_path, path_text.data(), path_text.size());
        addr.sun_path[path_text.size()] = '\0';

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

        worker_ = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    }

    ~LocalUnixLineServer() {
        if (worker_.joinable()) worker_.request_stop();
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        fs::remove(path_);
    }

    [[nodiscard]] bool valid() const {
        return listen_fd_ >= 0;
    }

    [[nodiscard]] std::optional<std::string> wait_for_message(
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !messages_.empty(); })) {
            return std::nullopt;
        }
        return messages_.front();
    }

private:
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(listen_fd_, &fds);
            timeval timeout{0, 100'000};
            const int ready = ::select(listen_fd_ + 1, &fds, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;

            const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) continue;
            std::string payload;
            std::array<char, 1024> buffer{};
            while (true) {
                const auto n = ::recv(client_fd, buffer.data(), buffer.size(), 0);
                if (n <= 0) break;
                payload.append(buffer.data(), static_cast<std::size_t>(n));
            }
            ::close(client_fd);
            {
                std::lock_guard lock(mutex_);
                messages_.push_back(std::move(payload));
            }
            cv_.notify_all();
            break;
        }
    }

    fs::path path_;
    int listen_fd_{-1};
    std::jthread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> messages_;
};
#endif

struct RuntimeComputerUseProviderGuard {
    ~RuntimeComputerUseProviderGuard() {
        cc::tools::clear_runtime_computer_use_capture_provider_for_testing();
        cc::tools::clear_runtime_computer_use_input_provider_for_testing();
    }
};

std::optional<std::string> extract_background_task_id(std::string_view text) {
    constexpr std::string_view marker = "Task ID: ";
    const auto start = text.find(marker);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value_start = start + marker.size();
    const auto value_end = text.find_first_of("\r\n", value_start);
    return std::string(text.substr(value_start, value_end == std::string_view::npos
        ? std::string_view::npos
        : value_end - value_start));
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

cc::tools::AgentLivePermissionCheck check_agent_tool_permission_from_hook(
    cc::hooks::ToolPermissionHook& permission_hook,
    std::string_view tool_name,
    std::string_view input_json,
    std::string_view tool_use_id
) {
    permission_hook.set_current_tool_use_id(tool_use_id);
    auto response = permission_hook.can_use_response(tool_name, input_json);
    permission_hook.clear_current_tool_use_id();

    cc::tools::AgentLivePermissionCheck check;
    check.allowed = response.decision == cc::hooks::PermissionDecision::allow ||
                    response.decision == cc::hooks::PermissionDecision::allow_once;
    check.updated_input_json = std::move(response.updated_input_json);
    check.message = std::move(response.message);
    return check;
}

std::optional<std::string> extract_background_pid(std::string_view text) {
    constexpr std::string_view marker = "PID: ";
    const auto start = text.find(marker);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value_start = start + marker.size();
    const auto value_end = text.find_first_of("\r\n", value_start);
    return std::string(text.substr(value_start, value_end == std::string_view::npos
        ? std::string_view::npos
        : value_end - value_start));
}

std::string shell_quote_for_test(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

class LocalRemoteSessionApiServer {
public:
    std::atomic<int> session_list_requests{0};
    std::atomic<int> event_requests{0};
    std::atomic<int> event_posts{0};
    std::atomic<int> title_patches{0};
    std::atomic<int> archive_requests{0};
    std::atomic<int> environment_requests{0};
    std::atomic<int> environment_create_requests{0};
    std::atomic<int> file_upload_requests{0};
    std::string last_event_body;
    std::string last_title_body;
    std::string last_upload_body;
    std::string last_upload_filename;
    std::string last_upload_purpose;
    std::size_t last_upload_file_size{0};

    LocalRemoteSessionApiServer() {
        server_.Get("/v1/sessions", [&](const httplib::Request&, httplib::Response& res) {
            ++session_list_requests;
            res.set_content(R"({
              "data": [
                {
                  "id": "remote-session-http",
                  "title": "Remote HTTP session",
                  "session_status": "running",
                  "environment_id": "env-test",
                  "created_at": "2026-06-06T00:00:00Z",
                  "updated_at": "2026-06-06T00:00:01Z",
                  "session_context": {
                    "sources": [
                      {"type": "git_repository", "url": "https://github.com/acme/widgets.git", "revision": "main"}
                    ],
                    "cwd": "/tmp",
                    "outcomes": []
                  }
                }
              ],
              "has_more": false
            })", "application/json");
        });
        server_.Get(R"(/v1/sessions/([^/]+)/events)", [&](const httplib::Request&, httplib::Response& res) {
            ++event_requests;
            res.set_content(R"({
              "data": [
                {
                  "id": "event-42",
                  "type": "assistant",
                  "session_id": "remote-session-http",
                  "message": {
                    "content": [
                      {"type": "text", "text": "server says hello"}
                    ]
                  }
                }
              ],
              "has_more": false,
              "last_id": "event-42"
            })", "application/json");
        });
        server_.Post(R"(/v1/sessions/([^/]+)/events)", [&](const httplib::Request& req, httplib::Response& res) {
            ++event_posts;
            last_event_body = req.body;
            res.status = 201;
            res.set_content("{}", "application/json");
        });
        server_.Get(R"(/v1/sessions/([^/]+))", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({
              "id": "remote-session-http",
              "session_status": "archived",
              "environment_id": "env-test",
              "created_at": "2026-06-06T00:00:00Z",
              "updated_at": "2026-06-06T00:00:01Z",
              "session_context": {
                "cwd": "/tmp",
                "outcomes": [
                  {"git_info": {"repo": "repo", "branches": ["feature/http-poll"]}}
                ]
              }
            })", "application/json");
        });
        server_.Patch(R"(/v1/sessions/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
            ++title_patches;
            last_title_body = req.body;
            res.status = 200;
            res.set_content(R"({"id":"remote-session-http","title":"Updated"})", "application/json");
        });
        server_.Post(R"(/v1/sessions/([^/]+)/archive)", [&](const httplib::Request&, httplib::Response& res) {
            ++archive_requests;
            res.status = 200;
            res.set_content("{}", "application/json");
        });
        server_.Get("/v1/environment_providers", [&](const httplib::Request&, httplib::Response& res) {
            ++environment_requests;
            res.set_content(R"({
              "environments": [
                {
                  "kind": "bridge",
                  "environment_id": "env-bridge",
                  "name": "Bridge",
                  "created_at": "2026-06-06T00:00:00Z",
                  "state": "active"
                },
                {
                  "kind": "anthropic_cloud",
                  "environment_id": "env-cloud",
                  "name": "Cloud",
                  "created_at": "2026-06-06T00:00:01Z",
                  "state": "active"
                }
              ]
            })", "application/json");
        });
        server_.Post("/v1/environment_providers/cloud/create", [&](const httplib::Request&, httplib::Response& res) {
            ++environment_create_requests;
            res.status = 201;
            res.set_content(R"({
              "kind": "anthropic_cloud",
              "environment_id": "env-created",
              "name": "Created Cloud",
              "created_at": "2026-06-06T00:00:02Z",
              "state": "active"
            })", "application/json");
        });
        server_.Post("/v1/files", [&](const httplib::Request& req, httplib::Response& res) {
            ++file_upload_requests;
            last_upload_body = req.body;
            auto file = req.get_file_value("file");
            last_upload_filename = file.filename;
            last_upload_file_size = file.content.size();
            auto purpose = req.get_file_value("purpose");
            last_upload_purpose = purpose.content;
            res.status = 201;
            res.set_content(R"({"id":"file-seed-bundle"})", "application/json");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalRemoteSessionApiServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
};

class LocalSlowAnthropicStreamServer {
public:
    explicit LocalSlowAnthropicStreamServer(std::chrono::milliseconds delay)
        : delay_(delay) {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                ++request_count_;
                last_body_ = req.body;
            }
            cv_.notify_all();

            std::this_thread::sleep_for(delay_);
            res.set_content(
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_slow\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"late stream response\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":3}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalSlowAnthropicStreamServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] bool wait_for_request(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return request_count_ > 0; });
    }

    [[nodiscard]] bool wait_for_request_count(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return request_count_ >= expected; });
    }

    [[nodiscard]] std::size_t request_count() const {
        std::lock_guard lock(mutex_);
        return request_count_;
    }

    [[nodiscard]] std::optional<std::string> last_body() const {
        std::lock_guard lock(mutex_);
        return last_body_;
    }

private:
    std::chrono::milliseconds delay_;
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::optional<std::string> last_body_;
};

class LocalSleepToolUseAnthropicServer {
public:
    LocalSleepToolUseAnthropicServer() {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            std::size_t count = 0;
            {
                std::lock_guard lock(mutex_);
                count = ++request_count_;
                last_body_ = req.body;
            }
            cv_.notify_all();

            if (count == 1) {
                res.set_content(
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_sleep_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_sleep\",\"name\":\"sleep\",\"input\":{}}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"duration\\\":5,\\\"reason\\\":\\\"wait for cancellation\\\"}\"}}\n\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":12}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n",
                    "text/event-stream");
                return;
            }

            res.set_content(
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_after_sleep\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"sleep finished\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":2}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalSleepToolUseAnthropicServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] bool wait_for_request_count(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return request_count_ >= expected; });
    }

    [[nodiscard]] std::size_t request_count() const {
        std::lock_guard lock(mutex_);
        return request_count_;
    }

    [[nodiscard]] std::optional<std::string> last_body() const {
        std::lock_guard lock(mutex_);
        return last_body_;
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::optional<std::string> last_body_;
};

class LocalBashToolUseAnthropicServer {
public:
    LocalBashToolUseAnthropicServer() {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            std::size_t count = 0;
            {
                std::lock_guard lock(mutex_);
                count = ++request_count_;
                last_body_ = req.body;
            }
            cv_.notify_all();

            if (count == 1) {
                res.set_content(
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_bash_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_bash\",\"name\":\"Bash\",\"input\":{}}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"command\\\":\\\"trap 'printf cancelled; exit 0' TERM; printf started; sleep 5; printf done\\\",\\\"description\\\":\\\"wait for cancellation\\\"}\"}}\n\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":12}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n",
                    "text/event-stream");
                return;
            }

            res.set_content(
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_after_bash\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"bash finished\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":2}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalBashToolUseAnthropicServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] bool wait_for_request_count(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return request_count_ >= expected; });
    }

    [[nodiscard]] std::size_t request_count() const {
        std::lock_guard lock(mutex_);
        return request_count_;
    }

    [[nodiscard]] std::optional<std::string> last_body() const {
        std::lock_guard lock(mutex_);
        return last_body_;
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::optional<std::string> last_body_;
};

class LocalScriptedBashToolUseAnthropicServer {
public:
    explicit LocalScriptedBashToolUseAnthropicServer(
        std::string command,
        std::string final_text = "scripted bash complete"
    ) : command_(std::move(command)), final_text_(std::move(final_text)) {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            std::size_t count = 0;
            {
                std::lock_guard lock(mutex_);
                count = ++request_count_;
                request_bodies_.push_back(req.body);
            }
            cv_.notify_all();

            if (count == 1) {
                const auto input_json = std::format(
                    R"({{"command":"{}","description":"hook fixture"}})",
                    cc::tools::agent::json_escape_string(command_));
                const auto partial_json = cc::tools::agent::json_escape_string(input_json);
                res.set_content(
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_scripted_bash_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_bash_fixture\",\"name\":\"Bash\",\"input\":{}}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"" + partial_json + "\"}}\n\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":12}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n",
                    "text/event-stream");
                return;
            }

            const auto final_text_json = cc::tools::agent::json_escape_string(final_text_);
            res.set_content(
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_after_scripted_bash\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" + final_text_json + "\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":3}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalScriptedBashToolUseAnthropicServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] bool wait_for_request_count(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return request_count_ >= expected; });
    }

    [[nodiscard]] std::optional<std::string> request_body(std::size_t index) const {
        std::lock_guard lock(mutex_);
        if (index >= request_bodies_.size()) return std::nullopt;
        return request_bodies_[index];
    }

    [[nodiscard]] std::size_t request_count() const {
        std::lock_guard lock(mutex_);
        return request_count_;
    }

private:
    std::string command_;
    std::string final_text_;
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::vector<std::string> request_bodies_;
};

class LocalScriptedToolUseAnthropicServer {
public:
    LocalScriptedToolUseAnthropicServer(
        std::string tool_name,
        std::string tool_input_json,
        std::string tool_use_id,
        std::string final_text = "scripted tool complete"
    ) : tool_name_(std::move(tool_name)),
        tool_input_json_(std::move(tool_input_json)),
        tool_use_id_(std::move(tool_use_id)),
        final_text_(std::move(final_text)) {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            std::size_t count = 0;
            {
                std::lock_guard lock(mutex_);
                count = ++request_count_;
                request_bodies_.push_back(req.body);
            }
            cv_.notify_all();

            if (count == 1) {
                const auto tool_name_json = cc::tools::agent::json_escape_string(tool_name_);
                const auto tool_use_id_json = cc::tools::agent::json_escape_string(tool_use_id_);
                const auto partial_json = cc::tools::agent::json_escape_string(tool_input_json_);
                res.set_content(
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_scripted_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"" + tool_use_id_json + "\",\"name\":\"" + tool_name_json + "\",\"input\":{}}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"" + partial_json + "\"}}\n\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":12}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n",
                    "text/event-stream");
                return;
            }

            const auto final_text_json = cc::tools::agent::json_escape_string(final_text_);
            res.set_content(
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_after_scripted_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" + final_text_json + "\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":3}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalScriptedToolUseAnthropicServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] bool wait_for_request_count(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return request_count_ >= expected; });
    }

    [[nodiscard]] std::optional<std::string> request_body(std::size_t index) const {
        std::lock_guard lock(mutex_);
        if (index >= request_bodies_.size()) return std::nullopt;
        return request_bodies_[index];
    }

private:
    std::string tool_name_;
    std::string tool_input_json_;
    std::string tool_use_id_;
    std::string final_text_;
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::vector<std::string> request_bodies_;
};

class LocalPerTurnBashPwdAnthropicServer {
public:
    LocalPerTurnBashPwdAnthropicServer() {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                ++request_count_;
                request_bodies_.push_back(req.body);
            }
            cv_.notify_all();

            const bool has_tool_result = req.body.find(R"("tool_result")") != std::string::npos;
            if (!has_tool_result) {
                const auto partial_json = cc::tools::agent::json_escape_string(R"({"command":"pwd","description":"print working directory"})");
                res.set_content(
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_pwd_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_pwd\",\"name\":\"Bash\",\"input\":{}}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"" + partial_json + "\"}}\n\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":12}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n",
                    "text/event-stream");
                return;
            }

            res.set_content(
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_pwd_done\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"pwd complete\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":3}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalPerTurnBashPwdAnthropicServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] bool wait_for_request_count(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return request_count_ >= expected; });
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::vector<std::string> request_bodies_;
};

class LocalPerTurnBashCommandAnthropicServer {
public:
    explicit LocalPerTurnBashCommandAnthropicServer(
        std::string command,
        std::string final_text = "bash command complete"
    ) : command_(std::move(command)), final_text_(std::move(final_text)) {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                ++request_count_;
                request_bodies_.push_back(req.body);
            }
            cv_.notify_all();

            const bool has_tool_result = req.body.find(R"("tool_result")") != std::string::npos;
            if (!has_tool_result) {
                const auto input_json = std::format(
                    R"({{"command":"{}","description":"run per-agent bash command"}})",
                    cc::tools::agent::json_escape_string(command_));
                const auto partial_json = cc::tools::agent::json_escape_string(input_json);
                res.set_content(
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_bash_command_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_bash_command\",\"name\":\"Bash\",\"input\":{}}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"" + partial_json + "\"}}\n\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":12}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n",
                    "text/event-stream");
                return;
            }

            const auto final_text_json = cc::tools::agent::json_escape_string(final_text_);
            res.set_content(
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_bash_command_done\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" + final_text_json + "\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":3}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalPerTurnBashCommandAnthropicServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] bool wait_for_request_count(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    ) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return request_count_ >= expected; });
    }

private:
    std::string command_;
    std::string final_text_;
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::vector<std::string> request_bodies_;
};

class LocalSlowContentServer {
public:
    explicit LocalSlowContentServer(std::chrono::milliseconds delay)
        : delay_(delay) {
        server_.Get("/slow", [&](const httplib::Request&, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                ++request_count_;
            }
            cv_.notify_all();
            std::this_thread::sleep_for(delay_);
            res.set_content("slow web body", "text/plain");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalSlowContentServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string url() const {
        return std::format("http://127.0.0.1:{}/slow", port_);
    }

    [[nodiscard]] bool wait_for_request(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return request_count_ > 0; });
    }

private:
    std::chrono::milliseconds delay_;
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
};

bool wait_for_native_agent_status(
    std::string_view agent_id,
    cc::tools::agent_runtime::NativeAgentStatus expected,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1'000)
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto record = cc::tools::agent_runtime::native_agent_store().get(agent_id);
        if (record && record->status == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto record = cc::tools::agent_runtime::native_agent_store().get(agent_id);
    return record && record->status == expected;
}

} // namespace

TEST(ToolRegistry, ListsBuiltInTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    EXPECT_FALSE(names.empty());
}

TEST(ToolRegistry, ContainsExpectedTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    ASSERT_FALSE(names.empty());

    // Check that known tools are present
    bool has_bash = false;
    bool has_computer_use = false;
    bool has_lsp = false;
    bool has_skill = false;
    bool has_task_create = false;
    bool has_web_browser = false;
    for (const auto& name : names) {
        if (name == "Bash") has_bash = true;
        if (name == "computer_use") has_computer_use = true;
        if (name == "lsp") has_lsp = true;
        if (name == "skill") has_skill = true;
        if (name == "task_create") has_task_create = true;
        if (name == "web_browser") has_web_browser = true;
    }
    EXPECT_TRUE(has_bash);
    EXPECT_TRUE(has_computer_use);
    EXPECT_TRUE(has_lsp);
    EXPECT_TRUE(has_skill);
    EXPECT_TRUE(has_task_create);
    EXPECT_TRUE(has_web_browser);
}

TEST(ToolRegistry, CoreRegistryCanBeConstructed) {
    cc::tools::registry::ToolRegistry registry;
    EXPECT_EQ(registry.size(), 0u);  // Empty by default
}

TEST(ToolRegistry, RegistersRuntimeTools) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    EXPECT_GT(registry.size(), 0u);
    EXPECT_TRUE(registry.contains("Bash"));
    EXPECT_TRUE(registry.contains("computer_use"));
    EXPECT_TRUE(registry.contains("Read"));
    EXPECT_TRUE(registry.contains("web_browser"));
    EXPECT_TRUE(registry.contains("mcp"));
    EXPECT_TRUE(registry.contains("lsp"));
    EXPECT_TRUE(registry.contains("skill"));
    EXPECT_TRUE(registry.contains("task_create"));
}

TEST(Tools, WebBrowserToolUsesScreenshotBackend) {
    bool called = false;
    cc::tools::WebBrowserTool tool([&](
        const cc::tools::BrowserRequest& request,
        const cc::tools::PageState& state) -> std::expected<std::string, cc::tools::BrowserError> {
        called = true;
        EXPECT_EQ(request.action, cc::tools::BrowserAction::Screenshot);
        EXPECT_TRUE(state.url().empty());
        return std::string("iVBORw0KGgo=");
    });

    auto result = tool.execute(cc::tools::BrowserRequest{
        .action = cc::tools::BrowserAction::Screenshot,
        .url = "https://example.test",
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(called);
    EXPECT_EQ(result->content, "Captured browser screenshot.");
    ASSERT_TRUE(result->screenshot_base64.has_value());
    EXPECT_EQ(*result->screenshot_base64, "iVBORw0KGgo=");
    EXPECT_EQ(result->media_type, std::optional<std::string>{"image/png"});
}

TEST(Tools, WebBrowserToolRejectsInteractiveActionsWithoutAutomationBackend) {
    cc::tools::WebBrowserTool tool;

    auto click = tool.execute(cc::tools::BrowserRequest{
        .action = cc::tools::BrowserAction::Click,
        .selector = "#submit",
    });
    ASSERT_FALSE(click.has_value());
    EXPECT_EQ(click.error(), cc::tools::BrowserError::BrowserNotAvailable);

    auto fill = tool.execute(cc::tools::BrowserRequest{
        .action = cc::tools::BrowserAction::FillForm,
        .form_fields = {{
            .selector = "#email",
            .value = "ada@example.test",
        }},
    });
    ASSERT_FALSE(fill.has_value());
    EXPECT_EQ(fill.error(), cc::tools::BrowserError::BrowserNotAvailable);
}

TEST(Tools, WebBrowserToolUsesAutomationBackendForClickAndFillForm) {
    std::vector<cc::tools::BrowserRequest> requests;
    cc::tools::WebBrowserTool tool(
        {},
        [&](const cc::tools::BrowserRequest& request,
            const cc::tools::PageState&) -> std::expected<cc::tools::BrowserResult, cc::tools::BrowserError> {
            requests.push_back(request);
            return cc::tools::BrowserResult{
                .content = std::format("automated {}", cc::tools::action_name(request.action)),
            };
        });

    auto click = tool.execute(cc::tools::BrowserRequest{
        .action = cc::tools::BrowserAction::Click,
        .selector = "#submit",
    });
    ASSERT_TRUE(click.has_value());
    EXPECT_EQ(click->content, "automated click");

    auto fill = tool.execute(cc::tools::BrowserRequest{
        .action = cc::tools::BrowserAction::FillForm,
        .form_fields = {{
            .selector = "#email",
            .value = "ada@example.test",
        }},
    });
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->content, "automated fill_form");

    ASSERT_EQ(requests.size(), 2u);
    ASSERT_TRUE(requests[0].selector.has_value());
    EXPECT_EQ(*requests[0].selector, "#submit");
    ASSERT_EQ(requests[1].form_fields.size(), 1u);
    EXPECT_EQ(requests[1].form_fields.front().selector, "#email");
    EXPECT_EQ(requests[1].form_fields.front().value, "ada@example.test");
}

TEST(Tools, RuntimeWebBrowserUsesAutomationCommandBackend) {
    EnvironmentGuard automation_guard(
        "CC_REPL_BROWSER_AUTOMATION_CMD",
        "printf '%s' '{\"content\":\"clicked via command\"}' # {request}"
    );

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("web_browser", cc::core::ToolInput::from_json(R"({
      "action": "click",
      "selector": "#submit"
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("clicked via command"), std::string::npos);
}

TEST(Tools, RuntimeWebBrowserKeepsPageStateAcrossCalls) {
    EnvironmentUnsetGuard clear_automation_guard("CC_REPL_BROWSER_AUTOMATION_CMD");
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    {
        EnvironmentGuard automation_guard(
            "CC_REPL_BROWSER_AUTOMATION_CMD",
            "printf '%s' '{\"content\":\"navigated\",\"title\":\"Runtime Browser State\",\"url\":\"https://example.test\"}' # {request}"
        );
        auto navigate = registry.execute("web_browser", cc::core::ToolInput::from_json(R"({
          "action": "navigate",
          "url": "https://example.test"
        })"));
        ASSERT_TRUE(navigate.has_value());
        ASSERT_FALSE(navigate->is_error);
    }

    auto title = registry.execute("web_browser", cc::core::ToolInput::from_json(R"({
      "action": "get_title"
    })"));
    ASSERT_TRUE(title.has_value());
    EXPECT_FALSE(title->is_error);
    ASSERT_FALSE(title->content.empty());
    EXPECT_EQ(title->content.front().text, "Runtime Browser State");
}

TEST(Tools, PowerShellToolValidatesCommandAndDangerousCmdlets) {
    auto root = fs::temp_directory_path() / "cc_repl_powershell_validation_test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::tools::PowerShellTool tool(root);
    EXPECT_EQ(tool.check_permission("Get-ChildItem"), cc::tools::CmdletPermission::Allowed);
    EXPECT_EQ(
        tool.check_permission("Remove-Item -Recurse C:\\Temp"),
        cc::tools::CmdletPermission::NeedsApproval
    );

    auto empty = tool.validate(cc::tools::PowerShellConfig{
        .command = "",
        .working_directory = root,
    });
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error(), cc::tools::PowerShellError::CommandEmpty);

    auto missing_cwd = tool.validate(cc::tools::PowerShellConfig{
        .command = "Get-ChildItem",
        .working_directory = root / "missing",
    });
    ASSERT_FALSE(missing_cwd.has_value());
    EXPECT_EQ(missing_cwd.error(), cc::tools::PowerShellError::InvalidWorkingDirectory);

    auto dangerous = tool.validate(cc::tools::PowerShellConfig{
        .command = "Remove-Item -Recurse C:\\Temp",
        .working_directory = root,
    });
    ASSERT_FALSE(dangerous.has_value());
    EXPECT_EQ(dangerous.error(), cc::tools::PowerShellError::DangerousCmdlet);

    auto allowed = tool.validate(cc::tools::PowerShellConfig{
        .command = "Get-ChildItem",
        .working_directory = root,
    });
    EXPECT_TRUE(allowed.has_value());

    fs::remove_all(root);
}

TEST(Tools, PowerShellEncodingHandlerDecodesUtf16LeAndRejectsOddBytes) {
    const std::array<std::byte, 4> utf16_ok{
        std::byte{0x4f}, std::byte{0x00}, std::byte{0x4b}, std::byte{0x00}
    };
    auto decoded = cc::tools::EncodingHandler::utf16le_to_utf8(utf16_ok);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, "OK");

    const std::array<std::byte, 1> odd{std::byte{0x4f}};
    auto invalid = cc::tools::EncodingHandler::utf16le_to_utf8(odd);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error(), cc::tools::PowerShellError::EncodingError);

    const std::array<std::byte, 2> bom{std::byte{0xff}, std::byte{0xfe}};
    EXPECT_TRUE(cc::tools::EncodingHandler::has_utf16_bom(bom));
}

TEST(Tools, PowerShellEncodedCommandUsesUtf16LeForUtf8Input) {
    EXPECT_EQ(cc::tools::powershell_encoded_command("A"), "QQA=");
    EXPECT_EQ(cc::tools::powershell_encoded_command("\xe4\xbd\xa0"), "YE8=");
    EXPECT_EQ(cc::tools::powershell_single_quote("C:\\It'S\\Here"), "'C:\\It''S\\Here'");
}

TEST(Tools, RuntimePowerShellToolReportsUnavailableOnNonWindows) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("powershell", cc::core::ToolInput::from_json(R"({
      "command": "Get-ChildItem"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->content.empty());
#ifdef _WIN32
    GTEST_SKIP() << "PowerShell runtime execution depends on the Windows host shell";
#else
    EXPECT_TRUE(result->is_error);
    EXPECT_NE(result->content.front().text.find("only available on Windows"), std::string::npos);
#endif
}

TEST(Tools, RuntimePowerShellToolValidatesDangerousCommandBeforePlatformExecution) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("powershell", cc::core::ToolInput::from_json(R"({
      "command": "Remove-Item -Recurse C:\\Temp"
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Dangerous cmdlet detected"), std::string::npos);
}

TEST(Tools, RuntimePowerShellToolValidatesWorkingDirectoryBeforePlatformExecution) {
    auto root = fs::temp_directory_path() / "cc_repl_powershell_runtime_cwd_test";
    fs::remove_all(root);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("powershell", cc::core::ToolInput::from_json(std::format(R"({{
      "command": "Get-ChildItem",
      "cwd": "{}"
    }})", (root / "missing").string())));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Working directory does not exist"), std::string::npos);
}

TEST(Tools, RuntimePowerShellToolExecutesRealCommandWithWorkingDirectoryOnWindows) {
#ifndef _WIN32
    GTEST_SKIP() << "Real PowerShell execution is only available on Windows";
#else
    auto root = fs::temp_directory_path() / "cc_repl_powershell_runtime_windows_e2e";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("powershell", cc::core::ToolInput::from_json(std::format(R"({{
      "command": "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; Write-Output (Get-Location).Path",
      "cwd": "{}"
    }})", cc::tools::agent::json_escape_string(root.string()))));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error) << (result->content.empty() ? "" : result->content.front().text);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find(root.string()), std::string::npos);

    fs::remove_all(root);
#endif
}

TEST(Tools, ComputerUseManagerUsesCaptureProviderForScreenshot) {
    using namespace cc::core::computer_use;
    using Rect = cc::core::computer_use::Rect;

    bool saw_region = false;
    ComputerUseManager manager(ScreenCapture([&](std::optional<Rect> region)
        -> std::expected<ImageData, std::string> {
        saw_region = region.has_value();
        if (region) {
            EXPECT_EQ(region->x, 1);
            EXPECT_EQ(region->y, 2);
            EXPECT_EQ(region->width, 3u);
            EXPECT_EQ(region->height, 4u);
        }
        return ImageData{
            .pixels = {1, 2, 3, 4},
            .width = 3,
            .height = 4,
            .format = "rgba",
        };
    }));

    auto result = manager.execute_action(ComputerAction{
        .type = ActionType::Screenshot,
        .position = std::nullopt,
        .drag_end = std::nullopt,
        .text = std::nullopt,
        .region = Rect{.x = 1, .y = 2, .width = 3, .height = 4},
        .keys = {},
    });

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_TRUE(saw_region);
    ASSERT_TRUE(result.screenshot.has_value());
    EXPECT_EQ(result.screenshot->width, 3u);
    EXPECT_EQ(result.screenshot->height, 4u);
    EXPECT_EQ(result.screenshot->format, "rgba");
}

TEST(Tools, RuntimeComputerUseScreenshotReturnsImageContentFromCaptureProvider) {
    using namespace cc::core::computer_use;
    using Rect = cc::core::computer_use::Rect;

    RuntimeComputerUseProviderGuard guard;
    bool saw_region = false;
    cc::tools::set_runtime_computer_use_capture_provider_for_testing(
        [&](std::optional<Rect> region) -> std::expected<ImageData, std::string> {
            saw_region = region.has_value();
            if (!region) return std::unexpected("missing region");
            EXPECT_EQ(region->x, 5);
            EXPECT_EQ(region->y, 6);
            EXPECT_EQ(region->width, 2u);
            EXPECT_EQ(region->height, 2u);
            return ImageData{
                .pixels = {1, 2, 3, 4},
                .width = 2,
                .height = 2,
                .format = "rgba",
            };
        });

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "screenshot",
      "x": 5,
      "y": 6,
      "width": 2,
      "height": 2
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    EXPECT_TRUE(saw_region);
    ASSERT_EQ(result->content.size(), 2u);
    EXPECT_NE(result->content[0].text.find("Captured screenshot 2x2."), std::string::npos);
    EXPECT_EQ(result->content[1].format, std::optional<std::string>{"image"});
    EXPECT_EQ(result->content[1].media_type, std::optional<std::string>{"image/rgba"});
    EXPECT_EQ(result->content[1].data, std::optional<std::string>{"AQIDBA=="});
}

TEST(Tools, RuntimeComputerUseUsesCommandBackendForScreenshotAndInputActions) {
    RuntimeComputerUseProviderGuard guard;
    EnvironmentGuard disable_native_input("CC_REPL_DISABLE_NATIVE_COMPUTER_INPUT", "1");

    auto root = fs::temp_directory_path() / "cc_repl_computer_use_command_backend_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto script_path = root / "computer-use-host.js";
    const auto log_path = root / "requests.jsonl";
    {
        std::ofstream script(script_path);
        script << R"JS(
const fs = require('fs');
const request = JSON.parse(process.argv[2] || '{}');
fs.appendFileSync(process.env.CC_REPL_COMPUTER_USE_LOG, JSON.stringify(request) + '\n');
if (request.action === 'screenshot') {
  console.log(JSON.stringify({
    success: true,
    screenshot_base64: 'AQIDBA==',
    width: 2,
    height: 2,
    format: 'rgba',
  }));
} else if (request.action === 'right_click') {
  console.log(JSON.stringify({success: false, error: 'blocked by host'}));
} else {
  console.log(JSON.stringify({success: true, content: 'input accepted'}));
}
)JS";
    }
    EnvironmentGuard log_guard("CC_REPL_COMPUTER_USE_LOG", log_path.string());
    EnvironmentGuard command_guard(
        "CC_REPL_COMPUTER_USE_CMD",
        "node " + shell_quote_for_test(script_path.string()) + " {request}");

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto screenshot = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "screenshot",
      "x": 5,
      "y": 6,
      "width": 2,
      "height": 2
    })"));
    ASSERT_TRUE(screenshot.has_value());
    EXPECT_FALSE(screenshot->is_error);
    ASSERT_EQ(screenshot->content.size(), 2u);
    EXPECT_EQ(screenshot->content[1].media_type, std::optional<std::string>{"image/rgba"});
    EXPECT_EQ(screenshot->content[1].data, std::optional<std::string>{"AQIDBA=="});

    auto click = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "click",
      "x": 11,
      "y": 12
    })"));
    ASSERT_TRUE(click.has_value());
    EXPECT_FALSE(click->is_error);

    auto blocked = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "right_click",
      "x": 13,
      "y": 14
    })"));
    ASSERT_TRUE(blocked.has_value());
    EXPECT_TRUE(blocked->is_error);
    ASSERT_FALSE(blocked->content.empty());
    EXPECT_NE(blocked->content.front().text.find("blocked by host"), std::string::npos);

    std::ifstream log(log_path);
    ASSERT_TRUE(log);
    std::stringstream buffer;
    buffer << log.rdbuf();
    const auto log_text = buffer.str();
    EXPECT_NE(log_text.find(R"("action":"screenshot")"), std::string::npos);
    EXPECT_NE(log_text.find(R"("region":{"x":5,"y":6,"width":2,"height":2})"), std::string::npos);
    EXPECT_NE(log_text.find(R"("action":"click")"), std::string::npos);
    EXPECT_NE(log_text.find(R"("x":11)"), std::string::npos);
    EXPECT_NE(log_text.find(R"("action":"right_click")"), std::string::npos);

    fs::remove_all(root);
}

TEST(Tools, ComputerUseManagerFailsInputActionsWithoutInputProvider) {
    using namespace cc::core::computer_use;

    ComputerUseManager manager;
    auto result = manager.execute_action(ComputerAction{
        .type = ActionType::MouseClick,
        .position = Point{.x = 10, .y = 20},
        .drag_end = std::nullopt,
        .text = std::nullopt,
        .region = std::nullopt,
        .keys = {},
    });

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "Computer input control not available");
}

TEST(Tools, NativeComputerUseInputProviderHonorsDisableEnv) {
    EnvironmentGuard guard("CC_REPL_DISABLE_NATIVE_COMPUTER_INPUT", "1");
    auto provider = cc::core::computer_use::make_native_input_provider();
    EXPECT_FALSE(static_cast<bool>(provider));
}

TEST(Tools, NativeComputerUseInputProviderIsAvailableOnApple) {
    if (const char* disabled = std::getenv("CC_REPL_DISABLE_NATIVE_COMPUTER_INPUT");
        disabled && std::string_view(disabled) == "1") {
        GTEST_SKIP() << "native computer input is disabled by environment";
    }
    auto provider = cc::core::computer_use::make_native_input_provider();
#ifdef __APPLE__
    EXPECT_TRUE(static_cast<bool>(provider));
#else
    EXPECT_FALSE(static_cast<bool>(provider));
#endif
}

TEST(Tools, RuntimeComputerUseDispatchesInputActionsToProvider) {
    using namespace cc::core::computer_use;

    RuntimeComputerUseProviderGuard guard;
    std::vector<ComputerAction> actions;
    cc::tools::set_runtime_computer_use_input_provider_for_testing(
        [&](const ComputerAction& action) -> std::expected<void, std::string> {
            actions.push_back(action);
            return {};
        });

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto click = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "click",
      "x": 11,
      "y": 12
    })"));
    ASSERT_TRUE(click.has_value());
    EXPECT_FALSE(click->is_error);

    auto typed = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "type",
      "text": "hello"
    })"));
    ASSERT_TRUE(typed.has_value());
    EXPECT_FALSE(typed->is_error);

    auto hotkey = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "hotkey",
      "keys": ["cmd", "k"]
    })"));
    ASSERT_TRUE(hotkey.has_value());
    EXPECT_FALSE(hotkey->is_error);

    auto scroll = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "scroll",
      "x": 0,
      "y": -3
    })"));
    ASSERT_TRUE(scroll.has_value());
    EXPECT_FALSE(scroll->is_error);

    ASSERT_EQ(actions.size(), 4u);
    EXPECT_EQ(actions[0].type, ActionType::MouseClick);
    ASSERT_TRUE(actions[0].position.has_value());
    EXPECT_EQ(actions[0].position->x, 11);
    EXPECT_EQ(actions[0].position->y, 12);
    EXPECT_EQ(actions[1].type, ActionType::KeyType);
    EXPECT_EQ(actions[1].text, std::optional<std::string>{"hello"});
    EXPECT_EQ(actions[2].type, ActionType::KeyHotkey);
    ASSERT_EQ(actions[2].keys.size(), 2u);
    EXPECT_EQ(actions[2].keys[0], "cmd");
    EXPECT_EQ(actions[2].keys[1], "k");
    EXPECT_EQ(actions[3].type, ActionType::Scroll);
    ASSERT_TRUE(actions[3].position.has_value());
    EXPECT_EQ(actions[3].position->x, 0);
    EXPECT_EQ(actions[3].position->y, -3);
}

TEST(Tools, RuntimeComputerUseRejectsInputActionsWithoutProvider) {
    RuntimeComputerUseProviderGuard guard;
    EnvironmentGuard disable_native_input("CC_REPL_DISABLE_NATIVE_COMPUTER_INPUT", "1");

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("computer_use", cc::core::ToolInput::from_json(R"({
      "action": "click",
      "x": 1,
      "y": 2
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Computer input control not available"), std::string::npos);
}

TEST(Tools, LspToolUsesConfiguredLanguageServer) {
    auto root = fs::temp_directory_path() / "cc_repl_runtime_lsp_tool_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "plugins" / "lsp-runtime-fixture");
    const auto plugin_root = root / ".claude" / "plugins" / "lsp-runtime-fixture";
    const auto server_path = plugin_root / "server.js";
    const auto source_path = root / "sample.foo";
    {
        std::ofstream source(source_path);
        source << "function fixtureSymbol() { return fixtureCompletion; }\n";
    }
    {
        std::ofstream server(server_path);
        server << R"JS(
let buffer = Buffer.alloc(0);

function send(message) {
  const body = JSON.stringify(message);
  process.stdout.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

function position(line, character) {
  return { line, character };
}

function range(line, character) {
  return { start: position(line, character), end: position(line, character + 4) };
}

function handle(message) {
  const uri = message.params?.textDocument?.uri || 'file:///fixture';
  if (message.method === 'initialize') {
    send({ jsonrpc: '2.0', id: message.id, result: { capabilities: { textDocumentSync: 1 } } });
    return;
  }
  if (message.method === 'textDocument/didOpen') {
    send({
      jsonrpc: '2.0',
      method: 'textDocument/publishDiagnostics',
      params: {
        uri,
        diagnostics: [{
          range: range(1, 2),
          severity: 1,
          source: 'fixture',
          message: 'fixture diagnostic',
          code: 'F001'
        }]
      }
    });
    return;
  }
  if (message.method === 'textDocument/definition') {
    send({ jsonrpc: '2.0', id: message.id, result: [{ uri, range: range(7, 3) }] });
    return;
  }
  if (message.method === 'textDocument/references') {
    send({ jsonrpc: '2.0', id: message.id, result: [{ uri, range: range(8, 4) }] });
    return;
  }
  if (message.method === 'textDocument/completion') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { isIncomplete: false, items: [{ label: 'fixtureCompletion', kind: 3, detail: 'callable' }] }
    });
    return;
  }
  if (message.method === 'textDocument/hover') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { contents: { kind: 'markdown', value: 'fixture hover' }, range: range(2, 1) }
    });
    return;
  }
  if (message.method === 'textDocument/documentSymbol') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: [{
        name: 'fixtureSymbol',
        kind: 12,
        range: range(0, 9),
        selectionRange: range(0, 9)
      }]
    });
    return;
  }
  if (message.method === 'exit') process.exit(0);
}

process.stdin.on('data', chunk => {
  buffer = Buffer.concat([buffer, chunk]);
  while (true) {
    const headerEnd = buffer.indexOf('\r\n\r\n');
    if (headerEnd === -1) return;
    const header = buffer.subarray(0, headerEnd).toString();
    const match = /Content-Length:\s*(\d+)/i.exec(header);
    if (!match) process.exit(2);
    const length = Number(match[1]);
    const bodyStart = headerEnd + 4;
    if (buffer.length < bodyStart + length) return;
    const body = buffer.subarray(bodyStart, bodyStart + length).toString();
    buffer = buffer.subarray(bodyStart + length);
    handle(JSON.parse(body));
  }
});
process.stdin.resume();
)JS";
    }
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << R"JSON({
  "name": "lsp-runtime-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "lspServers": {
    "fixture": {
      "command": "node",
      "args": ["${CLAUDE_PLUGIN_ROOT}/server.js"],
      "extensionToLanguage": {".foo": "foo"}
    }
  }
})JSON";
    }

    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard plugin_cache_guard("CLAUDE_CODE_PLUGIN_CACHE_DIR", (root / ".claude" / "plugins").string());
    CurrentPathGuard cwd(root);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto execute_lsp = [&](std::string_view action) {
        cc::utils::json::JsonMutDoc doc;
        auto input = doc.object();
        input.add("action", doc.string(action));
        input.add("file_path", doc.string(source_path.string()));
        input.add("line", doc.number(static_cast<int64_t>(0)));
        input.add("character", doc.number(static_cast<int64_t>(9)));
        doc.set_root(input);
        return registry.execute("lsp", cc::core::ToolInput::from_json(doc.to_string()));
    };

    auto definition = execute_lsp("definition");
    ASSERT_TRUE(definition.has_value());
    EXPECT_FALSE(definition->is_error) << definition->content.front().text;
    EXPECT_NE(definition->content.front().text.find(":7:3"), std::string::npos);

    auto completion = execute_lsp("completion");
    ASSERT_TRUE(completion.has_value());
    EXPECT_FALSE(completion->is_error) << completion->content.front().text;
    EXPECT_NE(completion->content.front().text.find("fixtureCompletion callable"), std::string::npos);

    auto hover = execute_lsp("hover");
    ASSERT_TRUE(hover.has_value());
    EXPECT_FALSE(hover->is_error) << hover->content.front().text;
    EXPECT_NE(hover->content.front().text.find("fixture hover"), std::string::npos);

    auto symbols = execute_lsp("symbols");
    ASSERT_TRUE(symbols.has_value());
    EXPECT_FALSE(symbols->is_error) << symbols->content.front().text;
    EXPECT_NE(symbols->content.front().text.find("function fixtureSymbol"), std::string::npos);

    auto diagnostics = execute_lsp("diagnostics");
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_FALSE(diagnostics->is_error) << diagnostics->content.front().text;
    EXPECT_NE(diagnostics->content.front().text.find("fixture:1:2 fixture diagnostic"), std::string::npos);

    fs::remove_all(root);
}

TEST(ToolInput, HasFieldParsesTopLevelJsonKeys) {
    auto input = cc::core::ToolInput::from_json(R"({
      "cwd": null,
      "description": "command mentions timeout and nested_field",
      "nested": {"command": "pwd"}
    })");

    EXPECT_TRUE(input.has_field("cwd"));
    EXPECT_TRUE(input.has_field("description"));
    EXPECT_TRUE(input.has_field("nested"));
    EXPECT_FALSE(input.has_field("timeout"));
    EXPECT_FALSE(input.has_field("command"));
    EXPECT_FALSE(input.has_field("nested_field"));
    EXPECT_FALSE(input.has_field(""));
}

TEST(ToolInput, HasFieldReturnsFalseForInvalidOrNonObjectJson) {
    EXPECT_FALSE(cc::core::ToolInput::from_json(R"("cwd")").has_field("cwd"));
    EXPECT_FALSE(cc::core::ToolInput::from_json(R"(["cwd"])").has_field("cwd"));
    EXPECT_FALSE(cc::core::ToolInput::from_json(R"({"cwd")").has_field("cwd"));
}

TEST(Tools, BashToolCapturesStderrAndNonZeroExitCode) {
    cc::tools::BashTool tool;

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "printf out; printf err >&2; exit 7"
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("err"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("out"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("Exit code: 7"), std::string::npos);
}

TEST(Tools, BashToolUsesCwdWithoutShellInterpolatingIt) {
    auto root = fs::temp_directory_path() / "cc repl bash cwd test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::tools::BashTool tool;
    auto input = std::format(R"({{"command":"pwd","cwd":"{}"}})", root.string());
    auto result = tool.execute(cc::core::ToolInput::from_json(input));

    fs::remove_all(root);

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find(root.string()), std::string::npos);
}

TEST(Tools, BashToolTimesOutLongRunningCommands) {
    cc::tools::BashTool tool;

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "sleep 2",
      "timeout": 50
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Command timed out"), std::string::npos);
}

TEST(Tools, BashToolStartsBackgroundCommands) {
    auto root = fs::temp_directory_path() / "cc_repl_bash_background_test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::tools::BashTool tool;
    auto input = std::format(R"({{"command":"printf start; sleep 0.1; printf done > background.txt; printf done","cwd":"{}","run_in_background":true}})",
        root.string());
    auto result = tool.execute(cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Background task started"), std::string::npos);
    auto task_id = extract_background_task_id(result->content.front().text);
    ASSERT_TRUE(task_id.has_value()) << result->content.front().text;
    EXPECT_EQ(result->content.front().text.find("coming soon"), std::string::npos);

    const auto output_path = root / "background.txt";
    for (int attempt = 0; attempt < 20 && !fs::exists(output_path); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(fs::exists(output_path));

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    std::string task_output;
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto output = registry.execute("task_output", cc::core::ToolInput::from_json(
            std::format(R"({{"task_id":"{}"}})", *task_id)));
        ASSERT_TRUE(output.has_value());
        ASSERT_FALSE(output->content.empty());
        task_output = output->content.front().text;
        if (task_output.find("start") != std::string::npos &&
            task_output.find("done") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_NE(task_output.find("Task: " + *task_id), std::string::npos);
    EXPECT_NE(task_output.find("Output:"), std::string::npos);
    EXPECT_NE(task_output.find("start"), std::string::npos);
    EXPECT_NE(task_output.find("done"), std::string::npos);

    fs::remove_all(root);
}

TEST(Tools, BashToolTagsBackgroundTasksWithAgentId) {
    auto parsed = cc::tools::bash::BashToolInput::from_json(R"({
      "command": "printf scoped",
      "run_in_background": true,
      "agent_id": "bash-agent-scope"
    })");
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    ASSERT_TRUE(parsed->agent_id.has_value());
    EXPECT_EQ(*parsed->agent_id, "bash-agent-scope");

    cc::tools::BashTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "trap 'printf stopped; exit 0' TERM; printf ready; sleep 5",
      "run_in_background": true,
      "agent_id": "bash-agent-scope"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    auto task_id = extract_background_task_id(result->content.front().text);
    ASSERT_TRUE(task_id.has_value()) << result->content.front().text;

    auto snapshot = cc::tools::bash::get_background_task_snapshot(*task_id);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->agent_id.has_value());
    EXPECT_EQ(*snapshot->agent_id, "bash-agent-scope");

    auto stopped = cc::tools::bash::stop_background_tasks_for_agent("bash-agent-scope");
    ASSERT_EQ(stopped.size(), 1u);
    EXPECT_EQ(stopped.front().id, *task_id);
    EXPECT_TRUE(stopped.front().stopped);
    cc::tools::bash::drain_all_background_tasks();
}

TEST(Tools, AgentShellTaskCleanupGuardStopsAgentOwnedBackgroundTasks) {
    cc::tools::BashTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "trap 'printf stopped-by-agent; exit 0' TERM; printf guard-ready; sleep 5",
      "run_in_background": true,
      "agentId": "agent-cleanup-guard"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    auto task_id = extract_background_task_id(result->content.front().text);
    ASSERT_TRUE(task_id.has_value()) << result->content.front().text;

    {
        cc::tools::agent::AgentShellTaskCleanupGuard guard{"agent-cleanup-guard"};
    }

    auto snapshot = cc::tools::bash::get_background_task_snapshot(*task_id);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->stopped);
    EXPECT_TRUE(snapshot->agent_id.has_value());
    EXPECT_EQ(*snapshot->agent_id, "agent-cleanup-guard");
}

TEST(Tools, TaskStopStopsBackgroundBashCommands) {
    cc::tools::BashTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "trap 'printf stopped; exit 0' TERM; printf ready; sleep 5",
      "run_in_background": true
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->content.empty());
    auto task_id = extract_background_task_id(result->content.front().text);
    ASSERT_TRUE(task_id.has_value()) << result->content.front().text;

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto output = registry.execute("task_output", cc::core::ToolInput::from_json(
            std::format(R"({{"task_id":"{}"}})", *task_id)));
        ASSERT_TRUE(output.has_value());
        ASSERT_FALSE(output->content.empty());
        if (output->content.front().text.find("ready") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(
        std::format(R"({{"task_id":"{}"}})", *task_id)));
    ASSERT_TRUE(stopped.has_value());
    EXPECT_FALSE(stopped->is_error);
    ASSERT_FALSE(stopped->content.empty());
    EXPECT_NE(stopped->content.front().text.find("Status: stopped"), std::string::npos);

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(
        std::format(R"({{"task_id":"{}"}})", *task_id)));
    ASSERT_TRUE(output.has_value());
    EXPECT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("Status: stopped"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("Output:"), std::string::npos);
    cc::tools::bash::drain_all_background_tasks();
}

TEST(Tools, TaskOutputAndStopAcceptBackgroundProcessPid) {
    cc::tools::BashTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "trap 'printf stopped-by-pid; exit 0' TERM; printf pid-ready; sleep 5",
      "run_in_background": true
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    auto pid = extract_background_pid(result->content.front().text);
    ASSERT_TRUE(pid.has_value()) << result->content.front().text;

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    std::string output_text;
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto output = registry.execute("task_output", cc::core::ToolInput::from_json(
            std::format(R"({{"pid":{}}})", *pid)));
        ASSERT_TRUE(output.has_value());
        ASSERT_FALSE(output->is_error);
        ASSERT_FALSE(output->content.empty());
        output_text = output->content.front().text;
        if (output_text.find("pid-ready") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_NE(output_text.find("PID: " + *pid), std::string::npos);
    EXPECT_NE(output_text.find("pid-ready"), std::string::npos);

    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(
        std::format(R"({{"pid":{}}})", *pid)));
    ASSERT_TRUE(stopped.has_value());
    EXPECT_FALSE(stopped->is_error);
    ASSERT_FALSE(stopped->content.empty());
    EXPECT_NE(stopped->content.front().text.find("Status: stopped"), std::string::npos);

    auto final_output = registry.execute("task_output", cc::core::ToolInput::from_json(
        std::format(R"({{"pid":{}}})", *pid)));
    ASSERT_TRUE(final_output.has_value());
    EXPECT_FALSE(final_output->is_error);
    ASSERT_FALSE(final_output->content.empty());
    EXPECT_NE(final_output->content.front().text.find("Status: stopped"), std::string::npos);
    EXPECT_NE(final_output->content.front().text.find("Output:"), std::string::npos);
}

TEST(Tools, WebFetchParsesEscapedUrlFromJson) {
    auto parsed = cc::tools::web_fetch::detail::parse_url(R"({"url":"https://example.com/a?x=\"quoted\"&y=1"})");

    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, R"(https://example.com/a?x="quoted"&y=1)");
    EXPECT_FALSE(cc::tools::web_fetch::detail::parse_url(R"({"description":"contains url"})").has_value());
}

TEST(Tools, WebSearchParsesEscapedQueryFromJson) {
    auto parsed = cc::tools::web_search::detail::parse_query(R"({"query":"C++ \"modules\" migration"})");

    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, R"(C++ "modules" migration)");
    EXPECT_FALSE(cc::tools::web_search::detail::parse_query(R"({"description":"contains query"})").has_value());
}

TEST(Tools, WebSearchFormatsDuckDuckGoHtmlResults) {
    const std::string html = R"HTML(
      <div class="result">
        <a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fdocs%3Fx%3D1%26y%3D2&amp;rut=abc">
          Example &amp; Docs
        </a>
        <a class="result__snippet">Docs <b>about</b> migration &amp; testing.</a>
      </div>
      <div class="result">
        <a class="result__a" href="https://second.example/path">Second Result</a>
      </div>
    )HTML";

    auto formatted = cc::tools::web_search::detail::format_results("migration test", html);

    EXPECT_NE(formatted.find("Search results for: migration test"), std::string::npos);
    EXPECT_NE(formatted.find("1. Example & Docs"), std::string::npos);
    EXPECT_NE(formatted.find("https://example.com/docs?x=1&y=2"), std::string::npos);
    EXPECT_NE(formatted.find("Docs about migration & testing."), std::string::npos);
    EXPECT_NE(formatted.find("2. Second Result"), std::string::npos);
    EXPECT_EQ(formatted.find("result__a"), std::string::npos);
}

TEST(Tools, NotebookEditPreservesNotebookJsonStructure) {
    auto root = fs::temp_directory_path() / "cc_repl_notebook_roundtrip_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto notebook_path = root / "sample.ipynb";
    {
        std::ofstream notebook(notebook_path);
        notebook << R"JSON({
  "nbformat": 4,
  "nbformat_minor": 5,
  "metadata": {
    "language_info": {"name": "python"},
    "custom": {"keep": true}
  },
  "cells": [
    {
      "cell_type": "code",
      "id": "abc123",
      "metadata": {"tags": ["keep-me"]},
      "source": ["print('old')\n"],
      "execution_count": 12,
      "outputs": [
        {"output_type": "stream", "name": "stdout", "text": ["old\n"]}
      ]
    },
    {
      "cell_type": "markdown",
      "id": "md1",
      "metadata": {"collapsed": false},
      "source": "unchanged markdown"
    }
  ]
})JSON";
    }

    cc::tools::NotebookEditTool tool;
    auto result = tool.execute(cc::tools::NotebookEditRequest{
        .notebook_path = notebook_path,
        .operation = cc::tools::CellOperation::Update,
        .cell_index = 0,
        .target_index = std::nullopt,
        .cell_type = std::nullopt,
        .source = std::string(R"(print("new value"))"),
    });

    ASSERT_TRUE(result.has_value()) << std::string(cc::tools::format_error(result.error()));
    EXPECT_EQ(result->total_cells, 2u);

    auto parsed = cc::utils::json::parse_file(notebook_path);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto root_json = parsed->root();
    EXPECT_EQ(root_json.get("metadata").get("language_info").get_string("name"), "python");
    EXPECT_TRUE(root_json.get("metadata").get("custom").get("keep").as_bool());

    auto cells = root_json.get("cells");
    ASSERT_TRUE(cells.is_arr());
    ASSERT_EQ(cells.size(), 2u);
    auto first = cells.at(0);
    EXPECT_EQ(first.get_string("cell_type"), "code");
    EXPECT_EQ(first.get_string("id"), "abc123");
    EXPECT_EQ(first.get("metadata").get("tags").at(0).as_str(), "keep-me");
    EXPECT_EQ(first.get_string("source"), R"(print("new value"))");
    EXPECT_TRUE(first.get("execution_count").is_null());
    EXPECT_EQ(first.get("outputs").size(), 0u);

    auto second = cells.at(1);
    EXPECT_EQ(second.get_string("cell_type"), "markdown");
    EXPECT_EQ(second.get_string("id"), "md1");
    EXPECT_FALSE(second.get("metadata").get("collapsed").as_bool());
    EXPECT_EQ(second.get_string("source"), "unchanged markdown");

    fs::remove_all(root);
}

TEST(Tools, NotebookRuntimeAdapterAcceptsTypeScriptInputShape) {
    auto root = fs::temp_directory_path() / "cc_repl_notebook_runtime_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto notebook_path = root / "runtime.ipynb";
    {
        std::ofstream notebook(notebook_path);
        notebook << R"JSON({
  "nbformat": 4,
  "nbformat_minor": 5,
  "metadata": {"language_info": {"name": "python"}},
  "cells": [
    {"cell_type": "markdown", "id": "first", "metadata": {}, "source": "before"}
  ]
})JSON";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"JSONFMT({{
      "notebook_path": "{}",
      "cell_id": "first",
      "edit_mode": "insert",
      "cell_type": "code",
      "new_source": "print(\"inserted\")"
    }})JSONFMT", notebook_path.string());
    auto result = registry.execute("notebook_edit", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Inserted cell at index 1"), std::string::npos);

    auto parsed = cc::utils::json::parse_file(notebook_path);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto cells = parsed->root().get("cells");
    ASSERT_EQ(cells.size(), 2u);
    EXPECT_EQ(cells.at(0).get_string("source"), "before");
    EXPECT_EQ(cells.at(1).get_string("cell_type"), "code");
    EXPECT_TRUE(cells.at(1).has("id"));
    EXPECT_EQ(cells.at(1).get_string("source"), R"(print("inserted"))");
    EXPECT_TRUE(cells.at(1).get("execution_count").is_null());
    EXPECT_EQ(cells.at(1).get("outputs").size(), 0u);

    fs::remove_all(root);
}

TEST(Tools, FileReadFormatsNotebookCellsForToolResult) {
    auto root = fs::temp_directory_path() / "cc_repl_notebook_read_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto notebook_path = root / "read.ipynb";
    {
        std::ofstream notebook(notebook_path);
        notebook << R"JSON({
  "nbformat": 4,
  "nbformat_minor": 5,
  "metadata": {"language_info": {"name": "r"}},
  "cells": [
    {
      "cell_type": "code",
      "id": "code-cell",
      "metadata": {},
      "source": ["print(1)\n"],
      "execution_count": 1,
      "outputs": [
        {"output_type": "stream", "name": "stdout", "text": ["1\n"]}
      ]
    },
    {
      "cell_type": "markdown",
      "metadata": {},
      "source": "markdown text"
    }
  ]
})JSON";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"({{"file_path":"{}"}})", notebook_path.string());
    auto result = registry.execute("Read", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    const auto& text = result->content.front().text;
    EXPECT_NE(text.find("Notebook:"), std::string::npos);
    EXPECT_NE(text.find(R"CHECK(<cell id="code-cell"><language>r</language>print(1))CHECK"), std::string::npos);
    EXPECT_NE(text.find("\n1\n"), std::string::npos);
    EXPECT_NE(text.find(R"(<cell id="cell-1"><cell_type>markdown</cell_type>markdown text</cell id="cell-1">)"), std::string::npos);
    EXPECT_EQ(text.find(R"("nbformat")"), std::string::npos);

    fs::remove_all(root);
}

TEST(Tools, FileReadReturnsImageContentBlock) {
    auto root = fs::temp_directory_path() / "cc_repl_image_read_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto image_path = root / "pixel.png";
    {
        const unsigned char png_header[] = {
            0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
            0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x02,
        };
        std::ofstream image(image_path, std::ios::binary);
        image.write(reinterpret_cast<const char*>(png_header), sizeof(png_header));
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"({{"file_path":"{}"}})", image_path.string());
    auto result = registry.execute("Read", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_EQ(result->content.size(), 2u);
    EXPECT_NE(result->content[0].text.find("Image file read:"), std::string::npos);
    EXPECT_EQ(result->content[1].format, std::optional<std::string>{"image"});
    EXPECT_EQ(result->content[1].media_type, std::optional<std::string>{"image/png"});
    ASSERT_TRUE(result->content[1].data.has_value());
    EXPECT_TRUE(result->content[1].data->starts_with("iVBOR"));

    fs::remove_all(root);
}

TEST(Tools, FileReadReturnsPdfDocumentBlock) {
    auto root = fs::temp_directory_path() / "cc_repl_pdf_read_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto pdf_path = root / "doc.pdf";
    {
        std::ofstream pdf(pdf_path, std::ios::binary);
        pdf << "%PDF-1.4\n%%EOF\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"({{"file_path":"{}"}})", pdf_path.string());
    auto result = registry.execute("Read", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_EQ(result->content.size(), 2u);
    EXPECT_NE(result->content[0].text.find("PDF file read:"), std::string::npos);
    EXPECT_EQ(result->content[1].format, std::optional<std::string>{"document"});
    EXPECT_EQ(result->content[1].media_type, std::optional<std::string>{"application/pdf"});
    ASSERT_TRUE(result->content[1].data.has_value());
    EXPECT_TRUE(result->content[1].data->starts_with("JVBER"));

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeLoadsMarkdownDefinitions) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "reviewer.md");
        agent << R"MD(---
name: reviewer
description: Reviews code changes
model: haiku
tools: [Read, Grep, Bash]
disallowedTools: [Bash]
permissionMode: plan
effort: 77
memory: local
color: cyan
omitClaudeMd: true
criticalSystemReminder_EXPERIMENTAL: Stay within the review scope.
maxTurns: 8
initialPrompt: Inspect only changed files first.
---
You review code changes and report risks.
)MD";
    }

    auto agents = cc::tools::agent_runtime::load_agent_definitions_from_dir(
        root / ".claude" / "agents",
        "projectSettings");

    ASSERT_EQ(agents.size(), 1u);
    EXPECT_EQ(agents.front().agent_type, "reviewer");
    EXPECT_EQ(agents.front().when_to_use, "Reviews code changes");
    EXPECT_EQ(agents.front().model, "haiku");
    ASSERT_EQ(agents.front().tools.size(), 3u);
    EXPECT_EQ(agents.front().tools.front(), "Read");
    ASSERT_EQ(agents.front().disallowed_tools.size(), 1u);
    EXPECT_EQ(agents.front().disallowed_tools.front(), "Bash");
    ASSERT_TRUE(agents.front().permission_mode.has_value());
    EXPECT_EQ(*agents.front().permission_mode, "plan");
    ASSERT_TRUE(agents.front().effort.has_value());
    EXPECT_EQ(*agents.front().effort, "77");
    ASSERT_TRUE(agents.front().memory.has_value());
    EXPECT_EQ(*agents.front().memory, "local");
    ASSERT_TRUE(agents.front().color.has_value());
    EXPECT_EQ(*agents.front().color, "cyan");
    EXPECT_TRUE(agents.front().omit_claude_md);
    ASSERT_TRUE(agents.front().critical_system_reminder.has_value());
    EXPECT_EQ(*agents.front().critical_system_reminder, "Stay within the review scope.");
    ASSERT_TRUE(agents.front().max_turns.has_value());
    EXPECT_EQ(*agents.front().max_turns, 8);
    ASSERT_TRUE(agents.front().initial_prompt.has_value());
    EXPECT_EQ(*agents.front().initial_prompt, "Inspect only changed files first.");

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeLoadsJsonDefinitions) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_json_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agents(root / ".claude" / "agents" / "agents.json");
        agents << R"JSON({
  "json-reviewer": {
    "description": "Reviews JSON-defined agents",
    "prompt": "Review JSON agent migrations.",
    "model": "haiku",
    "tools": ["Read", "Bash(git status)"],
    "disallowedTools": ["Write"],
    "permissionMode": "acceptEdits",
    "effort": "max",
    "memory": "project",
    "color": "green",
    "omitClaudeMd": true,
    "criticalSystemReminder": "Stay in JSON parity scope.",
    "maxTurns": 4,
    "initialPrompt": "Start with the JSON definition.",
    "background": true,
    "isolation": "worktree",
    "requiredMcpServers": ["github"],
    "mcpServers": [
      "filesystem",
      {
        "review": {
          "type": "stdio",
          "command": "node",
          "args": ["server.js"],
          "env": {"TOKEN": "secret"}
        }
      }
    ],
    "hooks": {
      "SubagentStart": [
        {
          "matcher": "json-reviewer",
          "hooks": [
            {
              "type": "command",
              "command": "echo json-hook-started",
              "shell": "bash",
              "timeoutSeconds": 3,
              "if": "always"
            }
          ]
        }
      ]
    },
    "skills": ["review-skill"]
  },
  "numeric-effort-agent": {
    "description": "Uses numeric JSON effort",
    "prompt": "Verify numeric JSON effort parity.",
    "effort": 77
  }
})JSON";
    }

    auto agents = cc::tools::agent_runtime::load_agent_definitions_from_dir(
        root / ".claude" / "agents",
        "projectSettings");

    ASSERT_EQ(agents.size(), 2u);
    auto find_agent = [&](std::string_view type) -> const cc::tools::agent_runtime::AgentDefinition* {
        for (const auto& candidate : agents) {
            if (candidate.agent_type == type) return &candidate;
        }
        return nullptr;
    };

    const auto* agent_ptr = find_agent("json-reviewer");
    ASSERT_NE(agent_ptr, nullptr);
    const auto& agent = *agent_ptr;
    EXPECT_EQ(agent.agent_type, "json-reviewer");
    EXPECT_EQ(agent.when_to_use, "Reviews JSON-defined agents");
    EXPECT_EQ(agent.system_prompt, "Review JSON agent migrations.");
    EXPECT_EQ(agent.model, "haiku");
    ASSERT_EQ(agent.tools.size(), 2u);
    EXPECT_EQ(agent.tools[1], "Bash(git status)");
    ASSERT_EQ(agent.disallowed_tools.size(), 1u);
    EXPECT_EQ(agent.disallowed_tools.front(), "Write");
    ASSERT_TRUE(agent.permission_mode.has_value());
    EXPECT_EQ(*agent.permission_mode, "acceptEdits");
    ASSERT_TRUE(agent.effort.has_value());
    EXPECT_EQ(*agent.effort, "max");
    ASSERT_TRUE(agent.memory.has_value());
    EXPECT_EQ(*agent.memory, "project");
    ASSERT_TRUE(agent.color.has_value());
    EXPECT_EQ(*agent.color, "green");
    EXPECT_TRUE(agent.omit_claude_md);
    ASSERT_TRUE(agent.critical_system_reminder.has_value());
    EXPECT_EQ(*agent.critical_system_reminder, "Stay in JSON parity scope.");
    ASSERT_TRUE(agent.max_turns.has_value());
    EXPECT_EQ(*agent.max_turns, 4);
    ASSERT_TRUE(agent.initial_prompt.has_value());
    EXPECT_EQ(*agent.initial_prompt, "Start with the JSON definition.");
    EXPECT_TRUE(agent.background);
    ASSERT_TRUE(agent.isolation.has_value());
    EXPECT_EQ(*agent.isolation, "worktree");
    ASSERT_EQ(agent.required_mcp_servers.size(), 1u);
    EXPECT_EQ(agent.required_mcp_servers.front(), "github");
    ASSERT_EQ(agent.mcp_servers.size(), 1u);
    EXPECT_EQ(agent.mcp_servers.front(), "filesystem");
    ASSERT_EQ(agent.inline_mcp_servers.size(), 1u);
    EXPECT_EQ(agent.inline_mcp_servers.front().name, "review");
    EXPECT_EQ(agent.inline_mcp_servers.front().command, "node");
    ASSERT_EQ(agent.inline_mcp_servers.front().args.size(), 1u);
    EXPECT_EQ(agent.inline_mcp_servers.front().args.front(), "server.js");
    EXPECT_EQ(agent.inline_mcp_servers.front().env.at("TOKEN"), "secret");
    EXPECT_TRUE(agent.hooks_present);
    ASSERT_TRUE(agent.hooks.contains("SubagentStart"));
    ASSERT_EQ(agent.hooks.at("SubagentStart").size(), 1u);
    ASSERT_TRUE(agent.hooks.at("SubagentStart").front().matcher.has_value());
    EXPECT_EQ(*agent.hooks.at("SubagentStart").front().matcher, "json-reviewer");
    ASSERT_EQ(agent.hooks.at("SubagentStart").front().hooks.size(), 1u);
    const auto& hook = agent.hooks.at("SubagentStart").front().hooks.front();
    EXPECT_EQ(hook.command, "echo json-hook-started");
    EXPECT_EQ(hook.shell, "bash");
    ASSERT_TRUE(hook.timeout_seconds.has_value());
    EXPECT_EQ(*hook.timeout_seconds, 3);
    ASSERT_TRUE(hook.condition.has_value());
    EXPECT_EQ(*hook.condition, "always");
    ASSERT_EQ(agent.skills.size(), 1u);
    EXPECT_EQ(agent.skills.front(), "review-skill");

    const auto* numeric_effort = find_agent("numeric-effort-agent");
    ASSERT_NE(numeric_effort, nullptr);
    ASSERT_TRUE(numeric_effort->effort.has_value());
    EXPECT_EQ(*numeric_effort->effort, "77");

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeGatesRemoteIsolationToAntUsers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_remote_isolation_gate_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    EnvironmentUnsetGuard user_type_guard("USER_TYPE");

    {
        std::ofstream agent(root / ".claude" / "agents" / "remote.md");
        agent << R"MD(---
name: remote-md-agent
description: Remote markdown agent
isolation: remote
---
Review remotely.
)MD";
    }
    auto markdown_agents = cc::tools::agent_runtime::load_agent_definitions_from_dir(
        root / ".claude" / "agents",
        "projectSettings");
    ASSERT_EQ(markdown_agents.size(), 1u);
    EXPECT_EQ(markdown_agents.front().agent_type, "remote-md-agent");
    EXPECT_FALSE(markdown_agents.front().isolation.has_value());

    {
        std::ofstream agents(root / ".claude" / "agents" / "agents.json");
        agents << R"JSON({
  "remote-json-agent": {
    "description": "Remote JSON agent",
    "prompt": "Review remotely.",
    "isolation": "remote"
  }
})JSON";
    }
    auto json_agents = cc::tools::agent_runtime::parse_agents_json_file(
        root / ".claude" / "agents" / "agents.json",
        "projectSettings");
    EXPECT_TRUE(json_agents.empty());

    {
        EnvironmentGuard ant_user_guard("USER_TYPE", "ant");
        auto ant_agents = cc::tools::agent_runtime::parse_agents_json_file(
            root / ".claude" / "agents" / "agents.json",
            "projectSettings");
        ASSERT_EQ(ant_agents.size(), 1u);
        ASSERT_TRUE(ant_agents.front().isolation.has_value());
        EXPECT_EQ(*ant_agents.front().isolation, "remote");
    }

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeLoadsSettingsFlagAndPolicyAgentsInPriorityOrder) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_settings_priority_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude");
    {
        std::ofstream settings(root / ".claude" / "settings.json");
        settings << R"JSON({
  "agents": {
    "shared-agent": {
      "description": "Project shared agent",
      "prompt": "project prompt",
      "model": "haiku"
    },
    "project-only": {
      "description": "Project only agent",
      "prompt": "project only prompt"
    }
  }
})JSON";
    }
    const auto policy_path = root / "policy-settings.json";
    {
        std::ofstream policy(policy_path);
        policy << R"JSON({
  "agents": {
    "shared-agent": {
      "description": "Policy shared agent",
      "prompt": "policy prompt",
      "model": "opus"
    },
    "policy-only": {
      "description": "Policy only agent",
      "prompt": "policy only prompt"
    }
  }
})JSON";
    }

    EnvironmentGuard flag_agents("CC_REPL_AGENTS_JSON", R"JSON({
  "shared-agent": {
    "description": "Flag shared agent",
    "prompt": "flag prompt",
    "model": "sonnet"
  },
  "flag-only": {
    "description": "Flag only agent",
    "prompt": "flag only prompt"
  }
})JSON");
    EnvironmentUnsetGuard legacy_flag_agents("CLAUDE_CODE_AGENTS_JSON");
    EnvironmentGuard policy_settings("CLAUDE_CODE_POLICY_SETTINGS", policy_path.string());

    auto agents = cc::tools::agent_runtime::get_all_agent_definitions(root);
    auto find_agent = [&](std::string_view type) -> const cc::tools::agent_runtime::AgentDefinition* {
        for (const auto& agent : agents) {
            if (agent.agent_type == type) return &agent;
        }
        return nullptr;
    };

    auto* shared = find_agent("shared-agent");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->source, "policySettings");
    EXPECT_EQ(shared->when_to_use, "Policy shared agent");
    EXPECT_EQ(shared->system_prompt, "policy prompt");
    EXPECT_EQ(shared->model, "opus");

    auto* flag_only = find_agent("flag-only");
    ASSERT_NE(flag_only, nullptr);
    EXPECT_EQ(flag_only->source, "flagSettings");
    EXPECT_EQ(flag_only->system_prompt, "flag only prompt");

    auto* project_only = find_agent("project-only");
    ASSERT_NE(project_only, nullptr);
    EXPECT_EQ(project_only->source, "projectSettings");
    EXPECT_EQ(project_only->system_prompt, "project only prompt");

    auto* policy_only = find_agent("policy-only");
    ASSERT_NE(policy_only, nullptr);
    EXPECT_EQ(policy_only->source, "policySettings");

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeSimpleModeOnlyLoadsBuiltInAgents) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_simple_mode_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "project-only.md");
        agent << R"MD(---
name: project-only
description: Project agent should be hidden in simple mode
---
Project prompt.
)MD";
    }
    const auto policy_path = root / "policy-settings.json";
    {
        std::ofstream policy(policy_path);
        policy << R"JSON({
  "agents": {
    "policy-only": {
      "description": "Policy agent should be hidden in simple mode",
      "prompt": "policy prompt"
    }
  }
})JSON";
    }

    EnvironmentUnsetGuard disable_guard("CLAUDE_AGENT_SDK_DISABLE_BUILTIN_AGENTS");
    EnvironmentGuard simple_mode("CLAUDE_CODE_SIMPLE", "1");
    EnvironmentGuard flag_agents("CC_REPL_AGENTS_JSON", R"JSON({
  "flag-only": {
    "description": "Flag agent should be hidden in simple mode",
    "prompt": "flag prompt"
  }
})JSON");
    EnvironmentUnsetGuard legacy_flag_agents("CLAUDE_CODE_AGENTS_JSON");
    EnvironmentGuard policy_settings("CLAUDE_CODE_POLICY_SETTINGS", policy_path.string());

    auto agents = cc::tools::agent_runtime::get_all_agent_definitions(root);
    auto has_agent = [&](std::string_view type) {
        return std::ranges::any_of(agents, [&](const auto& agent) {
            return agent.agent_type == type;
        });
    };

    EXPECT_TRUE(has_agent("general-purpose"));
    EXPECT_FALSE(has_agent("project-only"));
    EXPECT_FALSE(has_agent("flag-only"));
    EXPECT_FALSE(has_agent("policy-only"));

    fs::remove_all(root);
}

TEST(Tools, BuiltInAgentDefinitionsHonorNativeFeatureGates) {
    EnvironmentUnsetGuard disable_guard("CLAUDE_AGENT_SDK_DISABLE_BUILTIN_AGENTS");
    EnvironmentUnsetGuard explore_guard("CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS");
    EnvironmentUnsetGuard legacy_explore_guard("BUILTIN_EXPLORE_PLAN_AGENTS");
    EnvironmentUnsetGuard verification_guard("CLAUDE_CODE_ENABLE_VERIFICATION_AGENT");
    EnvironmentUnsetGuard legacy_verification_guard("VERIFICATION_AGENT");
    EnvironmentUnsetGuard entrypoint_guard("CLAUDE_CODE_ENTRYPOINT");

    auto has_agent = [](const std::vector<cc::tools::agent_runtime::AgentDefinition>& agents, std::string_view type) {
        return std::ranges::any_of(agents, [&](const auto& agent) {
            return agent.agent_type == type;
        });
    };

    auto defaults = cc::tools::agent_runtime::built_in_agent_definitions();
    EXPECT_TRUE(has_agent(defaults, "general-purpose"));
    EXPECT_TRUE(has_agent(defaults, "statusline-setup"));
    EXPECT_TRUE(has_agent(defaults, "claude-code-guide"));
    EXPECT_FALSE(has_agent(defaults, "Explore"));
    EXPECT_FALSE(has_agent(defaults, "Plan"));
    EXPECT_FALSE(has_agent(defaults, "verification"));
    auto general = std::ranges::find_if(defaults, [](const auto& agent) {
        return agent.agent_type == "general-purpose";
    });
    ASSERT_NE(general, defaults.end());
    EXPECT_EQ(general->tools, std::vector<std::string>{"*"});

    {
        EnvironmentGuard explore_enabled("CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS", "1");
        auto enabled = cc::tools::agent_runtime::built_in_agent_definitions();
        EXPECT_TRUE(has_agent(enabled, "Explore"));
        EXPECT_TRUE(has_agent(enabled, "Plan"));
    }

    {
        EnvironmentGuard verification_enabled("CLAUDE_CODE_ENABLE_VERIFICATION_AGENT", "1");
        auto enabled = cc::tools::agent_runtime::built_in_agent_definitions();
        EXPECT_TRUE(has_agent(enabled, "verification"));
    }

    {
        EnvironmentGuard sdk_entrypoint("CLAUDE_CODE_ENTRYPOINT", "sdk-ts");
        auto sdk_agents = cc::tools::agent_runtime::built_in_agent_definitions();
        EXPECT_FALSE(has_agent(sdk_agents, "claude-code-guide"));
    }

    {
        EnvironmentGuard disabled("CLAUDE_AGENT_SDK_DISABLE_BUILTIN_AGENTS", "1");
        auto interactive_agents = cc::tools::agent_runtime::built_in_agent_definitions();
        EXPECT_FALSE(interactive_agents.empty());
        EXPECT_TRUE(has_agent(interactive_agents, "general-purpose"));
    }

    {
        EnvironmentGuard sdk_entrypoint("CLAUDE_CODE_ENTRYPOINT", "sdk-cli");
        EnvironmentGuard disabled("CLAUDE_AGENT_SDK_DISABLE_BUILTIN_AGENTS", "1");
        EXPECT_TRUE(cc::tools::agent_runtime::built_in_agent_definitions().empty());
    }
}

TEST(Tools, AgentRuntimeResolvesLooseAgentTypeInputs) {
    EnvironmentGuard explore_enabled("CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS", "1");
    auto agents = cc::tools::agent_runtime::built_in_agent_definitions();

    auto general = cc::tools::agent_runtime::resolve_requested_agent_type("General Purpose", agents);
    ASSERT_TRUE(general.has_value());
    EXPECT_EQ(*general, "general-purpose");

    auto planner = cc::tools::agent_runtime::resolve_requested_agent_type("planner", agents);
    ASSERT_TRUE(planner.has_value());
    EXPECT_EQ(*planner, "Plan");

    auto explorer = cc::tools::agent_runtime::resolve_requested_agent_type("explorer", agents);
    ASSERT_TRUE(explorer.has_value());
    EXPECT_EQ(*explorer, "Explore");

    EXPECT_FALSE(cc::tools::agent_runtime::resolve_requested_agent_type("missing-agent-type", agents).has_value());
}

TEST(Tools, AgentToolAcceptsTypeScriptInputShape) {
    EnvironmentGuard explore_enabled("CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS", "1");
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Inspect plan",
      "prompt": "Inspect the migration plan",
      "subagent_type": "planner",
      "model": "haiku"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("Missing required"), std::string::npos);
}

TEST(Tools, AgentToolRejectsUnknownAgentTypesBeforeExecution) {
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Missing agent",
      "prompt": "Use an unknown agent",
      "subagent_type": "cc-repl-missing-agent-type"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent type 'cc-repl-missing-agent-type' not found"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("recursion depth"), std::string::npos);
}

TEST(Tools, AgentToolLoadsProjectAgentDefinitions) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_tool_project_test";
    auto previous_cwd = fs::current_path();
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "project-reviewer.md");
        agent << R"MD(---
name: project-reviewer
description: Reviews project changes
model: haiku
tools: [Read, Grep]
maxTurns: 2
---
Review the project change and report concrete risks.
)MD";
    }

    fs::current_path(root);
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Review changes",
      "prompt": "Review this migration change",
      "subagent_type": "project-reviewer"
    })"));

    fs::current_path(previous_cwd);
    fs::remove_all(root);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("not found"), std::string::npos);
}

TEST(Tools, AgentToolAppliesInitialPromptAndToolRestrictionsInExecutionPlan) {
    EnvironmentUnsetGuard auto_memory_guard("CLAUDE_CODE_DISABLE_AUTO_MEMORY");
    auto root = fs::temp_directory_path() / "cc_repl_agent_plan_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "restricted.md");
        agent << R"MD(---
name: restricted-reviewer
description: Reviews with restricted tools
model: haiku
tools: [Read, Bash]
disallowedTools: [Bash]
permissionMode: acceptEdits
effort: high
memory: project
color: purple
omitClaudeMd: true
criticalSystemReminder: Stay focused on migration risk.
initialPrompt: First inspect the diff.
maxTurns: 2
---
Review with a narrow tool set.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "restricted-reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        EXPECT_EQ(plan->agent_type, "restricted-reviewer");
        EXPECT_EQ(plan->model, "claude-3-5-haiku-20241022");
        EXPECT_EQ(plan->max_turns, 2);
        ASSERT_TRUE(plan->mode.has_value());
        EXPECT_EQ(*plan->mode, "acceptEdits");
        ASSERT_TRUE(plan->effort.has_value());
        EXPECT_EQ(*plan->effort, "high");
        ASSERT_TRUE(plan->memory.has_value());
        EXPECT_EQ(*plan->memory, "project");
        ASSERT_TRUE(plan->color.has_value());
        EXPECT_EQ(*plan->color, "purple");
        EXPECT_TRUE(plan->omit_claude_md);
        ASSERT_TRUE(plan->critical_system_reminder.has_value());
        EXPECT_EQ(*plan->critical_system_reminder, "Stay focused on migration risk.");
        EXPECT_NE(plan->system_prompt.find("- effort: high"), std::string::npos);
        EXPECT_NE(plan->system_prompt.find("- memory: project"), std::string::npos);
        EXPECT_NE(plan->system_prompt.find("- color: purple"), std::string::npos);
        EXPECT_NE(plan->system_prompt.find("- omit_claude_md: true"), std::string::npos);
        EXPECT_NE(plan->system_prompt.find("<critical_system_reminder>"), std::string::npos);
        EXPECT_NE(plan->system_prompt.find("# Persistent Agent Memory"), std::string::npos);
        EXPECT_NE(plan->system_prompt.find("project-scope"), std::string::npos);
        EXPECT_NE(plan->system_prompt.find((root / ".claude" / "agent-memory" / "restricted-reviewer").string()), std::string::npos);
        EXPECT_NE(plan->prompt.find("First inspect the diff.\n\nReview this change."), std::string::npos);
        EXPECT_TRUE(std::ranges::contains(plan->allowed_tools, "Read"));
        EXPECT_TRUE(std::ranges::contains(plan->allowed_tools, "Bash"));
        EXPECT_TRUE(std::ranges::contains(plan->allowed_tools, "Write"));
        EXPECT_TRUE(std::ranges::contains(plan->allowed_tools, "Edit"));
        EXPECT_TRUE(fs::exists(root / ".claude" / "agent-memory" / "restricted-reviewer"));
        ASSERT_EQ(plan->disallowed_tools.size(), 1u);
        EXPECT_EQ(plan->disallowed_tools.front(), "Bash");
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolPermissionModeHonorsParentPrecedenceInExecutionPlan) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_permission_mode_precedence_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "permission-planner.md");
        agent << R"MD(---
name: permission-planner
description: Plans with explicit agent permission mode
permissionMode: plan
---
Plan the assigned work.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Plan a migration change.";
        request.subagent_type = "permission-planner";

        auto default_plan = cc::tools::agent::build_agent_execution_plan(request, cc::tools::AgentConfig{});
        ASSERT_TRUE(default_plan.has_value()) << default_plan.error();
        ASSERT_TRUE(default_plan->mode.has_value());
        EXPECT_EQ(*default_plan->mode, "plan");
        EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
            "ExitPlanMode",
            false,
            false,
            std::optional<std::string_view>{std::string_view{*default_plan->mode}}));

        for (const auto parent_mode : std::array<std::string_view, 3>{
                 "acceptEdits",
                 "bypassPermissions",
                 "auto",
             }) {
            cc::tools::AgentConfig config;
            config.parent_permission_mode = std::string{parent_mode};

            auto protected_plan = cc::tools::agent::build_agent_execution_plan(request, config);
            ASSERT_TRUE(protected_plan.has_value()) << protected_plan.error();
            ASSERT_TRUE(protected_plan->mode.has_value());
            EXPECT_EQ(*protected_plan->mode, parent_mode);
            EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
                "ExitPlanMode",
                false,
                false,
                std::optional<std::string_view>{std::string_view{*protected_plan->mode}}));
        }
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolHonorsDisabledAutoMemoryForAgentDefinitions) {
    EnvironmentGuard disable_memory("CLAUDE_CODE_DISABLE_AUTO_MEMORY", "1");
    auto root = fs::temp_directory_path() / "cc_repl_agent_memory_disabled_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "memory-disabled.md");
        agent << R"MD(---
name: memory-disabled
description: Agent with memory disabled by env
model: haiku
tools: [Grep]
memory: local
---
Do focused work.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Check memory env behavior.";
        request.subagent_type = "memory-disabled";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_TRUE(plan->memory.has_value());
        EXPECT_EQ(*plan->memory, "local");
        ASSERT_EQ(plan->allowed_tools.size(), 1u);
        EXPECT_EQ(plan->allowed_tools.front(), "Grep");
        EXPECT_EQ(plan->system_prompt.find("# Persistent Agent Memory"), std::string::npos);
        EXPECT_FALSE(fs::exists(root / ".claude" / "agent-memory-local" / "memory-disabled"));
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolAppliesAgentEffortToApiRequest) {
    EnvironmentUnsetGuard user_type_guard("USER_TYPE");
    EnvironmentUnsetGuard always_effort_guard("CLAUDE_CODE_ALWAYS_ENABLE_EFFORT");

    {
        cc::services::api::CreateMessageRequest request;
        request.model = "claude-sonnet-4-6-20260601";
        cc::tools::agent::apply_agent_effort_to_request(
            request,
            std::optional<std::string>{" high "});

        ASSERT_TRUE(request.output_effort.has_value());
        EXPECT_EQ(*request.output_effort, "high");
        EXPECT_TRUE(std::ranges::contains(request.betas, "effort-2025-11-24"));
        EXPECT_FALSE(request.internal_effort_override.has_value());
    }

    {
        cc::services::api::CreateMessageRequest request;
        request.model = "claude-sonnet-4-6-20260601";
        cc::tools::agent::apply_agent_effort_to_request(
            request,
            std::optional<std::string>{"max"});

        ASSERT_TRUE(request.output_effort.has_value());
        EXPECT_EQ(*request.output_effort, "high");
    }

    {
        cc::services::api::CreateMessageRequest request;
        request.model = "claude-opus-4-6-20260601";
        cc::tools::agent::apply_agent_effort_to_request(
            request,
            std::optional<std::string>{"max"});

        ASSERT_TRUE(request.output_effort.has_value());
        EXPECT_EQ(*request.output_effort, "max");
    }

    {
        cc::services::api::CreateMessageRequest request;
        request.model = "claude-3-5-haiku-20241022";
        cc::tools::agent::apply_agent_effort_to_request(
            request,
            std::optional<std::string>{"high"});

        EXPECT_FALSE(request.output_effort.has_value());
        EXPECT_TRUE(request.betas.empty());
    }

    {
        cc::services::api::CreateMessageRequest request;
        request.model = "claude-sonnet-4-6-20260601";
        cc::tools::agent::apply_agent_effort_to_request(
            request,
            std::optional<std::string>{"77"});

        EXPECT_FALSE(request.output_effort.has_value());
        EXPECT_FALSE(request.internal_effort_override.has_value());
    }

    {
        EnvironmentGuard ant_user("USER_TYPE", "ant");
        cc::services::api::CreateMessageRequest request;
        request.model = "claude-sonnet-4-6-20260601";
        cc::tools::agent::apply_agent_effort_to_request(
            request,
            std::optional<std::string>{"77"});

        ASSERT_TRUE(request.internal_effort_override.has_value());
        EXPECT_EQ(*request.internal_effort_override, 77);
        EXPECT_FALSE(request.output_effort.has_value());
    }
}

TEST(Tools, AgentToolPropagatesParentAgentIdIntoExecutionPlanAndRecord) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_parent_id_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "child-reviewer.md");
        agent << R"MD(---
name: child-reviewer
description: Reviews as a nested child agent
model: haiku
---
Review as a child agent.
)MD";
    }

    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.parent_agent_id = "parent-agent-1";

        cc::tools::agent::AgentToolRequest request;
        request.description = "Nested child review";
        request.prompt = "Review nested context.";
        request.subagent_type = "child-reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_TRUE(plan->parent_agent_id.has_value());
        EXPECT_EQ(*plan->parent_agent_id, "parent-agent-1");
        EXPECT_NE(plan->system_prompt.find("- parent_agent_id: parent-agent-1"), std::string::npos);

        cc::tools::agent::upsert_agent_record_for_plan(*plan);
        auto record = cc::tools::agent_runtime::native_agent_store().get(plan->agent_id);
        ASSERT_TRUE(record.has_value());
        ASSERT_TRUE(record->parent_agent_id.has_value());
        EXPECT_EQ(*record->parent_agent_id, "parent-agent-1");
        ASSERT_TRUE(record->description.has_value());
        EXPECT_EQ(*record->description, "Nested child review");
        EXPECT_EQ(record->agent_type, "child-reviewer");

        cc::tools::agent_runtime::native_agent_store().clear_for_testing();
        auto restored = cc::tools::agent_runtime::native_agent_store().get(plan->agent_id);
        ASSERT_TRUE(restored.has_value());
        ASSERT_TRUE(restored->description.has_value());
        EXPECT_EQ(*restored->description, "Nested child review");

        cc::tools::AgentTool tool(config);
        auto child_config = tool.child_config(plan->agent_id);
        ASSERT_TRUE(child_config.parent_agent_id.has_value());
        EXPECT_EQ(*child_config.parent_agent_id, plan->agent_id);
    }

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolMarksForkChildContextAndRejectsImplicitNestedFork) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_live_fork_guard_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto parsed = cc::tools::agent::parse_agent_tool_request(cc::core::ToolInput::from_json(R"({
      "description": "Fork worker",
      "prompt": "Continue the forked work",
      "subagent_type": "general-purpose",
      "querySource": "agent:builtin:fork"
    })"));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    auto plan = cc::tools::agent::build_agent_execution_plan(*parsed, cc::tools::AgentConfig{});
    ASSERT_TRUE(plan.has_value()) << plan.error();
    EXPECT_TRUE(plan->fork_child_context);
    EXPECT_TRUE(cc::tools::agent::should_reject_fork_child_agent_call(
        *plan,
        "Agent",
        R"({"description":"nested fork","prompt":"Split this work again"})"));
    EXPECT_FALSE(cc::tools::agent::should_reject_fork_child_agent_call(
        *plan,
        "Agent",
        R"({"description":"explicit child","prompt":"Run explicit child","subagent_type":"general-purpose"})"));
    EXPECT_FALSE(cc::tools::agent::should_reject_fork_child_agent_call(
        *plan,
        "Read",
        R"({"file_path":"README.md"})"));

    auto boilerplate = cc::tools::agent::parse_agent_tool_request(cc::core::ToolInput::from_json(R"({
      "description": "Fork worker from transcript",
      "prompt": "<fork-boilerplate>\nSTOP. READ THIS FIRST.\n</fork-boilerplate>",
      "subagent_type": "general-purpose"
    })"));
    ASSERT_TRUE(boilerplate.has_value()) << boilerplate.error();
    auto boilerplate_plan = cc::tools::agent::build_agent_execution_plan(*boilerplate, cc::tools::AgentConfig{});
    ASSERT_TRUE(boilerplate_plan.has_value()) << boilerplate_plan.error();
    EXPECT_TRUE(boilerplate_plan->fork_child_context);

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "persisted-fork-worker",
        .agent_type = "general-purpose",
        .cwd = root.string(),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .capabilities = {"fork-subagent", "Read"},
        .transcript = {"system: forked from parent-agent"},
    });
    cc::tools::agent::AgentToolRequest resumed;
    resumed.agent_id_override = "persisted-fork-worker";
    resumed.resume_existing = true;
    resumed.prompt = "Resume persisted fork worker.";
    resumed.subagent_type = "general-purpose";
    auto resumed_plan = cc::tools::agent::build_agent_execution_plan(resumed, cc::tools::AgentConfig{});
    ASSERT_TRUE(resumed_plan.has_value()) << resumed_plan.error();
    EXPECT_TRUE(resumed_plan->fork_child_context);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsForkParentPromptContextAndExactTools) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_fork_context_plan_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto parsed = cc::tools::agent::parse_agent_tool_request(cc::core::ToolInput::from_json(R"({
      "description": "Fork with inherited context",
      "prompt": "Your directive: inspect parser parity",
      "subagent_type": "general-purpose",
      "parent_system_prompt": "parent rendered system prompt bytes",
      "useExactTools": true,
      "exactTools": ["Read", "Agent"],
      "forkContextMessages": [
        {
          "type": "assistant",
          "message": {
            "role": "assistant",
            "content": [
              {"type":"text","text":"parent answer"},
              {"type":"tool_use","id":"read-1","name":"Read","input":{"file_path":"README.md"}}
            ]
          }
        },
        {
          "role": "user",
          "content": [
            {"type":"tool_result","tool_use_id":"read-1","content":[{"type":"text","text":"README content"}]}
          ]
        }
      ]
    })"));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    auto plan = cc::tools::agent::build_agent_execution_plan(*parsed, cc::tools::AgentConfig{});
    ASSERT_TRUE(plan.has_value()) << plan.error();
    EXPECT_TRUE(plan->system_prompt_overridden);
    EXPECT_EQ(plan->system_prompt, "parent rendered system prompt bytes");
    EXPECT_EQ(plan->system_prompt.find("- agent_id:"), std::string::npos);
    EXPECT_TRUE(plan->use_exact_tools);
    ASSERT_EQ(plan->exact_tools.size(), 2u);
    EXPECT_TRUE(cc::tools::agent::exact_tools_allow_tool(*plan, "Read"));
    EXPECT_TRUE(cc::tools::agent::exact_tools_allow_tool(*plan, "Agent"));
    EXPECT_FALSE(cc::tools::agent::exact_tools_allow_tool(*plan, "Write"));

    ASSERT_EQ(plan->fork_context_messages.size(), 2u);
    EXPECT_EQ(plan->fork_context_messages[0].role, "assistant");
    ASSERT_EQ(plan->fork_context_messages[0].content.size(), 2u);
    EXPECT_EQ(plan->fork_context_messages[0].content[0].text, "parent answer");
    EXPECT_EQ(plan->fork_context_messages[0].content[1].type, cc::services::api::ContentBlockType::ToolUse);
    EXPECT_EQ(plan->fork_context_messages[0].content[1].tool_use_id, "read-1");
    EXPECT_EQ(plan->fork_context_messages[0].content[1].tool_name, "Read");
    EXPECT_NE(plan->fork_context_messages[0].content[1].tool_input_json.find("README.md"), std::string::npos);
    EXPECT_EQ(plan->fork_context_messages[1].role, "user");
    ASSERT_EQ(plan->fork_context_messages[1].content.size(), 1u);
    EXPECT_EQ(plan->fork_context_messages[1].content[0].type, cc::services::api::ContentBlockType::ToolResult);
    EXPECT_EQ(plan->fork_context_messages[1].content[0].tool_use_id, "read-1");
    EXPECT_EQ(plan->fork_context_messages[1].content[0].text, "README content");

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolBuildsTsForkContextFromParentAssistantMessage) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_live_parent_fork_context_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto parsed = cc::tools::agent::parse_agent_tool_request(cc::core::ToolInput::from_json(R"({
      "description": "Implicit fork with live parent message",
      "prompt": "Audit parser migration parity",
      "parentSystemPrompt": "rendered parent prompt bytes",
      "exactTools": ["Read", "Bash", "Agent"],
      "parentAssistantMessage": {
        "type": "assistant",
        "message": {
          "role": "assistant",
          "content": [
            {"type":"thinking","thinking":"parent private reasoning","signature":"sig-1"},
            {"type":"text","text":"I will split this into a fork."},
            {"type":"tool_use","id":"agent-1","name":"Agent","input":{"description":"fork","prompt":"Audit parser migration parity"}},
            {"type":"tool_use","id":"read-1","name":"Read","input":{"file_path":"README.md"}}
          ]
        }
      }
    })"));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    auto plan = cc::tools::agent::build_agent_execution_plan(*parsed, cc::tools::AgentConfig{});
    ASSERT_TRUE(plan.has_value()) << plan.error();
    EXPECT_TRUE(plan->fork_child_context);
    EXPECT_TRUE(plan->fork_context_includes_prompt);
    EXPECT_TRUE(plan->system_prompt_overridden);
    EXPECT_EQ(plan->system_prompt, "rendered parent prompt bytes");
    EXPECT_TRUE(plan->use_exact_tools);
    EXPECT_TRUE(cc::tools::agent::exact_tools_allow_tool(*plan, "Agent"));
    EXPECT_FALSE(cc::tools::agent::exact_tools_allow_tool(*plan, "Write"));

    ASSERT_EQ(plan->fork_context_messages.size(), 2u);
    EXPECT_EQ(plan->fork_context_messages[0].role, "assistant");
    ASSERT_EQ(plan->fork_context_messages[0].content.size(), 4u);
    EXPECT_EQ(plan->fork_context_messages[0].content[0].type, cc::services::api::ContentBlockType::Thinking);
    EXPECT_EQ(plan->fork_context_messages[0].content[2].type, cc::services::api::ContentBlockType::ToolUse);
    EXPECT_EQ(plan->fork_context_messages[0].content[2].tool_use_id, "agent-1");
    EXPECT_EQ(plan->fork_context_messages[0].content[3].tool_use_id, "read-1");

    EXPECT_EQ(plan->fork_context_messages[1].role, "user");
    ASSERT_EQ(plan->fork_context_messages[1].content.size(), 3u);
    const std::string placeholder = "Fork started \u2014 processing in background";
    EXPECT_EQ(plan->fork_context_messages[1].content[0].type, cc::services::api::ContentBlockType::ToolResult);
    EXPECT_EQ(plan->fork_context_messages[1].content[0].tool_use_id, "agent-1");
    EXPECT_EQ(plan->fork_context_messages[1].content[0].text, placeholder);
    EXPECT_EQ(plan->fork_context_messages[1].content[1].tool_use_id, "read-1");
    EXPECT_EQ(plan->fork_context_messages[1].content[1].text, placeholder);
    EXPECT_EQ(plan->fork_context_messages[1].content[2].type, cc::services::api::ContentBlockType::Text);
    EXPECT_NE(plan->fork_context_messages[1].content[2].text.find("<fork-boilerplate>"), std::string::npos);
    EXPECT_NE(plan->fork_context_messages[1].content[2].text.find("Your response MUST begin with \"Scope:\""), std::string::npos);
    EXPECT_NE(plan->fork_context_messages[1].content[2].text.find("Your directive: Audit parser migration parity"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolInjectsImplicitForkInputsAtAgentCallSite) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_implicit_fork_injection_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent::AgentExecutionPlan parent_plan;
    parent_plan.agent_id = "parent-agent";
    parent_plan.agent_type = "general-purpose";
    parent_plan.prompt = "Parent task";
    parent_plan.model = "test-model";
    parent_plan.system_prompt = "parent rendered system prompt bytes";

    cc::services::api::Message parent_assistant;
    parent_assistant.role = "assistant";
    parent_assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::Text,
        .text = "Spawning an implicit fork",
    });
    parent_assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolUse,
        .tool_use_id = "agent-tool-1",
        .tool_name = "Agent",
        .tool_input_json = R"({"description":"fork","prompt":"Audit migration"})",
    });

    std::vector<cc::services::api::ToolDefinition> parent_tools{
        {.name = "Read", .description = "read", .input_schema_json = "{}"},
        {.name = "Agent", .description = "agent", .input_schema_json = "{}"},
        {.name = "Bash", .description = "bash", .input_schema_json = "{}"},
    };

    auto injected = cc::tools::agent::build_implicit_fork_agent_input_json(
        R"({"description":"fork","prompt":"Audit migration","run_in_background":false})",
        parent_plan,
        parent_assistant,
        parent_tools);
    auto parsed_json = cc::utils::json::parse(injected);
    ASSERT_TRUE(parsed_json.has_value()) << parsed_json.error().format();
    auto root_json = parsed_json->root();
    EXPECT_TRUE(root_json.get("run_in_background").is_bool());
    EXPECT_TRUE(root_json.get("run_in_background").as_bool());
    EXPECT_EQ(root_json.get("querySource").as_str(), "agent:builtin:fork");
    EXPECT_EQ(root_json.get("parentSystemPrompt").as_str(), "parent rendered system prompt bytes");
    ASSERT_TRUE(root_json.get("exactTools").is_arr());
    EXPECT_EQ(root_json.get("exactTools").size(), 3u);
    ASSERT_TRUE(root_json.get("parentAssistantMessage").is_obj());

    auto parsed_request = cc::tools::agent::parse_agent_tool_request(cc::core::ToolInput::from_json(injected));
    ASSERT_TRUE(parsed_request.has_value()) << parsed_request.error();
    auto child_plan = cc::tools::agent::build_agent_execution_plan(*parsed_request, cc::tools::AgentConfig{});
    ASSERT_TRUE(child_plan.has_value()) << child_plan.error();
    EXPECT_TRUE(child_plan->background);
    EXPECT_TRUE(child_plan->fork_child_context);
    EXPECT_TRUE(child_plan->fork_context_includes_prompt);
    EXPECT_TRUE(child_plan->system_prompt_overridden);
    EXPECT_EQ(child_plan->system_prompt, "parent rendered system prompt bytes");
    ASSERT_EQ(child_plan->fork_context_messages.size(), 2u);
    ASSERT_EQ(child_plan->fork_context_messages[1].content.size(), 2u);
    EXPECT_EQ(child_plan->fork_context_messages[1].content[0].type, cc::services::api::ContentBlockType::ToolResult);
    EXPECT_EQ(child_plan->fork_context_messages[1].content[0].tool_use_id, "agent-tool-1");
    EXPECT_NE(child_plan->fork_context_messages[1].content[1].text.find("Your directive: Audit migration"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolPermissionRulesMatchToolNamesFromParameterizedSpecs) {
    EXPECT_TRUE(cc::tools::agent::tool_name_allowed_by_definition(
        "Bash",
        {"Read", "Bash(git status)"}));
    EXPECT_TRUE(cc::tools::agent::tool_name_allowed_by_definition(
        "Agent",
        {"Agent(reviewer,planner)"}));
    EXPECT_FALSE(cc::tools::agent::tool_name_allowed_by_definition(
        "Write",
        {"Read", "Bash(git status)"}));

    EXPECT_TRUE(cc::tools::agent::tool_name_disallowed_by_definition(
        "Bash",
        {"Bash(rm -rf /tmp/example)"}));
    EXPECT_TRUE(cc::tools::agent::tool_name_disallowed_by_definition(
        "Agent",
        {"Agent(project-reviewer)"}));
    EXPECT_FALSE(cc::tools::agent::tool_name_disallowed_by_definition(
        "Read",
        {"Bash(git status)"}));

    EXPECT_TRUE(cc::tools::agent::agent_type_allowed_by_permission_rules(
        "project-reviewer",
        {"Agent(project-reviewer,planner)"},
        {}));
    EXPECT_FALSE(cc::tools::agent::agent_type_allowed_by_permission_rules(
        "general-purpose",
        {"Agent(project-reviewer,planner)"},
        {}));
    EXPECT_FALSE(cc::tools::agent::agent_type_allowed_by_permission_rules(
        "project-reviewer",
        {"Agent"},
        {"Agent(project-reviewer)"}));
}

TEST(Tools, AgentToolBaseFilteringMatchesTypeScriptToolSets) {
    EnvironmentUnsetGuard user_type_guard("USER_TYPE");
    EnvironmentUnsetGuard nested_guard("CC_REPL_ENABLE_NESTED_AGENTS");

    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "task_output",
        true,
        false));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "TaskOutput",
        true,
        false));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "Agent",
        true,
        false));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "enter_plan_mode",
        true,
        false));

    EXPECT_TRUE(cc::tools::agent::all_agent_disallows_tool("exit_plan_mode"));
    EXPECT_TRUE(cc::tools::agent::custom_agent_disallows_tool("exit_plan_mode"));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "exit_plan_mode",
        true,
        false));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "exit_plan_mode",
        false,
        false));
    EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
        "ExitPlanMode",
        true,
        false,
        std::optional<std::string_view>{"plan"}));

    EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
        "mcp__linear__list_issues",
        false,
        true));
    EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
        "todo_write",
        false,
        true));
    EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
        "TodoWrite",
        false,
        true));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "task_list",
        false,
        true));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "send_message",
        false,
        true));

    EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
        "task_list",
        false,
        true,
        std::nullopt,
        true));
    EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
        "send_message",
        false,
        true,
        std::nullopt,
        true));
    EXPECT_FALSE(cc::tools::agent::agent_base_filter_allows_tool(
        "Agent",
        false,
        true,
        std::nullopt,
        true));
    {
        EnvironmentGuard nested_agents("CC_REPL_ENABLE_NESTED_AGENTS", "1");
        EXPECT_TRUE(cc::tools::agent::agent_base_filter_allows_tool(
            "Agent",
            false,
            true,
            std::nullopt,
            true));
    }
}

TEST(Tools, AgentToolAgentTypePermissionRulesDoNotConstrainWorkerTools) {
    cc::tools::AgentConfig agent_only_config;
    agent_only_config.allowed_tools = {"Agent(restricted-reviewer)"};
    cc::tools::AgentTool agent_only_tool(agent_only_config);

    EXPECT_TRUE(agent_only_tool.is_tool_allowed("Agent"));
    EXPECT_TRUE(agent_only_tool.is_tool_allowed("Read"));
    EXPECT_TRUE(agent_only_tool.is_tool_allowed("Bash"));

    cc::tools::AgentConfig mixed_config;
    mixed_config.allowed_tools = {"Agent(restricted-reviewer)", "Read"};
    cc::tools::AgentTool mixed_tool(mixed_config);

    EXPECT_TRUE(mixed_tool.is_tool_allowed("Agent"));
    EXPECT_TRUE(mixed_tool.is_tool_allowed("Read"));
    EXPECT_FALSE(mixed_tool.is_tool_allowed("Bash"));
}

TEST(Tools, AgentToolRestrictsAgentTypesFromParameterizedPermissionSpecs) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_type_permission_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "restricted-reviewer.md");
        agent << R"MD(---
name: restricted-reviewer
description: Reviews only when explicitly allowed
---
Review the task.
)MD";
    }

    CurrentPathGuard cwd(root);
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    config.allowed_tools = {"Agent(restricted-reviewer)"};
    cc::tools::AgentTool tool(config);

    auto denied = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run general",
      "prompt": "Use a general agent",
      "subagent_type": "general-purpose"
    })"));
    ASSERT_TRUE(denied.has_value());
    ASSERT_TRUE(denied->is_error);
    ASSERT_FALSE(denied->content.empty());
    EXPECT_NE(denied->content.front().text.find("not allowed by current Agent tool permission rules"), std::string::npos);
    EXPECT_EQ(denied->content.front().text.find("recursion depth"), std::string::npos);

    auto allowed = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run reviewer",
      "prompt": "Use the reviewer",
      "subagent_type": "restricted-reviewer"
    })"));
    ASSERT_TRUE(allowed.has_value());
    ASSERT_TRUE(allowed->is_error);
    ASSERT_FALSE(allowed->content.empty());
    EXPECT_NE(allowed->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);

    fs::remove_all(root);
}

TEST(Tools, AgentToolPreloadsSkillsFromDefinition) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_skills_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    fs::create_directories(root / ".claude" / "skills" / "review-skill");
    {
        std::ofstream agent(root / ".claude" / "agents" / "skillful.md");
        agent << R"MD(---
name: skillful-reviewer
description: Reviews with preloaded skills
skills: [review-skill, missing-skill]
---
Review with a preloaded workflow.
)MD";
    }
    {
        std::ofstream skill(root / ".claude" / "skills" / "review-skill" / "SKILL.md");
        skill << R"MD(---
description: Review skill
---
Inspect the patch before reporting findings.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "skillful-reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->preloaded_skill_messages.size(), 1u);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("review-skill"), std::string::npos);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("Inspect the patch"), std::string::npos);
        EXPECT_EQ(plan->preloaded_skill_messages.front().find("missing-skill"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolLoadsPluginAgentsAndPluginSkills) {
    auto root = fs::temp_directory_path() / "cc_repl_plugin_agent_skills_test";
    fs::remove_all(root);
    const auto plugin_root = root / ".claude" / "plugins" / "plugin-fixture";
    fs::create_directories(plugin_root / "agents");
    fs::create_directories(plugin_root / "skills" / "review-skill");
    const auto server_path = plugin_root / "server.js";
    {
        std::ofstream entry(plugin_root / "plugin.js");
        entry << "process.exit(0)\n";
    }
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: process.env.SERVER_NAME || 'plugin-agent-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{
          name: process.env.TOOL_NAME || 'lookup',
          description: ['plugin tool', process.env.SERVER_NAME].filter(Boolean).join(':'),
          inputSchema: { type: 'object' }
        }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << R"JSON({
  "name": "plugin-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "description": "Plugin fixture"
})JSON";
    }
    {
        std::ofstream agent(plugin_root / "agents" / "reviewer.md");
        agent << std::format(R"MD(---
name: reviewer
description: Reviews using plugin resources
skills: [review-skill]
requiredMcpServers: [review-context]
mcpServers:
  - review-context
  - inline-review:
      type: stdio
      command: node
      args:
        - "{}"
      env:
        SERVER_NAME: plugin-fixture:inline-review
        TOOL_NAME: inline_lookup
hooks:
  SubagentStart:
    - command: "echo plugin-hook-started"
---
Review with plugin context.
)MD", server_path.string());
    }
    {
        std::ofstream skill(plugin_root / "skills" / "review-skill" / "SKILL.md");
        skill << R"MD(---
description: Plugin review skill
---
Use the plugin review checklist.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        auto agents = cc::tools::agent_runtime::get_all_agent_definitions();
        auto it = std::ranges::find_if(agents, [](const auto& agent) {
            return agent.agent_type == "plugin-fixture:reviewer";
        });
        ASSERT_NE(it, agents.end());
        EXPECT_EQ(it->source, "plugin");
        EXPECT_TRUE(it->hooks_present);
        EXPECT_TRUE(it->hooks.contains("SubagentStart"));
        ASSERT_EQ(it->required_mcp_servers.size(), 1u);
        EXPECT_EQ(it->required_mcp_servers.front(), "plugin:plugin-fixture:review-context");
        ASSERT_EQ(it->mcp_servers.size(), 1u);
        EXPECT_EQ(it->mcp_servers.front(), "plugin:plugin-fixture:review-context");
        ASSERT_EQ(it->inline_mcp_servers.size(), 1u);
        EXPECT_EQ(it->inline_mcp_servers.front().name, "plugin:plugin-fixture:inline-review");

        auto synced = cc::tools::sync_native_mcp_servers({
            cc::tools::NativeMcpConfiguredServer{
                .name = "plugin:plugin-fixture:review-context",
                .command = "node",
                .args = {server_path.string()},
                .env = {{"SERVER_NAME", "plugin:plugin-fixture:review-context"}, {"TOOL_NAME", "review_lookup"}},
            },
        });
        ASSERT_TRUE(synced.has_value());
        auto restarted = cc::tools::restart_native_mcp_server("plugin:plugin-fixture:review-context");
        ASSERT_TRUE(restarted.has_value()) << restarted.error();
        ASSERT_EQ(restarted->status, "ready");

        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "plugin-fixture:reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->preloaded_skill_messages.size(), 1u);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("plugin-fixture:review-skill"), std::string::npos);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("Use the plugin review checklist"), std::string::npos);
        ASSERT_EQ(plan->agent_mcp_servers.size(), 2u);
        EXPECT_EQ(plan->agent_mcp_servers[0], "plugin:plugin-fixture:review-context");
        EXPECT_EQ(plan->agent_mcp_servers[1], "plugin:plugin-fixture:inline-review");
        ASSERT_EQ(plan->agent_mcp_tools.size(), 2u);
        EXPECT_EQ(plan->agent_mcp_tools[0].server_name, "plugin:plugin-fixture:review-context");
        EXPECT_EQ(plan->agent_mcp_tools[0].tool_name, "review_lookup");
        EXPECT_EQ(plan->agent_mcp_tools[1].server_name, "plugin:plugin-fixture:inline-review");
        EXPECT_EQ(plan->agent_mcp_tools[1].tool_name, "inline_lookup");
        ASSERT_TRUE(plan->agent_mcp_context_message.has_value());
        EXPECT_NE(plan->agent_mcp_context_message->find("plugin:plugin-fixture:review-context/review_lookup"), std::string::npos);
        EXPECT_NE(plan->agent_mcp_context_message->find("plugin:plugin-fixture:inline-review/inline_lookup"), std::string::npos);
        EXPECT_TRUE(plan->frontmatter_hooks.contains("SubagentStart"));

        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        auto skill = registry.execute("skill", cc::core::ToolInput::from_json(R"({
          "name": "plugin-fixture:review-skill"
        })"));
        ASSERT_TRUE(skill.has_value());
        ASSERT_FALSE(skill->is_error);
        ASSERT_FALSE(skill->content.empty());
        EXPECT_NE(skill->content.front().text.find("Use the plugin review checklist"), std::string::npos);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsBackgroundAndIsolationDefinitionFeatures) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_background_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "async.md");
        agent << R"MD(---
name: async-reviewer
description: Reviews in background native modes
background: true
isolation: worktree
skills: [review]
---
Review asynchronously.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Async review",
          "prompt": "Review this change",
          "subagent_type": "async-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("features not yet supported"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("isolation"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolExecutesDefinitionHooksForBackgroundAgents) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_hooks_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto marker = root / "hook-marker.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "hooked.md");
        agent << R"MD(---
name: hooked-reviewer
description: Reviews with hooks
hooks:
  SubagentStart:
    - command: "printf start-$CLAUDE_HOOK_AGENT_ID > )MD" << marker.string() << R"MD(; echo hook-started"
---
Review with hooks.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentTool tool;

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Hooked review",
          "prompt": "Review this change",
          "subagent_type": "hooked-reviewer",
          "run_in_background": true,
          "name": "hooked-agent"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Queued background agent hooked-agent"), std::string::npos);
        ASSERT_TRUE(fs::exists(marker));
        std::ifstream marker_in(marker);
        std::string marker_text;
        std::getline(marker_in, marker_text);
        EXPECT_EQ(marker_text, "start-hooked-agent");

        auto record = cc::tools::agent_runtime::native_agent_store().get("hooked-agent");
        ASSERT_TRUE(record.has_value());
        ASSERT_GE(record->transcript.size(), 1u);
        EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
            return entry.find("hook-started") != std::string::npos;
        }));
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolExecutesSubagentStopHookWhenBackgroundAgentIsCancelled) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_stop_hook_cancel_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto marker = root / "stop-hook-marker.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "hooked-stop.md");
        agent << R"MD(---
name: hooked-stop-reviewer
description: Reviews with stop hooks
hooks:
  SubagentStop:
    - command: "printf stop-$CLAUDE_HOOK_AGENT_ID > )MD" << shell_quote_for_test(marker.string()) << R"MD(; echo hook-stopped"
---
Review with stop hooks.
)MD";
    }

    LocalSlowAnthropicStreamServer server(std::chrono::milliseconds(750));
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "stop-hook-cancel-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto started = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Run async and cancel with stop hook",
          "prompt": "Wait for cancellation and run stop hook",
          "subagent_type": "hooked-stop-reviewer",
          "run_in_background": true,
          "name": "hooked-stop-agent"
        })"));
        ASSERT_TRUE(started.has_value()) << started.error().format();
        ASSERT_FALSE(started->is_error);
        ASSERT_TRUE(server.wait_for_request());

        auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
          "task_id": "hooked-stop-agent"
        })"));
        ASSERT_TRUE(stopped.has_value());
        ASSERT_FALSE(stopped->is_error);

        bool observed_marker = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (fs::exists(marker)) {
                observed_marker = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(observed_marker);
        std::ifstream marker_in(marker);
        std::string marker_text;
        std::getline(marker_in, marker_text);
        EXPECT_EQ(marker_text, "stop-hooked-stop-agent");

        ASSERT_TRUE(wait_for_native_agent_status(
            "hooked-stop-agent",
            cc::tools::agent_runtime::NativeAgentStatus::Cancelled,
            std::chrono::seconds(3)));
        auto record = cc::tools::agent_runtime::native_agent_store().get("hooked-stop-agent");
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Cancelled);
        ASSERT_TRUE(record->error.has_value());
        EXPECT_NE(record->error->find("while waiting for model stream"), std::string::npos);
        EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
            return entry.find("hook SubagentStop: hook-stopped") != std::string::npos;
        }));
    }

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolRunsFrontmatterToolHooksAroundNativeToolUse) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_tool_hooks_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto pre_marker = root / "pre-hook-marker.txt";
    auto post_marker = root / "post-hook-marker.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "tool-hooks.md");
        agent << R"MD(---
name: tool-hook-reviewer
description: Reviews with tool hooks
hooks:
  PreToolUse:
    Bash:
      - command: "printf pre-$CLAUDE_HOOK_TOOL_NAME-$CLAUDE_HOOK_TOOL_USE_ID > )MD" << shell_quote_for_test(pre_marker.string()) << R"MD(; echo pre-ran"
  PostToolUse:
    Bash:
      - command: "printf post-$CLAUDE_HOOK_TOOL_NAME-$CLAUDE_HOOK_TOOL_USE_ID-$CLAUDE_HOOK_TOOL_OUTPUT_PREVIEW > )MD" << shell_quote_for_test(post_marker.string()) << R"MD(; printf '{\"hookSpecificOutput\":{\"hookEventName\":\"PostToolUse\",\"additionalContext\":\"post context visible\"}}'"
---
Review with tool hooks.
)MD";
    }

    LocalScriptedBashToolUseAnthropicServer server("printf tool-output", "tool hook complete");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "tool-hook-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Run tool hooks",
          "prompt": "Call Bash and finish",
          "subagent_type": "tool-hook-reviewer",
          "name": "tool-hook-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().format();
        ASSERT_FALSE(result->is_error);
        ASSERT_TRUE(server.wait_for_request_count(2));
    }

    ASSERT_TRUE(fs::exists(pre_marker));
    EXPECT_EQ(read_file(pre_marker), "pre-Bash-toolu_bash_fixture");
    ASSERT_TRUE(fs::exists(post_marker));
    EXPECT_EQ(read_file(post_marker), "post-Bash-toolu_bash_fixture-tool-output");
    auto second_body = server.request_body(1);
    ASSERT_TRUE(second_body.has_value());
    EXPECT_NE(second_body->find("post context visible"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("tool-hook-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook PreToolUse:Bash: pre-ran") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook PostToolUse:Bash:") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolPreToolFrontmatterHookCanDenyNativeToolUse) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_pre_tool_deny_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto executed_marker = root / "bash-executed.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "deny-tool.md");
        agent << R"MD(---
name: deny-tool-reviewer
description: Denies Bash through a pre hook
hooks:
  PreToolUse:
    Bash:
      - command: "printf '{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"deny\",\"permissionDecisionReason\":\"blocked by pre hook\"}}'"
---
Review with deny hooks.
)MD";
    }

    LocalScriptedBashToolUseAnthropicServer server(
        "printf executed > " + shell_quote_for_test(executed_marker.string()),
        "pre hook deny complete");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "pre-tool-deny-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Deny Bash",
          "prompt": "Call Bash and finish",
          "subagent_type": "deny-tool-reviewer",
          "name": "pre-deny-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().format();
        ASSERT_FALSE(result->is_error);
        ASSERT_TRUE(server.wait_for_request_count(2));
    }

    EXPECT_FALSE(fs::exists(executed_marker));
    auto second_body = server.request_body(1);
    ASSERT_TRUE(second_body.has_value());
    EXPECT_NE(second_body->find("Tool execution denied by PreToolUse hook"), std::string::npos);
    EXPECT_NE(second_body->find("blocked by pre hook"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("pre-deny-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook PreToolUse:Bash:") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolPreToolFrontmatterHookCanUpdateNativeToolInput) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_pre_tool_update_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto marker = root / "bash-updated-input.txt";
    auto hook_json = root / "updated-input-hook.json";
    {
        std::ofstream hook(hook_json);
        hook << R"({"hookSpecificOutput":{"hookEventName":"PreToolUse","updatedInput":{"command":")"
            << cc::tools::agent::json_escape_string("printf rewritten > " + shell_quote_for_test(marker.string()))
            << R"(","description":"rewritten by hook"},"additionalContext":"updated input context"}})";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "update-tool.md");
        agent << R"MD(---
name: update-tool-reviewer
description: Updates Bash input through a pre hook
hooks:
  PreToolUse:
    Bash:
      - command: "cat )MD" << shell_quote_for_test(hook_json.string()) << R"MD("
---
Review with update hooks.
)MD";
    }

    LocalScriptedBashToolUseAnthropicServer server(
        "printf original > " + shell_quote_for_test(marker.string()),
        "pre hook update complete");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "pre-tool-update-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Update Bash input",
          "prompt": "Call Bash and finish",
          "subagent_type": "update-tool-reviewer",
          "name": "pre-update-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().format();
        ASSERT_FALSE(result->is_error);
        ASSERT_TRUE(server.wait_for_request_count(2));
    }

    ASSERT_TRUE(fs::exists(marker));
    EXPECT_EQ(read_file(marker), "rewritten");
    auto second_body = server.request_body(1);
    ASSERT_TRUE(second_body.has_value());
    EXPECT_NE(second_body->find("updated input context"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("pre-update-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook PreToolUse:Bash updated input:") != std::string::npos &&
            entry.find("rewritten by hook") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolLivePermissionHookDeniesChildReadWriteEditAndBash) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_live_permission_deny_test";
    fs::remove_all(root);
    fs::create_directories(root);

    auto read_path = root / "read-denied-source.txt";
    {
        std::ofstream out(read_path);
        out << "read source";
    }
    auto write_path = root / "write-denied.txt";
    auto edit_path = root / "edit-denied.txt";
    {
        std::ofstream out(edit_path);
        out << "before edit";
    }
    auto bash_marker = root / "bash-denied.txt";

    struct PermissionCase {
        std::string tool_name;
        std::string input_json;
        std::string tool_use_id;
        std::string agent_name;
    };

    const auto cases = std::vector<PermissionCase>{
        PermissionCase{
            .tool_name = "Read",
            .input_json = std::format(
                R"({{"file_path":"{}"}})",
                cc::tools::agent::json_escape_string(read_path.string())),
            .tool_use_id = "toolu_live_deny_read",
            .agent_name = "live-deny-read-agent",
        },
        PermissionCase{
            .tool_name = "Write",
            .input_json = std::format(
                R"({{"file_path":"{}","content":"should not write"}})",
                cc::tools::agent::json_escape_string(write_path.string())),
            .tool_use_id = "toolu_live_deny_write",
            .agent_name = "live-deny-write-agent",
        },
        PermissionCase{
            .tool_name = "Edit",
            .input_json = std::format(
                R"({{"file_path":"{}","old_string":"before edit","new_string":"after edit"}})",
                cc::tools::agent::json_escape_string(edit_path.string())),
            .tool_use_id = "toolu_live_deny_edit",
            .agent_name = "live-deny-edit-agent",
        },
        PermissionCase{
            .tool_name = "Bash",
            .input_json = std::format(
                R"({{"command":"{}","description":"write denied marker"}})",
                cc::tools::agent::json_escape_string(
                    "printf denied > " + shell_quote_for_test(bash_marker.string()))),
            .tool_use_id = "toolu_live_deny_bash",
            .agent_name = "live-deny-bash-agent",
        },
    };

    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard model_guard("CLAUDE_MODEL", "live-permission-deny-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.tool_name);
        LocalScriptedToolUseAnthropicServer server(
            tc.tool_name,
            tc.input_json,
            tc.tool_use_id,
            "permission deny complete");
        ASSERT_TRUE(server.valid());
        EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());

        std::vector<cc::hooks::PermissionContext> calls;
        cc::hooks::ToolPermissionHook permission_hook;
        permission_hook.set_auto_approve(false);
        permission_hook.set_working_dir(root.string());
        permission_hook.set_ask_user_response_fn(
            [&calls, tool_name = tc.tool_name](const cc::hooks::PermissionContext& ctx) {
                calls.push_back(ctx);
                cc::hooks::PermissionResponse response;
                response.decision = cc::hooks::PermissionDecision::deny;
                response.message = "live deny " + tool_name;
                return response;
            });

        {
            CurrentPathGuard cwd(root);
            cc::core::ToolRegistry registry;
            cc::tools::register_runtime_tools(registry, cc::tools::RuntimeToolOptions{
                .permission_check = [&permission_hook](
                    std::string_view tool_name,
                    std::string_view input_json,
                    std::string_view tool_use_id
                ) {
                    return check_agent_tool_permission_from_hook(
                        permission_hook,
                        tool_name,
                        input_json,
                        tool_use_id);
                },
                .permission_hook_valid_for_background = false,
            });
            auto result = registry.execute("Agent", cc::core::ToolInput::from_json(std::format(R"({{
              "description": "Deny {}",
              "prompt": "Call {} and finish",
              "subagent_type": "general-purpose",
              "name": "{}"
            }})", tc.tool_name, tc.tool_name, tc.agent_name)));
            ASSERT_TRUE(result.has_value()) << result.error().message;
            ASSERT_FALSE(result->is_error);
            ASSERT_TRUE(server.wait_for_request_count(2));
        }

        ASSERT_EQ(calls.size(), 1u);
        EXPECT_EQ(calls.front().tool_name, tc.tool_name);
        EXPECT_EQ(calls.front().tool_use_id, tc.tool_use_id);
        EXPECT_FALSE(calls.front().args.empty());

        auto second_body = server.request_body(1);
        ASSERT_TRUE(second_body.has_value());
        EXPECT_NE(second_body->find("Tool execution denied by permission hook"), std::string::npos);
        EXPECT_NE(second_body->find("live deny " + tc.tool_name), std::string::npos);
    }

    EXPECT_FALSE(fs::exists(write_path));
    EXPECT_EQ(read_file(edit_path), "before edit");
    EXPECT_FALSE(fs::exists(bash_marker));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeRegistryEditToolEditsFileAndReturnsOutput) {
    auto root = fs::temp_directory_path() / "cc_repl_runtime_registry_edit_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto path = root / "edit.txt";
    {
        std::ofstream out(path);
        out << "before edit";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    // Read the file first (required by Edit tool)
    auto read_result = registry.execute("Read", cc::core::ToolInput::from_json(std::format(
        R"({{"file_path":"{}"}})",
        cc::tools::agent::json_escape_string(path.string()))));
    ASSERT_TRUE(read_result.has_value());
    auto result = registry.execute("Edit", cc::core::ToolInput::from_json(std::format(
        R"({{"file_path":"{}","old_string":"before edit","new_string":"after edit"}})",
        cc::tools::agent::json_escape_string(path.string()))));

    ASSERT_TRUE(result.has_value()) << result.error().format();
    ASSERT_FALSE(result->content.empty());
    ASSERT_FALSE(result->is_error) << result->content.front().text;
    EXPECT_NE(result->content.front().text.find("has been updated"), std::string::npos);
    EXPECT_EQ(read_file(path), "after edit");

    fs::remove_all(root);
}

TEST(Tools, AgentToolBackgroundAgentPreservesLivePermissionHook) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_background_live_permission_test";
    fs::remove_all(root);
    fs::create_directories(root);

    auto bash_marker = root / "background-bash-denied.txt";
    const auto command = "printf background-denied > " + shell_quote_for_test(bash_marker.string());
    LocalScriptedToolUseAnthropicServer server(
        "Bash",
        std::format(
            R"({{"command":"{}","description":"write background marker"}})",
            cc::tools::agent::json_escape_string(command)),
        "toolu_background_live_deny_bash",
        "background permission deny complete");
    ASSERT_TRUE(server.valid());

    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "background-live-permission-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    std::vector<cc::hooks::PermissionContext> calls;
    cc::hooks::ToolPermissionHook permission_hook;
    permission_hook.set_auto_approve(false);
    permission_hook.set_working_dir(root.string());
    permission_hook.set_ask_user_response_fn([&calls](const cc::hooks::PermissionContext& ctx) {
        calls.push_back(ctx);
        cc::hooks::PermissionResponse response;
        response.decision = cc::hooks::PermissionDecision::deny;
        response.message = "background live deny Bash";
        return response;
    });

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry, cc::tools::RuntimeToolOptions{
            .permission_check = [&permission_hook](
                std::string_view tool_name,
                std::string_view input_json,
                std::string_view tool_use_id
            ) {
                return check_agent_tool_permission_from_hook(
                    permission_hook,
                    tool_name,
                    input_json,
                    tool_use_id);
            },
            .permission_hook_valid_for_background = true,
        });

        auto result = registry.execute("Agent", cc::core::ToolInput::from_json(R"({
          "description": "Deny background Bash",
          "prompt": "Call Bash and finish",
          "subagent_type": "general-purpose",
          "run_in_background": true,
          "name": "background-live-permission-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().message;
        ASSERT_FALSE(result->is_error);
        ASSERT_TRUE(server.wait_for_request_count(2));
        ASSERT_TRUE(wait_for_native_agent_status(
            "background-live-permission-agent",
            cc::tools::agent_runtime::NativeAgentStatus::Completed,
            std::chrono::seconds(3)));
    }

    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.front().tool_name, "Bash");
    EXPECT_EQ(calls.front().tool_use_id, "toolu_background_live_deny_bash");
    EXPECT_FALSE(calls.front().args.empty());
    EXPECT_FALSE(fs::exists(bash_marker));

    auto second_body = server.request_body(1);
    ASSERT_TRUE(second_body.has_value());
    EXPECT_NE(second_body->find("Tool execution denied by permission hook"), std::string::npos);
    EXPECT_NE(second_body->find("background live deny Bash"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("background-live-permission-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Completed);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolLivePermissionHookCanAllowAndUpdateChildToolInputs) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_live_permission_update_test";
    fs::remove_all(root);
    fs::create_directories(root);

    auto read_original = root / "read-original.txt";
    auto read_updated = root / "read-updated.txt";
    {
        std::ofstream out(read_original);
        out << "original read content";
    }
    {
        std::ofstream out(read_updated);
        out << "updated read content";
    }

    auto write_original = root / "write-original.txt";
    auto write_updated = root / "write-updated.txt";
    auto edit_original = root / "edit-original.txt";
    auto edit_updated = root / "edit-updated.txt";
    {
        std::ofstream out(edit_original);
        out << "original edit before";
    }
    {
        std::ofstream out(edit_updated);
        out << "before edit";
    }
    auto bash_original = root / "bash-original.txt";
    auto bash_updated = root / "bash-updated.txt";

    struct PermissionCase {
        std::string tool_name;
        std::string input_json;
        std::string updated_input_json;
        std::string tool_use_id;
        std::string agent_name;
    };

    const auto cases = std::vector<PermissionCase>{
        PermissionCase{
            .tool_name = "Read",
            .input_json = std::format(
                R"({{"file_path":"{}"}})",
                cc::tools::agent::json_escape_string(read_original.string())),
            .updated_input_json = std::format(
                R"({{"file_path":"{}"}})",
                cc::tools::agent::json_escape_string(read_updated.string())),
            .tool_use_id = "toolu_live_update_read",
            .agent_name = "live-update-read-agent",
        },
        PermissionCase{
            .tool_name = "Write",
            .input_json = std::format(
                R"({{"file_path":"{}","content":"original write content"}})",
                cc::tools::agent::json_escape_string(write_original.string())),
            .updated_input_json = std::format(
                R"({{"file_path":"{}","content":"updated write content"}})",
                cc::tools::agent::json_escape_string(write_updated.string())),
            .tool_use_id = "toolu_live_update_write",
            .agent_name = "live-update-write-agent",
        },
        PermissionCase{
            .tool_name = "Edit",
            .input_json = std::format(
                R"({{"file_path":"{}","old_string":"original edit before","new_string":"original edit after"}})",
                cc::tools::agent::json_escape_string(edit_original.string())),
            .updated_input_json = std::format(
                R"({{"file_path":"{}","old_string":"before edit","new_string":"after edit"}})",
                cc::tools::agent::json_escape_string(edit_updated.string())),
            .tool_use_id = "toolu_live_update_edit",
            .agent_name = "live-update-edit-agent",
        },
        PermissionCase{
            .tool_name = "Bash",
            .input_json = std::format(
                R"({{"command":"{}","description":"write original marker"}})",
                cc::tools::agent::json_escape_string(
                    "printf original > " + shell_quote_for_test(bash_original.string()))),
            .updated_input_json = std::format(
                R"({{"command":"{}","description":"write updated marker"}})",
                cc::tools::agent::json_escape_string(
                    "printf updated > " + shell_quote_for_test(bash_updated.string()))),
            .tool_use_id = "toolu_live_update_bash",
            .agent_name = "live-update-bash-agent",
        },
    };

    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard model_guard("CLAUDE_MODEL", "live-permission-update-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.tool_name);
        LocalScriptedToolUseAnthropicServer server(
            tc.tool_name,
            tc.input_json,
            tc.tool_use_id,
            "permission update complete");
        ASSERT_TRUE(server.valid());
        EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());

        std::vector<cc::hooks::PermissionContext> calls;
        cc::hooks::ToolPermissionHook permission_hook;
        permission_hook.set_auto_approve(false);
        permission_hook.set_working_dir(root.string());
        permission_hook.set_ask_user_response_fn(
            [&calls, updated_input_json = tc.updated_input_json](const cc::hooks::PermissionContext& ctx) {
                calls.push_back(ctx);
                cc::hooks::PermissionResponse response;
                response.decision = cc::hooks::PermissionDecision::allow;
                response.updated_input_json = updated_input_json;
                return response;
            });

        {
            CurrentPathGuard cwd(root);
            cc::core::ToolRegistry registry;
            cc::tools::register_runtime_tools(registry, cc::tools::RuntimeToolOptions{
                .permission_check = [&permission_hook](
                    std::string_view tool_name,
                    std::string_view input_json,
                    std::string_view tool_use_id
                ) {
                    return check_agent_tool_permission_from_hook(
                        permission_hook,
                        tool_name,
                        input_json,
                        tool_use_id);
                },
                .permission_hook_valid_for_background = false,
            });
            auto result = registry.execute("Agent", cc::core::ToolInput::from_json(std::format(R"({{
              "description": "Update {}",
              "prompt": "Call {} and finish",
              "subagent_type": "general-purpose",
              "name": "{}"
            }})", tc.tool_name, tc.tool_name, tc.agent_name)));
            ASSERT_TRUE(result.has_value()) << result.error().message;
            ASSERT_FALSE(result->is_error);
            ASSERT_TRUE(server.wait_for_request_count(2));
        }

        ASSERT_EQ(calls.size(), 1u);
        EXPECT_EQ(calls.front().tool_name, tc.tool_name);
        EXPECT_EQ(calls.front().tool_use_id, tc.tool_use_id);
        EXPECT_FALSE(calls.front().args.empty());

        auto record = cc::tools::agent_runtime::native_agent_store().get(tc.agent_name);
        ASSERT_TRUE(record.has_value());
        EXPECT_TRUE(std::ranges::any_of(record->transcript, [&](const auto& entry) {
            return entry.find("permission hook " + tc.tool_name + " updated input:") != std::string::npos;
        }));

        if (tc.tool_name == "Read") {
            auto second_body = server.request_body(1);
            ASSERT_TRUE(second_body.has_value());
            EXPECT_NE(second_body->find("updated read content"), std::string::npos);
            EXPECT_EQ(second_body->find("original read content"), std::string::npos);
        }
    }

    EXPECT_FALSE(fs::exists(write_original));
    ASSERT_TRUE(fs::exists(write_updated));
    EXPECT_EQ(read_file(write_updated), "updated write content");
    EXPECT_EQ(read_file(edit_original), "original edit before");
    EXPECT_EQ(read_file(edit_updated), "after edit");
    EXPECT_FALSE(fs::exists(bash_original));
    ASSERT_TRUE(fs::exists(bash_updated));
    EXPECT_EQ(read_file(bash_updated), "updated");

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolPreToolHookCanPreventContinuationAfterToolExecution) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_pre_tool_stop_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto pre_marker = root / "pre-stop-hook-marker.txt";
    auto bash_marker = root / "pre-stop-bash-executed.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "stop-after-pre-tool.md");
        agent << R"MD(---
name: stop-after-pre-tool-reviewer
description: Stops continuation after pre-hooked Bash
hooks:
  PreToolUse:
    Bash:
      - command: "printf pre-stop-$CLAUDE_HOOK_TOOL_NAME-$CLAUDE_HOOK_TOOL_USE_ID > )MD" << shell_quote_for_test(pre_marker.string()) << R"MD(; printf '{\"continue\":false,\"stopReason\":\"stop after pre hook\",\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":\"pre stop context\"}}'"
---
Review with pre stop hooks.
)MD";
    }

    LocalScriptedBashToolUseAnthropicServer server(
        "printf tool-output > " + shell_quote_for_test(bash_marker.string()),
        "should not be requested");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "pre-tool-stop-hook-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Stop after pre-hooked Bash",
          "prompt": "Call Bash and finish",
          "subagent_type": "stop-after-pre-tool-reviewer",
          "name": "pre-stop-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().format();
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("stop after pre hook"), std::string::npos);
        ASSERT_TRUE(server.wait_for_request_count(1));
        EXPECT_FALSE(server.wait_for_request_count(2, std::chrono::milliseconds(250)));
        EXPECT_EQ(server.request_count(), 1u);
    }

    ASSERT_TRUE(fs::exists(pre_marker));
    EXPECT_EQ(read_file(pre_marker), "pre-stop-Bash-toolu_bash_fixture");
    ASSERT_TRUE(fs::exists(bash_marker));
    EXPECT_EQ(read_file(bash_marker), "tool-output");

    auto record = cc::tools::agent_runtime::native_agent_store().get("pre-stop-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Completed);
    ASSERT_TRUE(record->output.has_value());
    EXPECT_NE(record->output->find("stop after pre hook"), std::string::npos);
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook stopped continuation: stop after pre hook") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolRunsPostToolUseFailureHookForFailedNativeToolUse) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_post_tool_failure_hook_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto failure_marker = root / "failure-hook-marker.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "failure-hook.md");
        agent << R"MD(---
name: failure-hook-reviewer
description: Runs failure hooks
hooks:
  PostToolUseFailure:
    Bash:
      - command: "printf failure-$CLAUDE_HOOK_TOOL_NAME-$CLAUDE_HOOK_TOOL_USE_ID > )MD" << shell_quote_for_test(failure_marker.string()) << R"MD(; printf '{\"hookSpecificOutput\":{\"hookEventName\":\"PostToolUseFailure\",\"additionalContext\":\"failure context visible\"}}'"
---
Review with failure hooks.
)MD";
    }

    LocalScriptedBashToolUseAnthropicServer server("printf failing; exit 7", "failure hook complete");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "post-tool-failure-hook-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Fail Bash",
          "prompt": "Call Bash and finish",
          "subagent_type": "failure-hook-reviewer",
          "name": "post-failure-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().format();
        ASSERT_FALSE(result->is_error);
        ASSERT_TRUE(server.wait_for_request_count(2));
    }

    ASSERT_TRUE(fs::exists(failure_marker));
    EXPECT_EQ(read_file(failure_marker), "failure-Bash-toolu_bash_fixture");
    auto second_body = server.request_body(1);
    ASSERT_TRUE(second_body.has_value());
    EXPECT_NE(second_body->find("Exit code: 7"), std::string::npos);
    EXPECT_NE(second_body->find("failure context visible"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("post-failure-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook PostToolUseFailure:Bash:") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolPostToolHookCanPreventContinuation) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_post_tool_stop_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto post_marker = root / "post-stop-hook-marker.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "stop-after-tool.md");
        agent << R"MD(---
name: stop-after-tool-reviewer
description: Stops continuation after Bash
hooks:
  PostToolUse:
    Bash:
      - command: "printf post-stop-$CLAUDE_HOOK_TOOL_NAME-$CLAUDE_HOOK_TOOL_USE_ID > )MD" << shell_quote_for_test(post_marker.string()) << R"MD(; printf '{\"continue\":false,\"stopReason\":\"stop after post hook\",\"hookSpecificOutput\":{\"hookEventName\":\"PostToolUse\",\"additionalContext\":\"post stop context\"}}'"
---
Review with stop hooks.
)MD";
    }

    LocalScriptedBashToolUseAnthropicServer server("printf tool-output", "should not be requested");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "post-tool-stop-hook-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Stop after Bash",
          "prompt": "Call Bash and finish",
          "subagent_type": "stop-after-tool-reviewer",
          "name": "post-stop-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().format();
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("stop after post hook"), std::string::npos);
        ASSERT_TRUE(server.wait_for_request_count(1));
        EXPECT_FALSE(server.wait_for_request_count(2, std::chrono::milliseconds(250)));
        EXPECT_EQ(server.request_count(), 1u);
    }

    ASSERT_TRUE(fs::exists(post_marker));
    EXPECT_EQ(read_file(post_marker), "post-stop-Bash-toolu_bash_fixture");

    auto record = cc::tools::agent_runtime::native_agent_store().get("post-stop-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Completed);
    ASSERT_TRUE(record->output.has_value());
    EXPECT_NE(record->output->find("stop after post hook"), std::string::npos);
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook stopped continuation: stop after post hook") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolPostToolHookCanUpdateMcpToolOutput) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_post_tool_mcp_update_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto mcp_server_path = root / "server.js";
    const auto hook_json = root / "updated-mcp-output-hook.json";
    {
        std::ofstream server(mcp_server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'agent-mcp-update-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'echo', description: 'Echo value', inputSchema: { type: 'object' } }]
      }
    });
    return;
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{ type: 'text', text: 'echo:' + request.params.arguments.value }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream hook(hook_json);
        hook << R"({"hookSpecificOutput":{"hookEventName":"PostToolUse","updatedMCPToolOutput":"rewritten mcp output","additionalContext":"mcp updated context"}})";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "mcp-output-hook.md");
        agent << R"MD(---
name: mcp-output-hook-reviewer
description: Updates MCP tool output through a post hook
mcpServers: [echo_fixture]
hooks:
  PostToolUse:
    mcp:
      - command: "cat )MD" << shell_quote_for_test(hook_json.string()) << R"MD("
---
Review with MCP output hooks.
)MD";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "echo_fixture",
            .command = "node",
            .args = {mcp_server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());
    auto restarted = cc::tools::restart_native_mcp_server("echo_fixture");
    ASSERT_TRUE(restarted.has_value()) << restarted.error();
    ASSERT_EQ(restarted->status, "ready");

    LocalScriptedToolUseAnthropicServer server(
        "mcp",
        R"({"server_name":"echo_fixture","tool_name":"echo","arguments":{"value":"hello"}})",
        "toolu_mcp_fixture",
        "mcp hook complete");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "post-tool-mcp-update-hook-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        cc::tools::AgentTool tool({}, 0, &registry);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Update MCP output",
          "prompt": "Call MCP and finish",
          "subagent_type": "mcp-output-hook-reviewer",
          "name": "mcp-output-hook-agent"
        })"));
        ASSERT_TRUE(result.has_value()) << result.error().format();
        ASSERT_FALSE(result->is_error);
        ASSERT_TRUE(server.wait_for_request_count(2));
    }

    auto second_body = server.request_body(1);
    ASSERT_TRUE(second_body.has_value());
    EXPECT_NE(second_body->find("rewritten mcp output"), std::string::npos);
    EXPECT_NE(second_body->find("mcp updated context"), std::string::npos);
    EXPECT_EQ(second_body->find("echo:hello"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("mcp-output-hook-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& entry) {
        return entry.find("hook PostToolUse:mcp updated MCP output: rewritten mcp output") != std::string::npos;
    }));

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolExtractsSubagentStartHookAdditionalContext) {
    const auto context = cc::tools::agent::hook_additional_context_from_output(R"JSON({
      "hookSpecificOutput": {
        "hookEventName": "SubagentStart",
        "additionalContext": "Prefer inspecting generated bindings first."
      }
    })JSON");
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(*context, "Prefer inspecting generated bindings first.");

    EXPECT_FALSE(cc::tools::agent::hook_additional_context_from_output("plain hook log").has_value());
    EXPECT_FALSE(cc::tools::agent::hook_additional_context_from_output(R"JSON({
      "hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "additionalContext": "wrong event"
      }
    })JSON").has_value());

    std::vector<cc::services::api::Message> messages;
    cc::tools::agent::append_hook_additional_context_messages(
        messages,
        {*context, "Also check task notifications."});

    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages.front().role, "user");
    ASSERT_EQ(messages.front().content.size(), 1u);
    const auto& text = messages.front().content.front().text;
    EXPECT_NE(text.find("<hook_additional_context hook=\"SubagentStart\">"), std::string::npos);
    EXPECT_NE(text.find("Prefer inspecting generated bindings first."), std::string::npos);
    EXPECT_NE(text.find("Also check task notifications."), std::string::npos);
}

TEST(Tools, AgentToolRejectsMissingRequiredMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_required_mcp_missing_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "linear.md");
        agent << R"MD(---
name: linear-reviewer
description: Requires Linear MCP tools
requiredMcpServers: [linear]
---
Review Linear context.
)MD";
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Linear review",
          "prompt": "Review with Linear context",
          "subagent_type": "linear-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("requires MCP servers matching: linear"), std::string::npos);
        EXPECT_NE(result->content.front().text.find("MCP servers with tools: none"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("recursion depth"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolLoadsAgentSpecificMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_mcp_servers_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'agent-mcp-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'lookup', description: 'Lookup agent context', inputSchema: { type: 'object' } }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "mcp-agent.md");
        agent << R"MD(---
name: mcp-agent
description: Uses an agent-specific MCP server
tools: [Read]
mcpServers: [agent_fixture]
---
Use the agent-specific MCP server.
)MD";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "agent_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Use MCP context.";
        request.subagent_type = "mcp-agent";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->agent_mcp_servers.size(), 1u);
        EXPECT_EQ(plan->agent_mcp_servers.front(), "agent_fixture");
        ASSERT_EQ(plan->agent_mcp_tools.size(), 1u);
        EXPECT_EQ(plan->agent_mcp_tools.front().server_name, "agent_fixture");
        EXPECT_EQ(plan->agent_mcp_tools.front().tool_name, "lookup");
        ASSERT_TRUE(plan->agent_mcp_context_message.has_value());
        EXPECT_NE(plan->agent_mcp_context_message->find("agent_fixture/lookup"), std::string::npos);
        ASSERT_EQ(plan->allowed_tools.size(), 1u);
        EXPECT_EQ(plan->allowed_tools.front(), "Read");
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolLoadsInlineAgentMcpServersWithoutDroppingReferencedServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_inline_mcp_servers_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: process.env.SERVER_NAME || 'agent-inline-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{
          name: process.env.TOOL_NAME || 'lookup',
          description: ['tool', process.env.SERVER_NAME, process.env.INLINE_TOKEN].filter(Boolean).join(':'),
          inputSchema: { type: 'object' }
        }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "inline-mcp-agent.md");
        agent << std::format(R"MD(---
name: inline-mcp-agent
description: Uses referenced and inline MCP servers
mcpServers:
  - existing_fixture
  - inline_fixture:
      type: stdio
      command: node
      args:
        - "{}"
      env:
        SERVER_NAME: inline_fixture
        TOOL_NAME: inline_lookup
        INLINE_TOKEN: secret-token
---
Use both MCP servers.
)MD", server_path.string());
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "existing_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {{"SERVER_NAME", "existing_fixture"}, {"TOOL_NAME", "existing_lookup"}},
        },
    });
    ASSERT_TRUE(synced.has_value());

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Use MCP context.";
        request.subagent_type = "inline-mcp-agent";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->agent_mcp_servers.size(), 2u);
        EXPECT_EQ(plan->agent_mcp_servers[0], "existing_fixture");
        EXPECT_EQ(plan->agent_mcp_servers[1], "inline_fixture");
        ASSERT_EQ(plan->agent_mcp_tools.size(), 2u);
        EXPECT_EQ(plan->agent_mcp_tools[0].server_name, "existing_fixture");
        EXPECT_EQ(plan->agent_mcp_tools[0].tool_name, "existing_lookup");
        EXPECT_EQ(plan->agent_mcp_tools[1].server_name, "inline_fixture");
        EXPECT_EQ(plan->agent_mcp_tools[1].tool_name, "inline_lookup");
        EXPECT_NE(plan->agent_mcp_tools[1].description.find("secret-token"), std::string::npos);
        ASSERT_TRUE(plan->agent_mcp_context_message.has_value());
        EXPECT_NE(plan->agent_mcp_context_message->find("existing_fixture/existing_lookup"), std::string::npos);
        EXPECT_NE(plan->agent_mcp_context_message->find("inline_fixture/inline_lookup"), std::string::npos);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolCleansInlineMcpServerConfiguration) {
    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "inline_restore_fixture",
            .command = "node",
            .args = {"old-server.js"},
            .env = {{"TOKEN", "old"}},
        },
    }).has_value());

    std::vector<cc::tools::agent_runtime::AgentInlineMcpServerConfig> inline_servers{
        cc::tools::agent_runtime::AgentInlineMcpServerConfig{
            .name = "inline_restore_fixture",
            .transport = "stdio",
            .command = "node",
            .args = {"new-server.js"},
            .env = {{"TOKEN", "new"}},
        },
        cc::tools::agent_runtime::AgentInlineMcpServerConfig{
            .name = "inline_remove_fixture",
            .transport = "stdio",
            .command = "node",
            .args = {"temporary-server.js"},
            .env = {},
        },
    };

    auto states = cc::tools::agent::prepare_agent_inline_mcp_servers(inline_servers);
    ASSERT_TRUE(states.has_value()) << states.error();
    ASSERT_EQ(states->size(), 2u);

    auto overwritten = cc::tools::native_mcp_configured_server("inline_restore_fixture");
    ASSERT_TRUE(overwritten.has_value());
    ASSERT_EQ(overwritten->args.size(), 1u);
    EXPECT_EQ(overwritten->args.front(), "new-server.js");
    EXPECT_EQ(overwritten->env.at("TOKEN"), "new");

    auto temporary = cc::tools::native_mcp_configured_server("inline_remove_fixture");
    ASSERT_TRUE(temporary.has_value());
    ASSERT_EQ(temporary->args.size(), 1u);
    EXPECT_EQ(temporary->args.front(), "temporary-server.js");

    {
        cc::tools::agent::AgentMcpCleanupGuard cleanup{
            .agent_id = "mcp-cleanup-agent",
            .inline_servers = *states,
        };
    }

    auto restored = cc::tools::native_mcp_configured_server("inline_restore_fixture");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->args.size(), 1u);
    EXPECT_EQ(restored->args.front(), "old-server.js");
    EXPECT_EQ(restored->env.at("TOKEN"), "old");
    EXPECT_FALSE(cc::tools::native_mcp_configured_server("inline_remove_fixture").has_value());

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
}

TEST(Tools, AgentToolCleansInlineMcpServersWhenPlanBuildFails) {
    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "inline_plan_failure_restore",
            .command = "node",
            .args = {"old-server.js"},
            .env = {{"TOKEN", "old"}},
        },
    }).has_value());

    auto root = fs::temp_directory_path() / "cc_repl_agent_inline_mcp_failure_cleanup_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: process.env.SERVER_NAME || 'inline-plan-failure-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{
          name: process.env.TOOL_NAME || 'lookup',
          description: ['tool', process.env.SERVER_NAME].filter(Boolean).join(':'),
          inputSchema: { type: 'object' }
        }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "inline-failure.md");
        agent << std::format(R"MD(---
name: inline-failure-agent
description: Fails after configuring inline MCP servers
requiredMcpServers: [definitely-missing-required-server]
mcpServers:
  - inline_plan_failure_restore:
      type: stdio
      command: node
      args: ["{}"]
      env:
        TOKEN: new
        SERVER_NAME: inline_plan_failure_restore
        TOOL_NAME: restore_lookup
  - inline_plan_failure_remove:
      type: stdio
      command: node
      args: ["{}"]
      env:
        SERVER_NAME: inline_plan_failure_remove
        TOOL_NAME: remove_lookup
---
Review with inline MCP cleanup on failure.
)MD", server_path.string(), server_path.string());
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Trigger required MCP validation failure.";
        request.subagent_type = "inline-failure-agent";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, cc::tools::AgentConfig{});
        ASSERT_FALSE(plan.has_value());
        EXPECT_NE(plan.error().find("requires MCP servers matching"), std::string::npos);
    }

    auto restored = cc::tools::native_mcp_configured_server("inline_plan_failure_restore");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->args.size(), 1u);
    EXPECT_EQ(restored->args.front(), "old-server.js");
    EXPECT_EQ(restored->env.at("TOKEN"), "old");
    EXPECT_FALSE(cc::tools::native_mcp_configured_server("inline_plan_failure_remove").has_value());

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsReadyRequiredMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_required_mcp_ready_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'linear-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'lookup', description: 'Lookup issue', inputSchema: { type: 'object' } }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "linear.md");
        agent << R"MD(---
name: linear-reviewer
description: Requires Linear MCP tools
requiredMcpServers: [linear]
---
Review Linear context.
)MD";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "linear_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());
    auto restarted = cc::tools::restart_native_mcp_server("linear_fixture");
    ASSERT_TRUE(restarted.has_value()) << restarted.error();
    ASSERT_EQ(restarted->status, "ready");
    ASSERT_EQ(restarted->tools.size(), 1u);

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Linear review",
          "prompt": "Review with Linear context",
          "subagent_type": "linear-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("requires MCP servers"), std::string::npos);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsBackgroundNativeParameters) {
    cc::tools::AgentTool tool;

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async",
      "prompt": "Run in the background",
      "name": "reviewer-one",
      "mode": "default",
      "run_in_background": true
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Queued background agent reviewer-one"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("reviewer-one");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->agent_type, "general-purpose");
    EXPECT_FALSE(record->isolation.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
}

TEST(Tools, AgentToolResumeExistingBackgroundPreservesNativeHistoryAndPendingQueue) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_resume_existing_preserve_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "resume-existing",
        .agent_type = "general-purpose",
        .description = "Existing background agent",
        .cwd = root.string(),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .sidechain_entries = {
            R"({"type":"user","uuid":"resume-existing-0","parentUuid":null,"isSidechain":true,"agentId":"resume-existing","message":{"role":"user","content":[{"type":"text","text":"original context"}]}})",
        },
        .pending_messages = {"[Message from team-lead priority=normal]\nContinue the old job"},
        .transcript = {"user: original context", "assistant: partial result"},
        .progress = 0.5,
    });

    cc::tools::AgentTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "agent_id": "resume-existing",
      "resume_existing": true,
      "description": "Existing background agent",
      "prompt": "Resume this existing background agent.",
      "subagent_type": "general-purpose",
      "run_in_background": true
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Queued background agent resume-existing"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("resume-existing");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    ASSERT_GE(record->transcript.size(), 3u);
    EXPECT_EQ(record->transcript[0], "user: original context");
    EXPECT_EQ(record->transcript[1], "assistant: partial result");
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("Resume this existing background agent") != std::string::npos;
    }));
    ASSERT_EQ(record->pending_messages.size(), 1u);
    EXPECT_NE(record->pending_messages.front().find("Continue the old job"), std::string::npos);
    ASSERT_EQ(record->sidechain_entries.size(), 1u);
    EXPECT_NE(record->sidechain_entries.front().find("original context"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolRejectsRemoteIsolationForNonAntUser) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_remote_isolation_reject_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentUnsetGuard user_type_guard("USER_TYPE");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    {
        CurrentPathGuard cwd(root);
        auto result = registry.execute("Agent", cc::core::ToolInput::from_json(R"({
          "description": "Remote review",
          "prompt": "Review this remotely",
          "name": "remote-rejected-agent",
          "isolation": "remote"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Unsupported isolation mode 'remote'"), std::string::npos);
        EXPECT_NE(result->content.front().text.find("Valid options for this environment: worktree"), std::string::npos);
    }

    EXPECT_FALSE(cc::tools::agent_runtime::native_agent_store().get("remote-rejected-agent").has_value());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolLaunchesRemoteIsolationThroughRemoteTrigger) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_remote_isolation_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard user_type_guard("USER_TYPE", "ant");
    EnvironmentGuard trigger_guard("CC_REPL_REMOTE_TRIGGER_COMMAND", "printf remote-started");
    EnvironmentUnsetGuard target_guard("CC_REPL_REMOTE_AGENT_TARGET");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    {
        CurrentPathGuard cwd(root);
        auto result = registry.execute("Agent", cc::core::ToolInput::from_json(R"({
          "description": "Remote review",
          "prompt": "Review this remotely",
          "name": "remote-agent",
          "isolation": "remote"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Remote agent launched via configured remote trigger"), std::string::npos);
        EXPECT_NE(result->content.front().text.find("taskId: remote-agent"), std::string::npos);
    }

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->background);
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Running);
    ASSERT_TRUE(record->isolation.has_value());
    EXPECT_EQ(*record->isolation, "remote");
    ASSERT_TRUE(record->output_file_path.has_value());
    EXPECT_TRUE(fs::exists(*record->output_file_path));
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("remote trigger delivered: remote-started") != std::string::npos;
    }));

    auto listed = registry.execute("task_list", cc::core::ToolInput::from_json("{}"));
    ASSERT_TRUE(listed.has_value());
    ASSERT_FALSE(listed->is_error);
    ASSERT_FALSE(listed->content.empty());
    EXPECT_NE(listed->content.front().text.find("remote-agent"), std::string::npos);
    EXPECT_NE(listed->content.front().text.find("remote"), std::string::npos);

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-agent"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("remote trigger delivered: remote-started"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, TeleportUtilsListSessionsSendEventAndUpdateTitle) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());
    EnvironmentGuard api_base_guard("CC_REPL_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard token_guard("CC_REPL_REMOTE_OAUTH_TOKEN", "test-token");
    EnvironmentGuard org_guard("CC_REPL_REMOTE_ORG_UUID", "test-org");

    auto sessions = cc::utils::teleport::fetch_code_sessions_from_sessions_api();
    ASSERT_TRUE(sessions.has_value()) << sessions.error();
    ASSERT_EQ(sessions->size(), 1u);
    EXPECT_EQ(sessions->front().id, "remote-session-http");
    EXPECT_EQ(sessions->front().title, "Remote HTTP session");
    ASSERT_TRUE(sessions->front().repo.has_value());
    EXPECT_EQ(sessions->front().repo->owner_login, "acme");
    EXPECT_EQ(sessions->front().repo->name, "widgets");
    ASSERT_TRUE(sessions->front().repo->default_branch.has_value());
    EXPECT_EQ(*sessions->front().repo->default_branch, "main");

    cc::utils::teleport::RemoteMessageContent message = std::string("continue remotely");
    auto sent = cc::utils::teleport::send_event_to_remote_session(
        "remote-session-http",
        message,
        "event-uuid-test");
    ASSERT_TRUE(sent.has_value()) << sent.error();
    EXPECT_TRUE(*sent);
    EXPECT_EQ(server.event_posts.load(), 1);
    EXPECT_NE(server.last_event_body.find("event-uuid-test"), std::string::npos);
    EXPECT_NE(server.last_event_body.find("continue remotely"), std::string::npos);

    auto updated = cc::utils::teleport::update_session_title("remote-session-http", "Updated title");
    ASSERT_TRUE(updated.has_value()) << updated.error();
    EXPECT_TRUE(*updated);
    EXPECT_EQ(server.title_patches.load(), 1);
    EXPECT_NE(server.last_title_body.find("Updated title"), std::string::npos);
}

TEST(Tools, TeleportUtilsFetchCreateAndSelectEnvironments) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());
    EnvironmentGuard api_base_guard("CC_REPL_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard token_guard("CC_REPL_REMOTE_OAUTH_TOKEN", "test-token");
    EnvironmentGuard org_guard("CC_REPL_REMOTE_ORG_UUID", "test-org");
    EnvironmentUnsetGuard default_env_guard("CC_REPL_REMOTE_DEFAULT_ENVIRONMENT_ID");
    EnvironmentUnsetGuard default_env_guard_2("CLAUDE_CODE_REMOTE_DEFAULT_ENVIRONMENT_ID");
    EnvironmentUnsetGuard default_env_guard_3("ANTHROPIC_REMOTE_DEFAULT_ENVIRONMENT_ID");

    auto environments = cc::utils::teleport::fetch_environments();
    ASSERT_TRUE(environments.has_value()) << environments.error();
    ASSERT_EQ(environments->size(), 2u);
    EXPECT_EQ(environments->front().environment_id, "env-bridge");

    auto selection = cc::utils::teleport::get_environment_selection_info();
    ASSERT_TRUE(selection.has_value()) << selection.error();
    ASSERT_TRUE(selection->selected_environment.has_value());
    EXPECT_EQ(selection->selected_environment->environment_id, "env-cloud");

    auto created = cc::utils::teleport::create_default_cloud_environment("Created Cloud");
    ASSERT_TRUE(created.has_value()) << created.error();
    EXPECT_EQ(created->environment_id, "env-created");
    EXPECT_EQ(server.environment_create_requests.load(), 1);
}

TEST(Tools, TeleportUtilsCreatesAndUploadsGitBundle) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());

    auto root = fs::temp_directory_path() / "cc_repl_git_bundle_upload_test";
    fs::remove_all(root);
    fs::create_directories(root);
    {
        std::ofstream file(root / "README.md");
        file << "seed\n";
    }
    auto run = [&](const std::string& command) {
        auto status = std::system(command.c_str());
        ASSERT_EQ(status, 0) << command;
    };
    run("git -C " + shell_quote_for_test(root.string()) + " init --template= >/dev/null 2>&1");
    run("git -C " + shell_quote_for_test(root.string()) + " add README.md >/dev/null 2>&1");
    run("git -C " + shell_quote_for_test(root.string()) +
        " -c user.email=test@example.com -c user.name='Test User' commit --no-verify -m seed >/dev/null 2>&1");

    auto result = cc::utils::teleport::create_and_upload_git_bundle(
        cc::utils::teleport::FilesApiConfig{
            .access_token = "test-token",
            .org_uuid = "test-org",
            .base_url = server.base_url(),
        },
        root.string(),
        [] { return false; });

    ASSERT_TRUE(result.has_value()) << result.error().error;
    EXPECT_EQ(result->file_id, "file-seed-bundle");
    EXPECT_GT(result->bundle_size_bytes, 0u);
    EXPECT_EQ(server.file_upload_requests.load(), 1);
    EXPECT_EQ(server.last_upload_filename, "_source_seed.bundle");
    EXPECT_EQ(server.last_upload_purpose, "user_data");
    EXPECT_GT(server.last_upload_file_size, 0u);

    fs::remove_all(root);
}

TEST(Tools, AgentToolPersistsRemoteSessionMetadataFromTriggerOutput) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());

    auto root = fs::temp_directory_path() / "cc_repl_agent_remote_metadata_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_base_guard("CC_REPL_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard token_guard("CC_REPL_REMOTE_OAUTH_TOKEN", "test-token");
    EnvironmentGuard org_guard("CC_REPL_REMOTE_ORG_UUID", "test-org");
    EnvironmentGuard user_type_guard("USER_TYPE", "ant");
    EnvironmentGuard auto_poll_guard("CC_REPL_REMOTE_AGENT_AUTO_POLL", "0");
    EnvironmentGuard trigger_guard(
        "CC_REPL_REMOTE_TRIGGER_COMMAND",
        R"(printf '{"session_id":"remote-session-1","sessionUrl":"https://remote.example/session/remote-session-1","taskId":"remote-task-1","remoteTaskType":"remote-agent","title":"Remote review session","isLongRunning":true}')");
    EnvironmentUnsetGuard target_guard("CC_REPL_REMOTE_AGENT_TARGET");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    {
        CurrentPathGuard cwd(root);
        auto result = registry.execute("Agent", cc::core::ToolInput::from_json(R"({
          "description": "Remote metadata review",
          "prompt": "Review this remotely and report back",
          "name": "remote-metadata-agent",
          "isolation": "remote"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("session_url: https://remote.example/session/remote-session-1"), std::string::npos);
        EXPECT_NE(result->content.front().text.find("remote_task_id: remote-task-1"), std::string::npos);
    }

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-metadata-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->remote_task_id.has_value());
    EXPECT_EQ(*record->remote_task_id, "remote-task-1");
    ASSERT_TRUE(record->remote_task_type.has_value());
    EXPECT_EQ(*record->remote_task_type, "remote-agent");
    ASSERT_TRUE(record->remote_session_id.has_value());
    EXPECT_EQ(*record->remote_session_id, "remote-session-1");
    ASSERT_TRUE(record->remote_session_url.has_value());
    EXPECT_EQ(*record->remote_session_url, "https://remote.example/session/remote-session-1");
    ASSERT_TRUE(record->remote_title.has_value());
    EXPECT_EQ(*record->remote_title, "Remote review session");
    ASSERT_TRUE(record->remote_command.has_value());
    EXPECT_EQ(*record->remote_command, "Review this remotely and report back");
    EXPECT_TRUE(record->remote_is_long_running);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored = cc::tools::agent_runtime::native_agent_store().get("remote-metadata-agent");
    ASSERT_TRUE(restored.has_value());
    ASSERT_TRUE(restored->remote_session_id.has_value());
    EXPECT_EQ(*restored->remote_session_id, "remote-session-1");

    auto output_by_remote_task = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-task-1"
    })"));
    ASSERT_TRUE(output_by_remote_task.has_value());
    ASSERT_FALSE(output_by_remote_task->is_error);
    ASSERT_FALSE(output_by_remote_task->content.empty());
    EXPECT_NE(output_by_remote_task->content.front().text.find("remote_session_id: remote-session-1"), std::string::npos);
    EXPECT_NE(output_by_remote_task->content.front().text.find("remote_task_id: remote-task-1"), std::string::npos);

    auto stopped_by_session = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-session-1"
    })"));
    ASSERT_TRUE(stopped_by_session.has_value());
    ASSERT_FALSE(stopped_by_session->is_error);
    ASSERT_FALSE(stopped_by_session->content.empty());
    EXPECT_NE(stopped_by_session->content.front().text.find("remote_session_id: remote-session-1"), std::string::npos);
    EXPECT_NE(stopped_by_session->content.front().text.find("remote_archived: true"), std::string::npos);
    EXPECT_EQ(server.archive_requests.load(), 1);

    auto stopped_record = cc::tools::agent_runtime::native_agent_store().get("remote-metadata-agent");
    ASSERT_TRUE(stopped_record.has_value());
    EXPECT_EQ(stopped_record->status, cc::tools::agent_runtime::NativeAgentStatus::Cancelled);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolStartsRemoteAutoPollerAndCompletionNotification) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());

    auto root = fs::temp_directory_path() / "cc_repl_agent_remote_auto_poll_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_base_guard("CC_REPL_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard token_guard("CC_REPL_REMOTE_OAUTH_TOKEN", "test-token");
    EnvironmentGuard org_guard("CC_REPL_REMOTE_ORG_UUID", "test-org");
    EnvironmentGuard user_type_guard("USER_TYPE", "ant");
    EnvironmentGuard poll_interval_guard("CC_REPL_REMOTE_AGENT_POLL_INTERVAL_MS", "10");
    EnvironmentUnsetGuard auto_poll_guard("CC_REPL_REMOTE_AGENT_AUTO_POLL");
    const auto trigger_json = R"({"session_id":"remote-session-http","sessionUrl":"https://remote.example/session/remote-session-http","taskId":"remote-auto-task","remoteTaskType":"remote-agent","title":"Remote auto poll session"})";
    EnvironmentGuard trigger_guard(
        "CC_REPL_REMOTE_TRIGGER_COMMAND",
        "printf " + shell_quote_for_test(trigger_json));
    EnvironmentUnsetGuard target_guard("CC_REPL_REMOTE_AGENT_TARGET");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    {
        CurrentPathGuard cwd(root);
        auto result = registry.execute("Agent", cc::core::ToolInput::from_json(R"({
          "description": "Remote auto poll",
          "prompt": "Run remotely and report back",
          "name": "remote-auto-agent",
          "isolation": "remote"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("remote_auto_poll: true"), std::string::npos);
    }

    ASSERT_TRUE(wait_for_native_agent_status(
        "remote-auto-agent",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::milliseconds(2'000)));

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-auto-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->remote_last_event_id.has_value());
    EXPECT_EQ(*record->remote_last_event_id, "event-42");
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("assistant: server says hello") != std::string::npos;
    }));
    ASSERT_TRUE(record->output.has_value());
    EXPECT_EQ(*record->output, "Remote session archived");

    auto notifications = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications.front().find("<task_id>remote-auto-agent</task_id>"), std::string::npos);
    EXPECT_NE(notifications.front().find("<status>completed</status>"), std::string::npos);
    EXPECT_NE(notifications.front().find("<session_id>remote-session-http</session_id>"), std::string::npos);
    EXPECT_GE(server.event_requests.load(), 1);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeRestoresRemoteAutoPollerFromPersistedRecord) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());

    auto root = fs::temp_directory_path() / "cc_repl_remote_restore_poll_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_base_guard("CC_REPL_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard token_guard("CC_REPL_REMOTE_OAUTH_TOKEN", "test-token");
    EnvironmentGuard org_guard("CC_REPL_REMOTE_ORG_UUID", "test-org");
    EnvironmentGuard poll_interval_guard("CC_REPL_REMOTE_AGENT_POLL_INTERVAL_MS", "10");
    EnvironmentUnsetGuard auto_poll_guard("CC_REPL_REMOTE_AGENT_AUTO_POLL");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "remote-restore-agent",
        .agent_type = "general-purpose",
        .description = "Remote restore poll agent",
        .isolation = std::string("remote"),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .remote_task_id = std::string("remote-restore-task"),
        .remote_task_type = std::string("remote-agent"),
        .remote_session_id = std::string("remote-session-http"),
        .remote_session_url = std::string("https://remote.example/session/remote-session-http"),
    });

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    ASSERT_TRUE(wait_for_native_agent_status(
        "remote-restore-agent",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::milliseconds(2'000)));

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-restore-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->remote_last_event_id.has_value());
    EXPECT_EQ(*record->remote_last_event_id, "event-42");
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("assistant: server says hello") != std::string::npos;
    }));

    auto notifications = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications.front().find("<task_id>remote-restore-agent</task_id>"), std::string::npos);
    EXPECT_NE(notifications.front().find("<status>completed</status>"), std::string::npos);
    EXPECT_GE(server.event_requests.load(), 1);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTaskUpdateAppliesRemotePollEventsAndStableIdleCompletion) {
    auto root = fs::temp_directory_path() / "cc_repl_remote_poll_update_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "remote-poll-agent",
        .agent_type = "general-purpose",
        .description = "Remote poll agent",
        .isolation = std::string("remote"),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .remote_task_id = std::string("remote-poll-task"),
        .remote_task_type = std::string("remote-agent"),
        .remote_session_id = std::string("remote-poll-session"),
        .remote_session_url = std::string("https://remote.example/session/remote-poll-session"),
    });

    auto update = registry.execute("task_update", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-poll-task",
      "session_status": "running",
      "last_event_id": "event-1",
      "events": [
        {
          "type": "assistant",
          "session_id": "remote-poll-session",
          "message": {
            "content": [
              {"type": "text", "text": "remote progress line"}
            ]
          }
        }
      ]
    })"));
    ASSERT_TRUE(update.has_value());
    ASSERT_FALSE(update->is_error);

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-poll-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->remote_last_event_id.has_value());
    EXPECT_EQ(*record->remote_last_event_id, "event-1");
    EXPECT_TRUE(record->remote_has_output);
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("assistant: remote progress line") != std::string::npos;
    }));

    for (int i = 0; i < 5; ++i) {
        auto idle = registry.execute("task_update", cc::core::ToolInput::from_json(R"({
          "task_id": "remote-poll-session",
          "session_status": "idle",
          "events": []
        })"));
        ASSERT_TRUE(idle.has_value());
        ASSERT_FALSE(idle->is_error);
    }

    auto completed = cc::tools::agent_runtime::native_agent_store().get("remote-poll-agent");
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, cc::tools::agent_runtime::NativeAgentStatus::Completed);

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-poll-task"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("remote progress line"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<task_notification>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<task_type>remote_agent</task_type>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<session_id>remote-poll-session</session_id>"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTaskUpdateMarksRemotePollResultFailure) {
    auto root = fs::temp_directory_path() / "cc_repl_remote_poll_failure_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "remote-failed-agent",
        .agent_type = "general-purpose",
        .description = "Remote failed agent",
        .isolation = std::string("remote"),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .remote_task_id = std::string("remote-failed-task"),
        .remote_task_type = std::string("remote-agent"),
        .remote_session_id = std::string("remote-failed-session"),
    });

    auto failed = registry.execute("task_update", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-failed-session",
      "events": [
        {
          "type": "result",
          "session_id": "remote-failed-session",
          "subtype": "error",
          "error": "remote session returned an error"
        }
      ]
    })"));
    ASSERT_TRUE(failed.has_value());
    ASSERT_FALSE(failed->is_error);

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-failed-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Failed);
    ASSERT_TRUE(record->error.has_value());
    EXPECT_EQ(*record->error, "remote session returned an error");

    auto notifications = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications.front().find("<status>failed</status>"), std::string::npos);
    EXPECT_NE(notifications.front().find("<session_id>remote-failed-session</session_id>"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTaskUpdatePollsRemoteSessionEventsOverHttp) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());

    auto root = fs::temp_directory_path() / "cc_repl_remote_poll_http_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_base_guard("CC_REPL_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard token_guard("CC_REPL_REMOTE_OAUTH_TOKEN", "test-token");
    EnvironmentGuard org_guard("CC_REPL_REMOTE_ORG_UUID", "test-org");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "remote-http-agent",
        .agent_type = "general-purpose",
        .description = "Remote HTTP poll agent",
        .isolation = std::string("remote"),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .remote_task_id = std::string("remote-http-task"),
        .remote_task_type = std::string("remote-agent"),
        .remote_session_id = std::string("remote-session-http"),
    });

    auto polled = registry.execute("task_update", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-http-task",
      "action": "poll_remote"
    })"));
    ASSERT_TRUE(polled.has_value());
    ASSERT_FALSE(polled->is_error);
    ASSERT_FALSE(polled->content.empty());
    EXPECT_NE(polled->content.front().text.find("Polled remote task remote-http-agent [completed]"), std::string::npos);
    EXPECT_NE(polled->content.front().text.find("events_appended: 1"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-http-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Completed);
    ASSERT_TRUE(record->remote_last_event_id.has_value());
    EXPECT_EQ(*record->remote_last_event_id, "event-42");
    ASSERT_TRUE(record->output.has_value());
    EXPECT_EQ(*record->output, "Remote session archived");
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("assistant: server says hello") != std::string::npos;
    }));
    EXPECT_EQ(server.event_requests.load(), 1);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTaskStopArchivesRemoteSessionOverHttp) {
    LocalRemoteSessionApiServer server;
    ASSERT_TRUE(server.valid());

    auto root = fs::temp_directory_path() / "cc_repl_remote_archive_http_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_base_guard("CC_REPL_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard token_guard("CC_REPL_REMOTE_OAUTH_TOKEN", "test-token");
    EnvironmentGuard org_guard("CC_REPL_REMOTE_ORG_UUID", "test-org");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "remote-archive-agent",
        .agent_type = "general-purpose",
        .description = "Remote archive agent",
        .isolation = std::string("remote"),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .remote_task_id = std::string("remote-archive-task"),
        .remote_task_type = std::string("remote-agent"),
        .remote_session_id = std::string("remote-session-http"),
    });

    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
      "task_id": "remote-archive-task"
    })"));
    ASSERT_TRUE(stopped.has_value());
    ASSERT_FALSE(stopped->is_error);
    ASSERT_FALSE(stopped->content.empty());
    EXPECT_NE(stopped->content.front().text.find("remote_archived: true"), std::string::npos);
    EXPECT_EQ(server.archive_requests.load(), 1);

    auto record = cc::tools::agent_runtime::native_agent_store().get("remote-archive-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Cancelled);
    ASSERT_TRUE(record->error.has_value());
    EXPECT_EQ(*record->error, "stop requested");

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolSpawnsTeammateWithDeterministicAgentId) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_teammate_spawn_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard teammate_backend_guard("CC_REPL_TEAMMATE_BACKEND", "in-process");
    cc::utils::swarm_backends::BackendRegistry::reset();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto team = cc::tools::global_team_store().create("migration-team", "migration-team", {});
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));

    cc::tools::AgentTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Spawn reviewer",
      "prompt": "Review migration parity",
      "name": "reviewer-one",
      "team_name": "migration-team",
      "subagent_type": "general-purpose",
      "mode": "plan"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Spawned successfully"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("agent_id: reviewer-one@migration-team"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("backend: in-process"), std::string::npos);
    EXPECT_NE(
        result->content.front().text.find("task_id: in-process:reviewer-one@migration-team"),
        std::string::npos);
    EXPECT_NE(result->content.front().text.find("status: teammate_spawned"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("reviewer-one@migration-team");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->background);
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    ASSERT_TRUE(record->team_name.has_value());
    EXPECT_EQ(*record->team_name, "migration-team");
    ASSERT_TRUE(record->mode.has_value());
    EXPECT_EQ(*record->mode, "plan");
    ASSERT_TRUE(record->teammate_backend.has_value());
    EXPECT_EQ(*record->teammate_backend, "in-process");
    ASSERT_TRUE(record->teammate_task_id.has_value());
    EXPECT_EQ(*record->teammate_task_id, "in-process:reviewer-one@migration-team");
    EXPECT_FALSE(record->teammate_pane_id.has_value());
    ASSERT_TRUE(record->parent_session_id.has_value());
    EXPECT_EQ(*record->parent_session_id, "native-session");

    auto restored_team = cc::tools::global_team_store().get("migration-team");
    ASSERT_TRUE(restored_team.has_value()) << std::string(cc::tools::format_error(restored_team.error()));
    auto member = std::ranges::find_if((*restored_team)->members, [](const auto& candidate) {
        return candidate.agent_id == "reviewer-one@migration-team";
    });
    ASSERT_NE(member, (*restored_team)->members.end());
    EXPECT_EQ(member->role, cc::tools::MemberRole::Worker);
    EXPECT_EQ(member->status, cc::tools::MemberStatus::Working);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto listed = registry.execute("task_list", cc::core::ToolInput::from_json("{}"));
    ASSERT_TRUE(listed.has_value());
    ASSERT_FALSE(listed->is_error);
    ASSERT_FALSE(listed->content.empty());
    EXPECT_NE(listed->content.front().text.find("reviewer-one@migration-team [queued]"), std::string::npos);
    EXPECT_NE(listed->content.front().text.find("teammate_backend: in-process"), std::string::npos);
    EXPECT_NE(
        listed->content.front().text.find("teammate_task_id: in-process:reviewer-one@migration-team"),
        std::string::npos);

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::utils::swarm_backends::BackendRegistry::reset();
    fs::remove_all(root);
}

TEST(Tools, AgentToolSpawnsTeammateWithUniqueNameWhenTeamAlreadyHasMember) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_teammate_unique_spawn_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard teammate_backend_guard("CC_REPL_TEAMMATE_BACKEND", "in-process");
    cc::utils::swarm_backends::BackendRegistry::reset();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto team = cc::tools::global_team_store().create("migration-team", "migration-team", {
        cc::tools::TeamMember{
            .agent_id = "reviewer-one@migration-team",
            .role = cc::tools::MemberRole::Worker,
            .status = cc::tools::MemberStatus::Working,
        },
        cc::tools::TeamMember{
            .agent_id = "reviewer-one-2@migration-team",
            .role = cc::tools::MemberRole::Worker,
            .status = cc::tools::MemberStatus::Working,
        },
    });
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));

    cc::tools::AgentTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Spawn duplicate reviewer",
      "prompt": "Review migration parity again",
      "name": "reviewer-one",
      "team_name": "migration-team",
      "subagent_type": "general-purpose"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("agent_id: reviewer-one-3@migration-team"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("name: reviewer-one-3"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("reviewer-one-3@migration-team");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->name.has_value());
    EXPECT_EQ(*record->name, "reviewer-one-3");
    ASSERT_TRUE(record->teammate_task_id.has_value());
    EXPECT_EQ(*record->teammate_task_id, "in-process:reviewer-one-3@migration-team");

    auto restored_team = cc::tools::global_team_store().get("migration-team");
    ASSERT_TRUE(restored_team.has_value()) << std::string(cc::tools::format_error(restored_team.error()));
    auto member = std::ranges::find_if((*restored_team)->members, [](const auto& candidate) {
        return candidate.agent_id == "reviewer-one-3@migration-team";
    });
    ASSERT_NE(member, (*restored_team)->members.end());

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::utils::swarm_backends::BackendRegistry::reset();
    fs::remove_all(root);
}

TEST(Tools, AgentToolRejectsNestedTeammateSpawnFromTeamContext) {
    struct ClearDynamicTeamContext {
        ~ClearDynamicTeamContext() {
            cc::utils::clear_dynamic_team_context();
        }
    } clear_dynamic_team_context;

    cc::utils::set_dynamic_team_context(cc::utils::DynamicTeamContext{
        .agent_id = "worker-one@migration-team",
        .agent_name = "worker-one",
        .team_name = "migration-team",
        .agent_type = std::nullopt,
        .color = "blue",
        .plan_mode_required = false,
        .parent_session_id = "leader-session",
    });

    cc::tools::AgentTool tool;
    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Spawn nested teammate",
      "prompt": "Try to spawn another teammate",
      "name": "nested-worker",
      "subagent_type": "general-purpose"
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Teammates cannot spawn other teammates"), std::string::npos);
}

TEST(Tools, AgentToolRejectsBackgroundAgentFromInProcessTeammateContext) {
    auto ctx = cc::utils::create_teammate_context(
        "worker-one@migration-team",
        "worker-one",
        "migration-team",
        "leader-session",
        false,
        std::optional<std::string_view>{"blue"});

    auto result = cc::utils::run_with_teammate_context(ctx, [] {
        cc::tools::AgentTool tool;
        return tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Spawn async subagent",
          "prompt": "Try to spawn a background subagent",
          "subagent_type": "general-purpose",
          "run_in_background": true
        })"));
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("In-process teammates cannot spawn background agents"), std::string::npos);
}

TEST(Tools, SwarmBackendsInProcessExecutorTracksActiveTeammates) {
    auto root = fs::temp_directory_path() / "cc_repl_in_process_executor_mailbox_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard teammate_backend_guard("CC_REPL_TEAMMATE_BACKEND", "in-process");
    cc::utils::swarm_backends::BackendRegistry::reset();

    auto executor = cc::utils::swarm_backends::BackendRegistry::get_teammate_executor();
    ASSERT_TRUE(executor);
    EXPECT_EQ(executor->type(), cc::utils::swarm_backends::BackendType::InProcess);

    cc::utils::swarm_backends::TeammateSpawnConfig config{
        .name = "reviewer-one",
        .team_name = "migration-team",
        .color = std::nullopt,
        .plan_mode_required = false,
        .permission_mode = std::nullopt,
        .agent_type = std::nullopt,
        .prompt = "Review migration parity",
        .cwd = fs::current_path().string(),
        .model = std::nullopt,
        .system_prompt = std::nullopt,
        .system_prompt_mode = "default",
        .worktree_path = std::nullopt,
        .parent_session_id = "test-session",
        .permissions = {},
        .allow_permission_prompts = false,
    };
    auto spawned = executor->spawn(config);
    ASSERT_TRUE(spawned.success) << spawned.error.value_or("");
    EXPECT_EQ(spawned.agent_id, "reviewer-one@migration-team");
    ASSERT_TRUE(spawned.task_id.has_value());
    EXPECT_EQ(*spawned.task_id, "in-process:reviewer-one@migration-team");
    EXPECT_FALSE(spawned.pane_id.has_value());
    EXPECT_TRUE(executor->is_active("reviewer-one@migration-team"));

    executor->send_message(
        "reviewer-one@migration-team",
        cc::utils::swarm_backends::TeammateMessage{
            .text = "Please review the migration",
            .from = "team-lead",
            .color = std::optional<std::string>{"cyan"},
            .timestamp = std::nullopt,
            .summary = std::optional<std::string>{"review migration"},
        });
    auto inbox = cc::utils::read_inbox("reviewer-one", std::optional<std::string_view>{"migration-team"});
    ASSERT_TRUE(inbox.has_value()) << inbox.error();
    ASSERT_EQ(inbox->size(), 1u);
    EXPECT_EQ(inbox->front().from, "team-lead");
    EXPECT_EQ(inbox->front().text, "Please review the migration");
    ASSERT_TRUE(inbox->front().summary.has_value());
    EXPECT_EQ(*inbox->front().summary, "review migration");

    EXPECT_TRUE(executor->terminate("reviewer-one@migration-team", "done"));
    EXPECT_TRUE(executor->is_active("reviewer-one@migration-team"));

    inbox = cc::utils::read_inbox("reviewer-one", std::optional<std::string_view>{"migration-team"});
    ASSERT_TRUE(inbox.has_value()) << inbox.error();
    ASSERT_EQ(inbox->size(), 2u);
    EXPECT_NE(inbox->back().text.find(R"("type":"shutdown_request")"), std::string::npos);
    EXPECT_NE(inbox->back().text.find(R"("reason":"done")"), std::string::npos);

    EXPECT_TRUE(executor->kill("reviewer-one@migration-team"));
    EXPECT_FALSE(executor->is_active("reviewer-one@migration-team"));

    cc::utils::swarm_backends::BackendRegistry::reset();
    fs::remove_all(root);
}

TEST(Tools, SwarmBackendsPaneCommandPropagatesPermissionModeFlags) {
    EnvironmentGuard teammate_command_guard("CC_REPL_TEAMMATE_COMMAND", "/tmp/cc repl");

    cc::utils::swarm_backends::TeammateSpawnConfig config{
        .name = "reviewer-one",
        .team_name = "migration-team",
        .color = std::nullopt,
        .plan_mode_required = false,
        .permission_mode = std::optional<std::string>{"acceptEdits"},
        .agent_type = std::optional<std::string>{"verification"},
        .prompt = "Review migration parity",
        .cwd = "/tmp/cc repl worktree",
        .model = std::nullopt,
        .system_prompt = std::nullopt,
        .system_prompt_mode = "default",
        .worktree_path = std::nullopt,
        .parent_session_id = "test-session",
        .permissions = {},
        .allow_permission_prompts = false,
    };

    auto accept_edits = cc::utils::swarm_backends::detail::build_teammate_cli_command(config);
    EXPECT_NE(accept_edits.find("--permission-mode"), std::string::npos);
    EXPECT_NE(accept_edits.find("'acceptEdits'"), std::string::npos);
    EXPECT_NE(accept_edits.find("--agent-type"), std::string::npos);
    EXPECT_NE(accept_edits.find("'verification'"), std::string::npos);
    EXPECT_EQ(accept_edits.find("--dangerously-skip-permissions"), std::string::npos);

    config.permission_mode = "bypassPermissions";
    auto bypass = cc::utils::swarm_backends::detail::build_teammate_cli_command(config);
    EXPECT_NE(bypass.find("--dangerously-skip-permissions"), std::string::npos);
    EXPECT_EQ(bypass.find("--permission-mode"), std::string::npos);

    config.permission_mode = "auto";
    auto automatic = cc::utils::swarm_backends::detail::build_teammate_cli_command(config);
    EXPECT_NE(automatic.find("--permission-mode"), std::string::npos);
    EXPECT_NE(automatic.find("'auto'"), std::string::npos);
    EXPECT_EQ(automatic.find("--dangerously-skip-permissions"), std::string::npos);

    config.plan_mode_required = true;
    auto plan = cc::utils::swarm_backends::detail::build_teammate_cli_command(config);
    EXPECT_NE(plan.find("--plan-mode-required"), std::string::npos);
    EXPECT_EQ(plan.find("--permission-mode"), std::string::npos);
    EXPECT_EQ(plan.find("--dangerously-skip-permissions"), std::string::npos);
}

TEST(Tools, TeamHelpersResolveTeammateAgentTypeAndPlanMode) {
    struct ClearDynamicTeamContext {
        ~ClearDynamicTeamContext() {
            cc::utils::clear_dynamic_team_context();
        }
    } clear_dynamic_team_context;

    EnvironmentGuard agent_type_guard("CC_REPL_AGENT_TYPE", "verification");
    EnvironmentGuard plan_mode_guard("CC_REPL_PLAN_MODE_REQUIRED", "true");

    auto env_agent_type = cc::utils::get_agent_type();
    ASSERT_TRUE(env_agent_type.has_value());
    EXPECT_EQ(*env_agent_type, "verification");
    EXPECT_TRUE(cc::utils::is_plan_mode_required());

    cc::utils::set_dynamic_team_context(cc::utils::DynamicTeamContext{
        .agent_id = "reviewer-one@migration-team",
        .agent_name = "reviewer-one",
        .team_name = "migration-team",
        .agent_type = std::optional<std::string>{"Explore"},
        .color = "blue",
        .plan_mode_required = false,
        .parent_session_id = "leader-session",
    });

    auto dynamic_agent_type = cc::utils::get_agent_type();
    ASSERT_TRUE(dynamic_agent_type.has_value());
    EXPECT_EQ(*dynamic_agent_type, "Explore");
    EXPECT_FALSE(cc::utils::is_plan_mode_required());

    auto in_process_result = cc::utils::run_with_teammate_context(
        cc::utils::TeammateContext{
            .agent_id = "planner@migration-team",
            .agent_name = "planner",
            .team_name = "migration-team",
            .agent_type = std::optional<std::string>{"Plan"},
            .color = std::optional<std::string>{"green"},
            .plan_mode_required = true,
            .parent_session_id = "leader-session",
            .is_in_process = true,
        },
        [] {
            return std::pair{
                cc::utils::get_agent_type(),
                cc::utils::is_plan_mode_required(),
            };
        });
    ASSERT_TRUE(in_process_result.first.has_value());
    EXPECT_EQ(*in_process_result.first, "Plan");
    EXPECT_TRUE(in_process_result.second);
}

TEST(Tools, AgentRuntimeBuildsTeammateAppendSystemPromptFromAgentType) {
    struct ClearDynamicTeamContext {
        ~ClearDynamicTeamContext() {
            cc::utils::clear_dynamic_team_context();
        }
    } clear_dynamic_team_context;

    auto root = fs::temp_directory_path() / "cc_repl_teammate_agent_type_prompt_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "reviewer.md");
        agent << R"MD(---
name: reviewer
description: Reviews migration parity
---
You review C++ migration parity and report missing behavior.
)MD";
    }

    cc::utils::set_dynamic_team_context(cc::utils::DynamicTeamContext{
        .agent_id = "reviewer-one@migration-team",
        .agent_name = "reviewer-one",
        .team_name = "migration-team",
        .agent_type = std::optional<std::string>{"reviewer"},
        .color = "blue",
        .plan_mode_required = false,
        .parent_session_id = "leader-session",
    });

    auto prompt = cc::tools::agent_runtime::build_teammate_append_system_prompt(
        std::optional<std::string>{"existing append prompt"},
        root);
    ASSERT_TRUE(prompt.has_value());
    EXPECT_NE(prompt->find("existing append prompt"), std::string::npos);
    EXPECT_NE(prompt->find("Agent Teammate Communication"), std::string::npos);
    EXPECT_NE(prompt->find("# Custom Agent Instructions"), std::string::npos);
    EXPECT_NE(prompt->find("You review C++ migration parity"), std::string::npos);

    cc::utils::clear_dynamic_team_context();
    fs::remove_all(root);
}

TEST(Tools, RuntimeSendMessageWritesNativeTeammateMailbox) {
    auto root = fs::temp_directory_path() / "cc_repl_send_message_teammate_mailbox_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard teammate_backend_guard("CC_REPL_TEAMMATE_BACKEND", "in-process");
    cc::utils::swarm_backends::BackendRegistry::reset();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto team = cc::tools::global_team_store().create("migration-team", "migration-team", {});
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));

    cc::tools::AgentTool tool;
    auto spawned = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Spawn reviewer",
      "prompt": "Review migration parity",
      "name": "reviewer-one",
      "team_name": "migration-team",
      "subagent_type": "general-purpose"
    })"));
    ASSERT_TRUE(spawned.has_value());
    ASSERT_FALSE(spawned->is_error);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "reviewer-one",
      "team_name": "migration-team",
      "content": "Please review the parser migration",
      "summary": "review parser migration"
    })"));
    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("reviewer-one@migration-team"), std::string::npos);

    auto inbox = cc::utils::read_inbox("reviewer-one", std::optional<std::string_view>{"migration-team"});
    ASSERT_TRUE(inbox.has_value()) << inbox.error();
    ASSERT_EQ(inbox->size(), 1u);
    EXPECT_EQ(inbox->front().from, "team-lead");
    EXPECT_EQ(inbox->front().text, "Please review the parser migration");
    EXPECT_FALSE(inbox->front().read);
    ASSERT_TRUE(inbox->front().summary.has_value());
    EXPECT_EQ(*inbox->front().summary, "review parser migration");
    EXPECT_TRUE(fs::exists(root / "teams" / "migration-team" / "inboxes" / "reviewer-one.json"));

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::utils::swarm_backends::BackendRegistry::reset();
    fs::remove_all(root);
}

TEST(Tools, RuntimeSendMessageAcceptsTsSchemaAndBroadcastsToTeamMailbox) {
    auto root = fs::temp_directory_path() / "cc_repl_send_message_ts_broadcast_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto team = cc::tools::global_team_store().create(
        "broadcast-team-id",
        "Broadcast Team",
        {
            cc::tools::TeamMember{.agent_id = "team-lead@Broadcast Team"},
            cc::tools::TeamMember{.agent_id = "reviewer@Broadcast Team"},
            cc::tools::TeamMember{.agent_id = "planner@Broadcast Team"},
        });
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "*",
      "team_name": "Broadcast Team",
      "message": "Please sync on the migration audit",
      "summary": "migration audit sync"
    })"));
    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("Message broadcast to 2 teammate(s): reviewer, planner"), std::string::npos);

    auto reviewer_inbox = cc::utils::read_inbox("reviewer", std::optional<std::string_view>{"Broadcast Team"});
    ASSERT_TRUE(reviewer_inbox.has_value()) << reviewer_inbox.error();
    ASSERT_EQ(reviewer_inbox->size(), 1u);
    EXPECT_EQ(reviewer_inbox->front().from, "team-lead");
    EXPECT_EQ(reviewer_inbox->front().text, "Please sync on the migration audit");
    ASSERT_TRUE(reviewer_inbox->front().summary.has_value());
    EXPECT_EQ(*reviewer_inbox->front().summary, "migration audit sync");

    auto planner_inbox = cc::utils::read_inbox("planner", std::optional<std::string_view>{"Broadcast Team"});
    ASSERT_TRUE(planner_inbox.has_value()) << planner_inbox.error();
    ASSERT_EQ(planner_inbox->size(), 1u);
    EXPECT_EQ(planner_inbox->front().text, "Please sync on the migration audit");

    auto leader_inbox = cc::utils::read_inbox("team-lead", std::optional<std::string_view>{"Broadcast Team"});
    ASSERT_TRUE(leader_inbox.has_value()) << leader_inbox.error();
    EXPECT_TRUE(leader_inbox->empty());

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeSendMessageWritesStructuredTeamProtocolMessages) {
    auto root = fs::temp_directory_path() / "cc_repl_send_message_structured_protocol_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto team = cc::tools::global_team_store().create(
        "protocol-team-id",
        "Protocol Team",
        {
            cc::tools::TeamMember{.agent_id = "team-lead@Protocol Team"},
            cc::tools::TeamMember{.agent_id = "reviewer@Protocol Team"},
            cc::tools::TeamMember{.agent_id = "planner@Protocol Team"},
        });
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto shutdown_request = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "reviewer",
      "team_name": "Protocol Team",
      "message": {
        "type": "shutdown_request",
        "reason": "Stop after final review"
      }
    })"));
    ASSERT_TRUE(shutdown_request.has_value());
    ASSERT_FALSE(shutdown_request->is_error);
    EXPECT_NE(shutdown_request->content.front().text.find("request_id: shutdown-"), std::string::npos);

    auto reviewer_inbox = cc::utils::read_inbox("reviewer", std::optional<std::string_view>{"Protocol Team"});
    ASSERT_TRUE(reviewer_inbox.has_value()) << reviewer_inbox.error();
    ASSERT_EQ(reviewer_inbox->size(), 1u);
    auto shutdown_request_json = cc::utils::json::parse(reviewer_inbox->front().text);
    ASSERT_TRUE(shutdown_request_json.has_value());
    EXPECT_EQ(shutdown_request_json->root().get_string("type"), "shutdown_request");
    EXPECT_EQ(shutdown_request_json->root().get_string("from"), "team-lead");
    EXPECT_EQ(shutdown_request_json->root().get_string("reason"), "Stop after final review");
    EXPECT_NE(shutdown_request_json->root().get_string("requestId").find("shutdown-"), std::string::npos);

    auto plan_response = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "planner",
      "team_name": "Protocol Team",
      "message": {
        "type": "plan_approval_response",
        "request_id": "plan-req-1",
        "approve": false,
        "feedback": "Revise the migration scope"
      }
    })"));
    ASSERT_TRUE(plan_response.has_value());
    ASSERT_FALSE(plan_response->is_error);
    EXPECT_NE(plan_response->content.front().text.find("request_id: plan-req-1"), std::string::npos);

    auto planner_inbox = cc::utils::read_inbox("planner", std::optional<std::string_view>{"Protocol Team"});
    ASSERT_TRUE(planner_inbox.has_value()) << planner_inbox.error();
    ASSERT_EQ(planner_inbox->size(), 1u);
    auto plan_json = cc::utils::json::parse(planner_inbox->front().text);
    ASSERT_TRUE(plan_json.has_value());
    EXPECT_EQ(plan_json->root().get_string("type"), "plan_approval_response");
    EXPECT_EQ(plan_json->root().get_string("requestId"), "plan-req-1");
    EXPECT_FALSE(plan_json->root().get("approved").as_bool());
    EXPECT_EQ(plan_json->root().get_string("feedback"), "Revise the migration scope");

    auto plan_approval = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "planner",
      "team_name": "Protocol Team",
      "message": {
        "type": "plan_approval_response",
        "requestId": "plan-req-2",
        "approve": "yes",
        "permission_mode": "acceptEdits"
      }
    })"));
    ASSERT_TRUE(plan_approval.has_value());
    ASSERT_FALSE(plan_approval->is_error);

    planner_inbox = cc::utils::read_inbox("planner", std::optional<std::string_view>{"Protocol Team"});
    ASSERT_TRUE(planner_inbox.has_value()) << planner_inbox.error();
    ASSERT_EQ(planner_inbox->size(), 2u);
    auto plan_approval_json = cc::utils::json::parse(planner_inbox->back().text);
    ASSERT_TRUE(plan_approval_json.has_value());
    EXPECT_EQ(plan_approval_json->root().get_string("type"), "plan_approval_response");
    EXPECT_EQ(plan_approval_json->root().get_string("requestId"), "plan-req-2");
    EXPECT_TRUE(plan_approval_json->root().get("approved").as_bool());
    EXPECT_EQ(plan_approval_json->root().get_string("permissionMode"), "acceptEdits");

    auto shutdown_rejection = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "team-lead",
      "team_name": "Protocol Team",
      "from_agent": "reviewer",
      "message": {
        "type": "shutdown_response",
        "request_id": "shutdown-req-1",
        "approve": false,
        "reason": "Need more time"
      }
    })"));
    ASSERT_TRUE(shutdown_rejection.has_value());
    ASSERT_FALSE(shutdown_rejection->is_error);

    auto leader_inbox = cc::utils::read_inbox("team-lead", std::optional<std::string_view>{"Protocol Team"});
    ASSERT_TRUE(leader_inbox.has_value()) << leader_inbox.error();
    ASSERT_EQ(leader_inbox->size(), 1u);
    auto shutdown_json = cc::utils::json::parse(leader_inbox->front().text);
    ASSERT_TRUE(shutdown_json.has_value());
    EXPECT_EQ(shutdown_json->root().get_string("type"), "shutdown_rejected");
    EXPECT_EQ(shutdown_json->root().get_string("requestId"), "shutdown-req-1");
    EXPECT_EQ(shutdown_json->root().get_string("from"), "reviewer");
    EXPECT_EQ(shutdown_json->root().get_string("reason"), "Need more time");

    auto shutdown_approval = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "team-lead",
      "team_name": "Protocol Team",
      "from_agent": "planner",
      "message": {
        "type": "shutdown_response",
        "requestId": "shutdown-req-2",
        "approved": true
      }
    })"));
    ASSERT_TRUE(shutdown_approval.has_value());
    ASSERT_FALSE(shutdown_approval->is_error);

    leader_inbox = cc::utils::read_inbox("team-lead", std::optional<std::string_view>{"Protocol Team"});
    ASSERT_TRUE(leader_inbox.has_value()) << leader_inbox.error();
    ASSERT_EQ(leader_inbox->size(), 2u);
    auto shutdown_approval_json = cc::utils::json::parse(leader_inbox->back().text);
    ASSERT_TRUE(shutdown_approval_json.has_value());
    EXPECT_EQ(shutdown_approval_json->root().get_string("type"), "shutdown_approved");
    EXPECT_EQ(shutdown_approval_json->root().get_string("requestId"), "shutdown-req-2");
    EXPECT_EQ(shutdown_approval_json->root().get_string("from"), "planner");

    auto misrouted_shutdown_response = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "planner",
      "team_name": "Protocol Team",
      "from_agent": "reviewer",
      "message": {
        "type": "shutdown_response",
        "request_id": "shutdown-req-wrong-target",
        "approve": true
      }
    })"));
    ASSERT_TRUE(misrouted_shutdown_response.has_value());
    EXPECT_TRUE(misrouted_shutdown_response->is_error);
    EXPECT_NE(misrouted_shutdown_response->content.front().text.find("shutdown_response must be sent to \"team-lead\""), std::string::npos);

    auto structured_broadcast = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "*",
      "team_name": "Protocol Team",
      "message": {
        "type": "shutdown_request",
        "reason": "Stop all teammates"
      }
    })"));
    ASSERT_TRUE(structured_broadcast.has_value());
    EXPECT_TRUE(structured_broadcast->is_error);
    EXPECT_NE(structured_broadcast->content.front().text.find("structured messages cannot be broadcast"), std::string::npos);

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

#ifndef _WIN32
TEST(Tools, RuntimeSendMessageDeliversPlainTextToUdsPeer) {
    auto root = fs::temp_directory_path() / "cc_repl_send_message_uds_peer_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto socket_path = root / "peer.sock";
    LocalUnixLineServer server(socket_path);
    ASSERT_TRUE(server.valid());

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(std::format(R"({{
      "to": "uds:{}",
      "from_agent": "reviewer",
      "message": "Please inspect the peer session",
      "summary": "inspect peer session"
    }})", socket_path.string())));

    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("-> uds:"), std::string::npos);

    auto payload = server.wait_for_message();
    ASSERT_TRUE(payload.has_value());
    auto payload_json = cc::utils::json::parse(*payload);
    ASSERT_TRUE(payload_json.has_value()) << *payload;
    auto root_json = payload_json->root();
    EXPECT_EQ(root_json.get_string("type"), "cross_session_message");
    EXPECT_EQ(root_json.get_string("mode"), "prompt");
    EXPECT_EQ(root_json.get_string("from"), "reviewer");
    EXPECT_EQ(root_json.get_string("message"), "Please inspect the peer session");
    EXPECT_NE(root_json.get_string("value").find(R"(<cross-session-message from="reviewer">)"), std::string::npos);
    EXPECT_NE(root_json.get_string("value").find("Please inspect the peer session"), std::string::npos);

    fs::remove_all(root);
}
#endif

TEST(Tools, RuntimeSendMessageDeliversPlainTextToBridgePeer) {
    LocalBridgeIngressServer server;
    ASSERT_TRUE(server.ready());

    EnvironmentGuard endpoint_guard("CLAUDE_CODE_REMOTE_API_BASE_URL", server.base_url());
    EnvironmentGuard source_session_guard("CLAUDE_CODE_REMOTE_SESSION_ID", "session_source");
    EnvironmentGuard token_guard("CLAUDE_CODE_SESSION_ACCESS_TOKEN", "session-bridge-token");

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "bridge:session_target",
      "from_agent": "reviewer",
      "message": "Please inspect the remote peer",
      "summary": "inspect remote peer"
    })"));

    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("-> bridge:session_target"), std::string::npos);

    auto requests = server.wait_for_requests(1);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 1u);
    EXPECT_EQ(requests->front().target_session_id, "session_target");
    EXPECT_EQ(requests->front().authorization, "Bearer session-bridge-token");
    EXPECT_NE(requests->front().body.find(R"("events":[)"), std::string::npos);
    EXPECT_NE(requests->front().body.find(R"("type":"user")"), std::string::npos);
    EXPECT_NE(requests->front().body.find(R"("role":"user")"), std::string::npos);
    EXPECT_NE(requests->front().body.find(R"("session_id":"session_target")"), std::string::npos);
    EXPECT_NE(requests->front().body.find(R"(<cross-session-message from=\"session_source\">)"), std::string::npos);
    EXPECT_NE(requests->front().body.find("Please inspect the remote peer"), std::string::npos);
    EXPECT_EQ(requests->front().body.find(R"(<cross-session-message from=\"reviewer\">)"), std::string::npos);
}

TEST(Tools, RuntimeSendMessageRejectsCrossSessionStructuredMessages) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto structured_uds = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "uds:/tmp/cc-repl-peer.sock",
      "message": {
        "type": "shutdown_request",
        "reason": "done"
      }
    })"));
    ASSERT_TRUE(structured_uds.has_value());
    ASSERT_TRUE(structured_uds->is_error);
    EXPECT_NE(structured_uds->content.front().text.find("structured messages cannot be sent cross-session"), std::string::npos);

    auto structured_bridge = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "to": "bridge:session_123",
      "message": {
        "type": "shutdown_request",
        "reason": "done"
      }
    })"));
    ASSERT_TRUE(structured_bridge.has_value());
    ASSERT_TRUE(structured_bridge->is_error);
    EXPECT_NE(structured_bridge->content.front().text.find("structured messages cannot be sent cross-session"), std::string::npos);
}

TEST(Tools, RuntimeSendMessageRestoresPersistedTeammateMailboxAfterStoreReload) {
    auto root = fs::temp_directory_path() / "cc_repl_send_message_teammate_reload_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard teammate_backend_guard("CC_REPL_TEAMMATE_BACKEND", "in-process");
    cc::utils::swarm_backends::BackendRegistry::reset();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto team = cc::tools::global_team_store().create("restart-team-id", "Restart Team", {});
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));

    cc::tools::AgentTool tool;
    auto spawned = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Spawn reload reviewer",
      "prompt": "Wait for cross-process messages",
      "name": "reviewer-one",
      "team_name": "Restart Team",
      "subagent_type": "general-purpose"
    })"));
    ASSERT_TRUE(spawned.has_value());
    ASSERT_FALSE(spawned->is_error);
    auto persisted_records = cc::tools::agent_runtime::load_all_native_agent_records();
    EXPECT_TRUE(std::ranges::any_of(persisted_records, [](const auto& record) {
        return record.agent_id == "reviewer-one@Restart Team";
    }));

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "reviewer-one",
      "team_name": "Restart Team",
      "content": "Review after a runtime restart",
      "summary": "restart delivery"
    })"));
    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("reviewer-one@Restart Team"), std::string::npos);

    auto restored_record = cc::tools::agent_runtime::native_agent_store().get("reviewer-one@Restart Team");
    ASSERT_TRUE(restored_record.has_value());
    ASSERT_EQ(restored_record->pending_messages.size(), 1u);
    EXPECT_NE(restored_record->pending_messages.front().find("Review after a runtime restart"), std::string::npos);
    EXPECT_TRUE(std::ranges::any_of(restored_record->transcript, [](const auto& line) {
        return line.find("Review after a runtime restart") != std::string::npos;
    }));

    auto inbox = cc::utils::read_inbox("reviewer-one", std::optional<std::string_view>{"Restart Team"});
    ASSERT_TRUE(inbox.has_value()) << inbox.error();
    ASSERT_EQ(inbox->size(), 1u);
    EXPECT_EQ(inbox->front().from, "team-lead");
    EXPECT_EQ(inbox->front().text, "Review after a runtime restart");
    ASSERT_TRUE(inbox->front().summary.has_value());
    EXPECT_EQ(*inbox->front().summary, "restart delivery");
    EXPECT_TRUE(fs::exists(root / "teams" / "restart-team" / "inboxes" / "reviewer-one.json"));

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::utils::swarm_backends::BackendRegistry::reset();
    fs::remove_all(root);
}

TEST(Tools, AgentToolCreatesWorktreeForIsolatedBackgroundAgent) {
    if (std::system("git --version >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "git is required for worktree isolation";
    }

    auto root = fs::temp_directory_path() / "cc_repl_agent_worktree_isolation_test";
    fs::remove_all(root);
    fs::create_directories(root);
    {
        std::ofstream readme(root / "README.md");
        readme << "worktree isolation\n";
    }
    ASSERT_EQ(std::system(std::format("git -C \"{}\" init -q --template=", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" config user.email test@example.com", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" config user.name Test", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" add README.md", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" commit -q --no-verify -m init", root.string()).c_str()), 0);

    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentTool tool;
        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Run isolated",
          "prompt": "Inspect the isolated checkout",
          "name": "isolated-agent",
          "run_in_background": true,
          "isolation": "worktree"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Queued background agent isolated-agent"), std::string::npos);
    }

    auto record = cc::tools::agent_runtime::native_agent_store().get("isolated-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->cwd.has_value());
    auto worktree_path = fs::path{*record->cwd};
    EXPECT_TRUE(fs::exists(worktree_path / ".git"));
    EXPECT_EQ(worktree_path, fs::weakly_canonical(root) / ".claude" / "worktrees" / "isolated-agent");
    ASSERT_TRUE(record->isolation.has_value());
    EXPECT_EQ(*record->isolation, "worktree");
    ASSERT_TRUE(record->worktree_path.has_value());
    EXPECT_EQ(*record->worktree_path, worktree_path.string());
    ASSERT_TRUE(record->worktree_branch.has_value());
    EXPECT_EQ(*record->worktree_branch, "cc-agent-isolated-agent");
    ASSERT_TRUE(record->worktree_base_commit.has_value());
    ASSERT_TRUE(record->worktree_git_root.has_value());
    EXPECT_EQ(*record->worktree_git_root, fs::weakly_canonical(root).string());

    auto cleanup = cc::tools::agent::cleanup_agent_worktree("isolated-agent");
    EXPECT_TRUE(cleanup.attempted);
    EXPECT_TRUE(cleanup.removed);
    EXPECT_FALSE(fs::exists(worktree_path));

    auto cleaned = cc::tools::agent_runtime::native_agent_store().get("isolated-agent");
    ASSERT_TRUE(cleaned.has_value());
    EXPECT_TRUE(cleaned->worktree_cleanup_performed);
    EXPECT_FALSE(cleaned->worktree_path.has_value());
    EXPECT_FALSE(cleaned->cwd.has_value());

    EXPECT_NE(std::system(std::format(
        "git -C \"{}\" rev-parse --verify cc-agent-isolated-agent >/dev/null 2>&1",
        root.string()).c_str()), 0);
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolPreservesChangedWorktreeAndReportsPath) {
    if (std::system("git --version >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "git is required for worktree isolation";
    }

    auto root = fs::temp_directory_path() / "cc_repl_agent_worktree_dirty_test";
    fs::remove_all(root);
    fs::create_directories(root);
    {
        std::ofstream readme(root / "README.md");
        readme << "worktree dirty preservation\n";
    }
    ASSERT_EQ(std::system(std::format("git -C \"{}\" init -q --template=", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" config user.email test@example.com", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" config user.name Test", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" add README.md", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" commit -q --no-verify -m init", root.string()).c_str()), 0);

    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentTool tool;
        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Run isolated",
          "prompt": "Leave changed worktree",
          "name": "dirty-agent",
          "run_in_background": true,
          "isolation": "worktree"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
    }

    auto record = cc::tools::agent_runtime::native_agent_store().get("dirty-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->worktree_path.has_value());
    auto worktree_path = fs::path{*record->worktree_path};
    {
        std::ofstream dirty(worktree_path / "dirty.txt");
        dirty << "agent changes\n";
    }

    auto cleanup = cc::tools::agent::cleanup_agent_worktree("dirty-agent");
    EXPECT_TRUE(cleanup.attempted);
    EXPECT_FALSE(cleanup.removed);
    EXPECT_TRUE(cleanup.changed);
    EXPECT_TRUE(fs::exists(worktree_path));

    auto retained = cc::tools::agent_runtime::native_agent_store().get("dirty-agent");
    ASSERT_TRUE(retained.has_value());
    ASSERT_TRUE(retained->worktree_path.has_value());
    EXPECT_EQ(*retained->worktree_path, worktree_path.string());
    EXPECT_FALSE(retained->worktree_cleanup_performed);

    cc::tools::agent_runtime::native_agent_store().mark_completed("dirty-agent", "dirty worktree retained");
    auto notifications = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications.front().find("<worktree_path>"), std::string::npos);
    EXPECT_NE(notifications.front().find(worktree_path.string()), std::string::npos);
    EXPECT_NE(notifications.front().find("<worktree_branch>cc-agent-dirty-agent</worktree_branch>"), std::string::npos);

    (void)std::system(std::format("git -C \"{}\" worktree remove --force \"{}\" >/dev/null 2>&1",
        root.string(), worktree_path.string()).c_str());
    (void)std::system(std::format("git -C \"{}\" branch -D cc-agent-dirty-agent >/dev/null 2>&1",
        root.string()).c_str());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTaskToolsExposeNativeBackgroundAgents) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_task_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::AgentTool tool;
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async",
      "prompt": "Wait for task inspection",
      "name": "task-agent",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value());
    ASSERT_FALSE(started->is_error);
    ASSERT_FALSE(started->content.empty());
    EXPECT_NE(started->content.front().text.find("outputFile:"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("task-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->transcript_path.has_value());
    ASSERT_TRUE(record->output_file_path.has_value());
    EXPECT_NE(started->content.front().text.find(*record->output_file_path), std::string::npos);
    EXPECT_TRUE(fs::exists(*record->transcript_path));
    EXPECT_TRUE(fs::is_symlink(*record->output_file_path));
    EXPECT_EQ(fs::read_symlink(*record->output_file_path), fs::path{*record->transcript_path});

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto listed = registry.execute("task_list", cc::core::ToolInput::from_json("{}"));
    ASSERT_TRUE(listed.has_value());
    ASSERT_FALSE(listed->is_error);
    ASSERT_FALSE(listed->content.empty());
    EXPECT_NE(listed->content.front().text.find("task-agent [queued]"), std::string::npos);
    EXPECT_NE(listed->content.front().text.find("output_file:"), std::string::npos);
    EXPECT_NE(listed->content.front().text.find(*record->output_file_path), std::string::npos);

    auto got = registry.execute("task_get", cc::core::ToolInput::from_json(R"({
      "task_id": "task-agent"
    })"));
    ASSERT_TRUE(got.has_value());
    ASSERT_FALSE(got->is_error);
    ASSERT_FALSE(got->content.empty());
    EXPECT_NE(got->content.front().text.find("Agent general-purpose: task-agent"), std::string::npos);

    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
      "task_id": "task-agent"
    })"));
    ASSERT_TRUE(stopped.has_value());
    ASSERT_FALSE(stopped->is_error);
    ASSERT_FALSE(stopped->content.empty());
    EXPECT_NE(stopped->content.front().text.find("task-agent [cancelled]"), std::string::npos);
    EXPECT_NE(stopped->content.front().text.find("stop requested"), std::string::npos);

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "task-agent"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("<task_notification>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<status>stopped</status>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("stop requested"), std::string::npos);

    std::ifstream stopped_output_file(*record->output_file_path);
    std::string stopped_output_text(
        (std::istreambuf_iterator<char>(stopped_output_file)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(stopped_output_text.find("system: agent cancelled: stop requested"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, StandaloneTaskToolsExposeNativeBackgroundAgents) {
    auto root = fs::temp_directory_path() / "cc_repl_standalone_native_agent_task_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "standalone-native-agent",
        .agent_type = "general-purpose",
        .description = "Standalone native task",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Completed,
        .output = "standalone done",
        .transcript = {"assistant: standalone done"},
    });

    cc::tools::TaskListTool list_tool;
    auto listed = list_tool.execute();
    auto listed_native = std::ranges::find_if(listed, [](const auto* task) {
        return task && task->id == "standalone-native-agent";
    });
    ASSERT_NE(listed_native, listed.end());
    EXPECT_EQ((*listed_native)->status, cc::tools::TaskStatus::Completed);
    EXPECT_EQ((*listed_native)->description, "Standalone native task");

    cc::tools::TaskGetTool get_tool;
    auto got = get_tool.execute("standalone-native-agent");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ((*got)->id, "standalone-native-agent");
    EXPECT_EQ((*got)->result, std::optional<std::string>{"standalone done"});

    cc::tools::TaskOutputTool output_tool;
    auto output = output_tool.execute("standalone-native-agent");
    ASSERT_TRUE(output.has_value());
    EXPECT_NE(output->find("standalone done"), std::string_view::npos);
    EXPECT_NE(output->find("<task_notification>"), std::string_view::npos);
    EXPECT_NE(output->find("<status>completed</status>"), std::string_view::npos);

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "standalone-stop-agent",
        .agent_type = "general-purpose",
        .description = "Standalone stop task",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .remote_task_id = "remote-stop-task",
    });
    cc::tools::TaskStopTool stop_tool;
    auto stopped = stop_tool.execute("remote-stop-task");
    ASSERT_TRUE(stopped.has_value());
    auto stopped_record = cc::tools::agent_runtime::native_agent_store().get("standalone-stop-agent");
    ASSERT_TRUE(stopped_record.has_value());
    EXPECT_EQ(stopped_record->status, cc::tools::agent_runtime::NativeAgentStatus::Cancelled);
    ASSERT_TRUE(stopped_record->error.has_value());
    EXPECT_EQ(*stopped_record->error, "stop requested");

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    { std::error_code ec; fs::remove_all(root, ec); }
}

TEST(Tools, AgentToolUpdatesProgressAfterStartingApiStream) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_progress_test";
    { std::error_code ec; fs::remove_all(root, ec); }
    fs::create_directories(root);
    LocalSlowAnthropicStreamServer server(std::chrono::milliseconds(750));
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "stream-progress-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::AgentTool tool({}, 0, &registry);
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Track async progress while streaming",
      "prompt": "Wait for the slow stream to complete",
      "name": "stream-progress-agent",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value()) << started.error().format();
    ASSERT_FALSE(started->is_error);
    ASSERT_TRUE(server.wait_for_request());

    bool observed_running_progress = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto record = cc::tools::agent_runtime::native_agent_store().get("stream-progress-agent");
        ASSERT_TRUE(record.has_value());
        if (record->status == cc::tools::agent_runtime::NativeAgentStatus::Running &&
            record->progress &&
            *record->progress > 0.0 &&
            *record->progress < 1.0) {
            observed_running_progress = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(observed_running_progress);
    ASSERT_TRUE(wait_for_native_agent_status(
        "stream-progress-agent",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));
    auto completed = cc::tools::agent_runtime::native_agent_store().get("stream-progress-agent");
    ASSERT_TRUE(completed.has_value());
    ASSERT_TRUE(completed->progress.has_value());
    EXPECT_DOUBLE_EQ(*completed->progress, 1.0);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    { std::error_code ec; fs::remove_all(root, ec); }
}

TEST(Tools, TaskStopCancelsRunningBackgroundAgentDuringModelStream) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_stream_cancel_test";
    fs::remove_all(root);
    fs::create_directories(root);
    LocalSlowAnthropicStreamServer server(std::chrono::milliseconds(750));
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "stream-cancel-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::AgentTool tool({}, 0, &registry);
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async and cancel while streaming",
      "prompt": "Wait for cancellation during the model stream",
      "name": "stream-cancel-agent",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value()) << started.error().format();
    ASSERT_FALSE(started->is_error);
    ASSERT_TRUE(server.wait_for_request());
    auto body = server.last_body();
    ASSERT_TRUE(body.has_value());
    EXPECT_NE(body->find("Wait for cancellation during the model stream"), std::string::npos);

    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
      "task_id": "stream-cancel-agent"
    })"));
    ASSERT_TRUE(stopped.has_value());
    ASSERT_FALSE(stopped->is_error);
    ASSERT_FALSE(stopped->content.empty());
    EXPECT_NE(stopped->content.front().text.find("stream-cancel-agent [cancelled]"), std::string::npos);

    bool observed_stream_cancel = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto record = cc::tools::agent_runtime::native_agent_store().get("stream-cancel-agent");
        ASSERT_TRUE(record.has_value());
        if (record->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled &&
            record->error &&
            record->error->find("while waiting for model stream") != std::string::npos) {
            observed_stream_cancel = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(observed_stream_cancel);

    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    auto record = cc::tools::agent_runtime::native_agent_store().get("stream-cancel-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Cancelled);
    ASSERT_TRUE(record->error.has_value());
    EXPECT_NE(record->error->find("while waiting for model stream"), std::string::npos);
    EXPECT_FALSE(record->output.has_value());

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "stream-cancel-agent"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("<status>stopped</status>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("while waiting for model stream"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, TaskStopCancelsRunningBackgroundAgentDuringSleepToolExecution) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_sleep_cancel_test";
    fs::remove_all(root);
    fs::create_directories(root);
    LocalSleepToolUseAnthropicServer server;
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "sleep-cancel-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::AgentTool tool({}, 0, &registry);
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async and cancel during sleep",
      "prompt": "Use sleep until I stop you",
      "name": "sleep-cancel-agent",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value()) << started.error().format();
    ASSERT_FALSE(started->is_error);
    ASSERT_TRUE(server.wait_for_request_count(1));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stop_started = std::chrono::steady_clock::now();
    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
      "task_id": "sleep-cancel-agent"
    })"));
    ASSERT_TRUE(stopped.has_value());
    ASSERT_FALSE(stopped->is_error);

    bool observed_sleep_cancel = false;
    for (int attempt = 0; attempt < 250; ++attempt) {
        auto record = cc::tools::agent_runtime::native_agent_store().get("sleep-cancel-agent");
        ASSERT_TRUE(record.has_value());
        if (record->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled &&
            record->error &&
            record->error->find("while executing tool sleep") != std::string::npos) {
            observed_sleep_cancel = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(observed_sleep_cancel);
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stop_started).count(),
        5000);
    EXPECT_EQ(server.request_count(), 1u);

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "sleep-cancel-agent"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("<status>stopped</status>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("while executing tool sleep"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, TaskStopCancelsRunningBackgroundAgentDuringWebFetchToolExecution) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_webfetch_cancel_test";
    fs::remove_all(root);
    fs::create_directories(root);
    LocalSlowContentServer content_server(std::chrono::seconds(2));
    ASSERT_TRUE(content_server.valid());
    const auto fetch_input = std::format(
        R"({{"url":"{}"}})",
        cc::tools::agent::json_escape_string(content_server.url()));
    LocalScriptedToolUseAnthropicServer server(
        "WebFetch",
        fetch_input,
        "toolu_webfetch_cancel",
        "should not continue after web fetch cancellation");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "webfetch-cancel-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::AgentTool tool({}, 0, &registry);
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async and cancel during WebFetch",
      "prompt": "Use WebFetch until I stop you",
      "name": "webfetch-cancel-agent",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value()) << started.error().format();
    ASSERT_FALSE(started->is_error);
    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(content_server.wait_for_request());

    const auto stop_started = std::chrono::steady_clock::now();
    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
      "task_id": "webfetch-cancel-agent"
    })"));
    ASSERT_TRUE(stopped.has_value());
    ASSERT_FALSE(stopped->is_error);

    bool observed_webfetch_cancel = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto record = cc::tools::agent_runtime::native_agent_store().get("webfetch-cancel-agent");
        ASSERT_TRUE(record.has_value());
        if (record->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled &&
            record->error &&
            record->error->find("while executing tool WebFetch") != std::string::npos) {
            observed_webfetch_cancel = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(observed_webfetch_cancel);
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stop_started).count(),
        2000);
    EXPECT_FALSE(server.wait_for_request_count(2, std::chrono::milliseconds(250)));

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "webfetch-cancel-agent"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("<status>stopped</status>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("while executing tool WebFetch"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, TaskStopCancelsRunningBackgroundAgentDuringBashToolExecution) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_bash_cancel_test";
    fs::remove_all(root);
    fs::create_directories(root);
    LocalBashToolUseAnthropicServer server;
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "bash-cancel-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::AgentTool tool({}, 0, &registry);
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async and cancel during bash",
      "prompt": "Use Bash until I stop you",
      "name": "bash-cancel-agent",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value()) << started.error().format();
    ASSERT_FALSE(started->is_error);
    ASSERT_TRUE(server.wait_for_request_count(1));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stop_started = std::chrono::steady_clock::now();
    auto stopped = registry.execute("task_stop", cc::core::ToolInput::from_json(R"({
      "task_id": "bash-cancel-agent"
    })"));
    ASSERT_TRUE(stopped.has_value());
    ASSERT_FALSE(stopped->is_error);

    bool observed_bash_cancel = false;
    for (int attempt = 0; attempt < 250; ++attempt) {
        auto record = cc::tools::agent_runtime::native_agent_store().get("bash-cancel-agent");
        ASSERT_TRUE(record.has_value());
        if (record->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled &&
            record->error &&
            record->error->find("while executing tool Bash") != std::string::npos) {
            observed_bash_cancel = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(observed_bash_cancel);
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stop_started).count(),
        5000);
    EXPECT_EQ(server.request_count(), 1u);

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "bash-cancel-agent"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("<status>stopped</status>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("while executing tool Bash"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTaskOutputIncludesNativeAgentCompletionNotification) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_completion_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "completed-agent",
        .agent_type = "reviewer",
        .name = "completed reviewer",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .worktree_path = (root / "agent-worktree").string(),
        .worktree_branch = "cc-agent-completed-agent",
        .transcript = {"user: work", "assistant: done"},
    });
    cc::tools::agent_runtime::native_agent_store().mark_completed("completed-agent", "done");

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "completed-agent"
    })"));

    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("done"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("assistant: done"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<task_notification>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<status>completed</status>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<result>done</result>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("worktree_path: " + (root / "agent-worktree").string()), std::string::npos);
    EXPECT_NE(output->content.front().text.find("worktree_branch: cc-agent-completed-agent"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<worktree_path>" + (root / "agent-worktree").string()), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<worktree_branch>cc-agent-completed-agent</worktree_branch>"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTaskUpdateMarksNativeAgentFailedWithOutputArtifactAndNotification) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_failure_artifact_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "failed-agent",
        .agent_type = "reviewer",
        .name = "failed reviewer",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .transcript = {"user: inspect failure path"},
    });

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto update = registry.execute("task_update", cc::core::ToolInput::from_json(R"({
      "task_id": "failed-agent",
      "status": "failed",
      "result": "agent crashed while reading bindings"
    })"));
    ASSERT_TRUE(update.has_value());
    ASSERT_FALSE(update->is_error);

    auto record = cc::tools::agent_runtime::native_agent_store().get("failed-agent");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Failed);
    ASSERT_TRUE(record->error.has_value());
    EXPECT_EQ(*record->error, "agent crashed while reading bindings");
    ASSERT_TRUE(record->output_file_path.has_value());
    EXPECT_TRUE(fs::exists(*record->output_file_path));
    EXPECT_TRUE(fs::is_symlink(*record->output_file_path));

    std::ifstream artifact(*record->output_file_path);
    std::string artifact_text(
        (std::istreambuf_iterator<char>(artifact)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(artifact_text.find("user: inspect failure path"), std::string::npos);
    EXPECT_NE(artifact_text.find("system: agent failed: agent crashed while reading bindings"), std::string::npos);

    auto output = registry.execute("task_output", cc::core::ToolInput::from_json(R"({
      "task_id": "failed-agent"
    })"));
    ASSERT_TRUE(output.has_value());
    ASSERT_FALSE(output->is_error);
    ASSERT_FALSE(output->content.empty());
    EXPECT_NE(output->content.front().text.find("<task_notification>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<status>failed</status>"), std::string::npos);
    EXPECT_NE(output->content.front().text.find("<result>agent crashed while reading bindings</result>"), std::string::npos);

    auto notifications = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications.front().find("<task_id>failed-agent</task_id>"), std::string::npos);
    EXPECT_NE(notifications.front().find("<status>failed</status>"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, NativeAgentNotificationsAreConsumedOnce) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_notification_once_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "notify-agent",
        .agent_type = "reviewer",
        .name = "notify reviewer",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
    });
    cc::tools::agent_runtime::native_agent_store().mark_completed("notify-agent", "review complete");

    auto first = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_NE(first.front().find("<task_notification>"), std::string::npos);
    EXPECT_NE(first.front().find("<status>completed</status>"), std::string::npos);
    EXPECT_NE(first.front().find("<result>review complete</result>"), std::string::npos);

    auto second = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    EXPECT_TRUE(second.empty());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    EXPECT_TRUE(restored.empty());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, NativeAgentRecordPersistsWorktreeMetadata) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_worktree_metadata_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "metadata-agent",
        .agent_type = "reviewer",
        .cwd = (root / "worktree").string(),
        .isolation = "worktree",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .worktree_path = (root / "worktree").string(),
        .worktree_branch = "cc-agent-metadata-agent",
        .worktree_base_commit = "abc123",
        .worktree_git_root = root.string(),
    });

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored = cc::tools::agent_runtime::native_agent_store().get("metadata-agent");
    ASSERT_TRUE(restored.has_value());
    ASSERT_TRUE(restored->worktree_path.has_value());
    EXPECT_EQ(*restored->worktree_path, (root / "worktree").string());
    ASSERT_TRUE(restored->worktree_branch.has_value());
    EXPECT_EQ(*restored->worktree_branch, "cc-agent-metadata-agent");
    ASSERT_TRUE(restored->worktree_base_commit.has_value());
    EXPECT_EQ(*restored->worktree_base_commit, "abc123");
    ASSERT_TRUE(restored->worktree_git_root.has_value());
    EXPECT_EQ(*restored->worktree_git_root, root.string());
    EXPECT_FALSE(restored->worktree_cleanup_performed);

    cc::tools::agent_runtime::native_agent_store().mark_worktree_cleaned("metadata-agent");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto cleaned = cc::tools::agent_runtime::native_agent_store().get("metadata-agent");
    ASSERT_TRUE(cleaned.has_value());
    EXPECT_TRUE(cleaned->worktree_cleanup_performed);
    EXPECT_FALSE(cleaned->worktree_path.has_value());
    EXPECT_FALSE(cleaned->cwd.has_value());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, NativeAgentRecordPersistsSidechainJsonlAndResumesFromIt) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_sidechain_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "sidechain-agent",
        .agent_type = "reviewer",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Completed,
        .transcript = {"user: inspect generated bindings", "assistant: bindings reviewed"},
    });

    auto record = cc::tools::agent_runtime::native_agent_store().get("sidechain-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->transcript_path.has_value());
    ASSERT_TRUE(record->sidechain_jsonl_path.has_value());
    EXPECT_TRUE(fs::exists(*record->transcript_path));
    EXPECT_TRUE(fs::exists(*record->sidechain_jsonl_path));

    std::ifstream sidechain_in(*record->sidechain_jsonl_path);
    std::string sidechain_text(
        (std::istreambuf_iterator<char>(sidechain_in)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(sidechain_text.find(R"("type":"user")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("type":"assistant")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("parentUuid":null)"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("parentUuid":"sidechain-agent-0")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("isSidechain":true)"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("agentId":"sidechain-agent")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("message":{"role":"user","content":[{"type":"text","text":"inspect generated bindings"}]})"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("agent_id":"sidechain-agent")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("role":"user")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("raw":"assistant: bindings reviewed")"), std::string::npos);

    std::error_code ec;
    fs::remove(*record->transcript_path, ec);
    ASSERT_FALSE(fs::exists(*record->transcript_path));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto resumed = cc::tools::agent_runtime::resume_agent("sidechain-agent");
    ASSERT_TRUE(resumed.has_value()) << resumed.error();
    ASSERT_EQ(resumed->transcript.size(), 2u);
    EXPECT_EQ(resumed->transcript.front(), "user: inspect generated bindings");
    EXPECT_EQ(resumed->transcript.back(), "assistant: bindings reviewed");

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, NativeAgentResumeReadsTypeScriptSidechainTranscriptEntries) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_ts_sidechain_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "ts-sidechain-agent",
        .agent_type = "reviewer",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Completed,
        .output = "completed from persisted TS transcript",
        .transcript = {"system: placeholder"},
    });

    auto record = cc::tools::agent_runtime::native_agent_store().get("ts-sidechain-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->transcript_path.has_value());
    ASSERT_TRUE(record->sidechain_jsonl_path.has_value());

    std::error_code ec;
    fs::remove(*record->transcript_path, ec);
    ASSERT_FALSE(fs::exists(*record->transcript_path));

    {
        std::ofstream sidechain(*record->sidechain_jsonl_path, std::ios::trunc);
        sidechain << R"({"type":"user","uuid":"u1","parentUuid":null,"isSidechain":true,"agentId":"ts-sidechain-agent","message":{"role":"user","content":[{"type":"text","text":"Inspect TS persisted prompt"}]}})" << '\n';
        sidechain << R"({"type":"assistant","uuid":"a1","parentUuid":"u1","isSidechain":true,"agentId":"ts-sidechain-agent","message":{"role":"assistant","content":[{"type":"text","text":"TS persisted answer"},{"type":"tool_use","id":"tool-1","name":"Read","input":{"file_path":"README.md"}}]}})" << '\n';
        sidechain << R"({"type":"user","uuid":"u2","parentUuid":"a1","isSidechain":true,"agentId":"ts-sidechain-agent","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"tool-1","content":[{"type":"text","text":"README content"}]}]}})" << '\n';
    }

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto resumed = cc::tools::agent_runtime::resume_agent("ts-sidechain-agent");
    ASSERT_TRUE(resumed.has_value()) << resumed.error();
    ASSERT_EQ(resumed->transcript.size(), 3u);
    EXPECT_EQ(resumed->transcript[0], "user: Inspect TS persisted prompt");
    EXPECT_NE(resumed->transcript[1].find("assistant: TS persisted answer"), std::string::npos);
    EXPECT_NE(resumed->transcript[1].find("[tool_use:Read]"), std::string::npos);
    EXPECT_EQ(resumed->transcript[2], "user: tool_result: README content");

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, NativeAgentStructuredSidechainPreservesToolUseAndResultBlocks) {
    auto root = fs::temp_directory_path() / "cc_repl_native_agent_structured_sidechain_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "structured-agent",
        .agent_type = "reviewer",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
    });

    cc::services::api::Message assistant;
    assistant.role = "assistant";
    assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::Text,
        .text = "I will inspect README.",
    });
    assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolUse,
        .tool_use_id = "tool-structured-1",
        .tool_name = "Read",
        .tool_input_json = R"({"file_path":"README.md","limit":20})",
    });
    cc::tools::agent_runtime::native_agent_store().append_sidechain_message(
        "structured-agent",
        assistant.role,
        cc::tools::agent::message_content_sidechain_json(assistant),
        cc::tools::agent::message_content_text(assistant));

    cc::services::api::Message tool_result;
    tool_result.role = "user";
    tool_result.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolResult,
        .text = "README content",
        .tool_use_id = "tool-structured-1",
    });
    cc::tools::agent_runtime::native_agent_store().append_sidechain_message(
        "structured-agent",
        tool_result.role,
        cc::tools::agent::message_content_sidechain_json(tool_result),
        cc::tools::agent::message_content_text(tool_result));

    auto record = cc::tools::agent_runtime::native_agent_store().get("structured-agent");
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->sidechain_entries.size(), 2u);
    ASSERT_TRUE(record->sidechain_jsonl_path.has_value());
    ASSERT_TRUE(fs::exists(*record->sidechain_jsonl_path));

    std::ifstream sidechain_in(*record->sidechain_jsonl_path);
    std::string sidechain_text(
        (std::istreambuf_iterator<char>(sidechain_in)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(sidechain_text.find(R"("type":"tool_use")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("id":"tool-structured-1")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("name":"Read")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("file_path":"README.md")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("limit":20)"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("type":"tool_result")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("tool_use_id":"tool-structured-1")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("content":[{"type":"text","text":"README content"}])"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("parentUuid":null)"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("parentUuid":"structured-agent-0")"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored = cc::tools::agent_runtime::native_agent_store().get("structured-agent");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->sidechain_entries.size(), 2u);
    ASSERT_EQ(restored->transcript.size(), 2u);
    EXPECT_NE(restored->transcript[0].find("[tool_use:Read]"), std::string::npos);
    EXPECT_NE(restored->transcript[1].find("tool_result: README content"), std::string::npos);

    auto resumed = cc::tools::agent_runtime::resume_agent("structured-agent");
    ASSERT_TRUE(resumed.has_value()) << resumed.error();
    ASSERT_EQ(resumed->transcript.size(), 3u);
    EXPECT_NE(resumed->transcript[0].find("[tool_use:Read]"), std::string::npos);
    EXPECT_NE(resumed->transcript[1].find("tool_result: README content"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolBuildsFilteredStructuredResumeMessagesFromSidechain) {
    std::vector<std::string> entries{
        R"({"type":"user","uuid":"u1","parentUuid":null,"isSidechain":true,"agentId":"resume-filtered","message":{"role":"user","content":[{"type":"text","text":"First user"}]}})",
        R"({"type":"assistant","uuid":"a-whitespace","parentUuid":"u1","isSidechain":true,"agentId":"resume-filtered","message":{"role":"assistant","content":[{"type":"text","text":"\n\t  "}]}})",
        R"({"type":"user","uuid":"u2","parentUuid":"a-whitespace","isSidechain":true,"agentId":"resume-filtered","message":{"role":"user","content":[{"type":"text","text":"Second user"}]}})",
        R"({"type":"assistant","uuid":"a-thinking","parentUuid":"u2","isSidechain":true,"agentId":"resume-filtered","message":{"role":"assistant","content":[{"type":"thinking","thinking":"orphaned reasoning","signature":"sig"}]}})",
        R"({"type":"assistant","uuid":"a-unresolved","parentUuid":"a-thinking","isSidechain":true,"agentId":"resume-filtered","message":{"role":"assistant","content":[{"type":"text","text":"I will call a missing tool"},{"type":"tool_use","id":"missing-tool","name":"Read","input":{"file_path":"missing.md"}}]}})",
        R"({"type":"assistant","uuid":"a-resolved","parentUuid":"a-unresolved","isSidechain":true,"agentId":"resume-filtered","message":{"role":"assistant","content":[{"type":"text","text":"I will read README"},{"type":"tool_use","id":"read-ok","name":"Read","input":{"file_path":"README.md"}}]}})",
        R"({"type":"user","uuid":"u3","parentUuid":"a-resolved","isSidechain":true,"agentId":"resume-filtered","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"read-ok","content":[{"type":"text","text":"README content"}]}]}})",
    };

    auto messages = cc::tools::agent::resume_messages_from_sidechain_entries(entries);
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[0].role, "user");
    ASSERT_EQ(messages[0].content.size(), 2u);
    EXPECT_EQ(messages[0].content[0].text, "First user");
    EXPECT_EQ(messages[0].content[1].text, "Second user");
    EXPECT_EQ(messages[1].role, "assistant");
    ASSERT_EQ(messages[1].content.size(), 2u);
    EXPECT_EQ(messages[1].content[0].text, "I will read README");
    EXPECT_EQ(messages[1].content[1].type, cc::services::api::ContentBlockType::ToolUse);
    EXPECT_EQ(messages[1].content[1].tool_use_id, "read-ok");
    EXPECT_EQ(messages[2].role, "user");
    ASSERT_EQ(messages[2].content.size(), 1u);
    EXPECT_EQ(messages[2].content[0].type, cc::services::api::ContentBlockType::ToolResult);
    EXPECT_EQ(messages[2].content[0].tool_use_id, "read-ok");
    EXPECT_EQ(messages[2].content[0].text, "README content");
}

TEST(Tools, AgentToolReplaysResumeContentReplacementRecordsFromSidechain) {
    std::vector<std::string> entries{
        R"({"type":"user","uuid":"u1","parentUuid":null,"isSidechain":true,"agentId":"resume-replacement","message":{"role":"user","content":[{"type":"text","text":"Read a large report"}]}})",
        R"({"type":"assistant","uuid":"a1","parentUuid":"u1","isSidechain":true,"agentId":"resume-replacement","message":{"role":"assistant","content":[{"type":"tool_use","id":"large-result-1","name":"Bash","input":{"command":"cat report.txt"}}]}})",
        R"({"type":"user","uuid":"u2","parentUuid":"a1","isSidechain":true,"agentId":"resume-replacement","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"large-result-1","content":[{"type":"text","text":"FULL LARGE RESULT"}]}]}})",
        R"({"type":"content-replacement","sessionId":"session-1","agentId":"resume-replacement","replacements":[{"kind":"tool-result","toolUseId":"large-result-1","replacement":"[persisted preview for large-result-1]"}]})",
    };

    auto messages = cc::tools::agent::resume_messages_from_sidechain_entries(entries);
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[2].role, "user");
    ASSERT_EQ(messages[2].content.size(), 1u);
    EXPECT_EQ(messages[2].content[0].type, cc::services::api::ContentBlockType::ToolResult);
    EXPECT_EQ(messages[2].content[0].tool_use_id, "large-result-1");
    EXPECT_EQ(messages[2].content[0].text, "[persisted preview for large-result-1]");
}

TEST(Tools, AgentToolPersistsLiveContentReplacementRecordsForLargeToolResults) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_live_content_replacement_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "live-replacement",
        .agent_type = "general-purpose",
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
    });

    std::string large_result(210'000, 'x');
    std::vector<cc::services::api::Message> messages;
    cc::services::api::Message assistant;
    assistant.role = "assistant";
    assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolUse,
        .tool_use_id = "huge-1",
        .tool_name = "Bash",
        .tool_input_json = R"({"command":"cat huge.log"})",
    });
    messages.push_back(std::move(assistant));
    cc::services::api::Message result;
    result.role = "user";
    result.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolResult,
        .text = large_result,
        .tool_use_id = "huge-1",
    });
    messages.push_back(std::move(result));

    cc::tools::agent::AgentContentReplacementState state;
    auto replaced = cc::tools::agent::apply_agent_tool_result_budget("live-replacement", messages, state);
    EXPECT_EQ(replaced.newly_replaced, 1u);
    EXPECT_EQ(replaced.reapplied, 0u);
    ASSERT_EQ(messages[1].content.size(), 1u);
    EXPECT_NE(messages[1].content[0].text.find("<persisted-output>"), std::string::npos);
    EXPECT_NE(messages[1].content[0].text.find("Full output saved to:"), std::string::npos);
    EXPECT_NE(messages[1].content[0].text.find("Preview (first 2000 bytes):"), std::string::npos);
    EXPECT_LT(messages[1].content[0].text.size(), large_result.size());

    auto persisted_path = root / "runtime" / "tool-results" / "live-replacement-huge-1.txt";
    ASSERT_TRUE(fs::exists(persisted_path));
    EXPECT_EQ(read_file(persisted_path), large_result);

    auto record = cc::tools::agent_runtime::native_agent_store().get("live-replacement");
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->sidechain_entries.size(), 1u);
    EXPECT_NE(record->sidechain_entries.front().find(R"("type":"content-replacement")"), std::string::npos);
    EXPECT_NE(record->sidechain_entries.front().find(R"("agentId":"live-replacement")"), std::string::npos);
    EXPECT_NE(record->sidechain_entries.front().find(R"("toolUseId":"huge-1")"), std::string::npos);

    const auto replacement_text = messages[1].content[0].text;
    messages[1].content[0].text = large_result;
    auto reapplied = cc::tools::agent::apply_agent_tool_result_budget("live-replacement", messages, state);
    EXPECT_EQ(reapplied.newly_replaced, 0u);
    EXPECT_EQ(reapplied.reapplied, 1u);
    EXPECT_EQ(messages[1].content[0].text, replacement_text);
    auto after_reapply = cc::tools::agent_runtime::native_agent_store().get("live-replacement");
    ASSERT_TRUE(after_reapply.has_value());
    EXPECT_EQ(after_reapply->sidechain_entries.size(), 1u);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolSkipsLiveContentReplacementForUnboundedToolResults) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_unbounded_content_replacement_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "unbounded-replacement",
        .agent_type = "general-purpose",
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
    });

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto* read_tool = registry.get("Read");
    ASSERT_NE(read_tool, nullptr);
    EXPECT_TRUE(read_tool->definition().max_result_size_unbounded);
    auto* bash_tool = registry.get("Bash");
    ASSERT_NE(bash_tool, nullptr);
    EXPECT_FALSE(bash_tool->definition().max_result_size_unbounded);
    EXPECT_EQ(bash_tool->definition().max_result_size_chars, 30'000u);
    auto skip_names = cc::tools::agent::unbounded_tool_result_budget_names(registry.get_visible_definitions());
    EXPECT_TRUE(skip_names.contains("read"));

    auto make_messages = [](std::string tool_name, std::string tool_use_id, const std::string& text) {
        std::vector<cc::services::api::Message> messages;
        cc::services::api::Message assistant;
        assistant.role = "assistant";
        assistant.content.push_back(cc::services::api::ContentBlock{
            .type = cc::services::api::ContentBlockType::ToolUse,
            .tool_use_id = tool_use_id,
            .tool_name = std::move(tool_name),
            .tool_input_json = "{}",
        });
        messages.push_back(std::move(assistant));
        cc::services::api::Message result;
        result.role = "user";
        result.content.push_back(cc::services::api::ContentBlock{
            .type = cc::services::api::ContentBlockType::ToolResult,
            .text = text,
            .tool_use_id = std::move(tool_use_id),
        });
        messages.push_back(std::move(result));
        return messages;
    };

    std::string large_result(210'000, 'r');
    auto read_messages = make_messages("Read", "read-huge", large_result);
    cc::tools::agent::AgentContentReplacementState read_state;
    auto skipped = cc::tools::agent::apply_agent_tool_result_budget(
        "unbounded-replacement",
        read_messages,
        read_state,
        skip_names);
    EXPECT_EQ(skipped.newly_replaced, 0u);
    EXPECT_EQ(skipped.reapplied, 0u);
    EXPECT_EQ(read_messages[1].content[0].text, large_result);
    auto skipped_record = cc::tools::agent_runtime::native_agent_store().get("unbounded-replacement");
    ASSERT_TRUE(skipped_record.has_value());
    EXPECT_TRUE(skipped_record->sidechain_entries.empty());

    auto bash_messages = make_messages("Bash", "bash-huge", large_result);
    cc::tools::agent::AgentContentReplacementState bash_state;
    auto replaced = cc::tools::agent::apply_agent_tool_result_budget(
        "unbounded-replacement",
        bash_messages,
        bash_state,
        skip_names);
    EXPECT_EQ(replaced.newly_replaced, 1u);
    EXPECT_NE(bash_messages[1].content[0].text.find("<persisted-output>"), std::string::npos);
    auto replaced_record = cc::tools::agent_runtime::native_agent_store().get("unbounded-replacement");
    ASSERT_TRUE(replaced_record.has_value());
    ASSERT_EQ(replaced_record->sidechain_entries.size(), 1u);
    EXPECT_NE(replaced_record->sidechain_entries.front().find(R"("toolUseId":"bash-huge")"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolUsesFiniteToolResultThresholdsBeforeAggregateBudget) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_finite_threshold_replacement_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "finite-threshold",
        .agent_type = "general-purpose",
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
    });

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto thresholds = cc::tools::agent::tool_result_budget_thresholds(registry.get_visible_definitions());
    ASSERT_TRUE(thresholds.contains("bash"));
    EXPECT_EQ(thresholds["bash"], 30'000u);
    ASSERT_TRUE(thresholds.contains("grep"));
    EXPECT_EQ(thresholds["grep"], 20'000u);

    std::string bash_result(40'000, 'b');
    std::vector<cc::services::api::Message> messages;
    cc::services::api::Message assistant;
    assistant.role = "assistant";
    assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolUse,
        .tool_use_id = "bash-40k",
        .tool_name = "Bash",
        .tool_input_json = R"({"command":"cat mid.log"})",
    });
    messages.push_back(std::move(assistant));
    cc::services::api::Message result;
    result.role = "user";
    result.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolResult,
        .text = bash_result,
        .tool_use_id = "bash-40k",
    });
    messages.push_back(std::move(result));

    cc::tools::agent::AgentContentReplacementState state;
    auto replaced = cc::tools::agent::apply_agent_tool_result_budget(
        "finite-threshold",
        messages,
        state,
        cc::tools::agent::unbounded_tool_result_budget_names(registry.get_visible_definitions()),
        thresholds);
    EXPECT_EQ(replaced.newly_replaced, 1u);
    EXPECT_EQ(replaced.reapplied, 0u);
    EXPECT_NE(messages[1].content[0].text.find("<persisted-output>"), std::string::npos);
    EXPECT_LT(messages[1].content[0].text.size(), bash_result.size());
    auto persisted_path = root / "runtime" / "tool-results" / "finite-threshold-bash-40k.txt";
    ASSERT_TRUE(fs::exists(persisted_path));
    EXPECT_EQ(read_file(persisted_path), bash_result);

    auto record = cc::tools::agent_runtime::native_agent_store().get("finite-threshold");
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->sidechain_entries.size(), 1u);
    EXPECT_NE(record->sidechain_entries.front().find(R"("toolUseId":"bash-40k")"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolUsesGrowthBookToolResultThresholdOverrides) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_gb_threshold_override_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard ant_user_guard("USER_TYPE", "ant");
    EnvironmentGuard override_guard(
        "CLAUDE_INTERNAL_FC_OVERRIDES",
        R"({"tengu_satin_quoll":{"Bash":50000,"Read":1}})");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "gb-threshold",
        .agent_type = "general-purpose",
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
    });

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto thresholds = cc::tools::agent::tool_result_budget_thresholds(registry.get_visible_definitions());
    ASSERT_TRUE(thresholds.contains("bash"));
    EXPECT_EQ(thresholds["bash"], 50'000u);
    EXPECT_FALSE(thresholds.contains("read"));

    std::string bash_result(40'000, 'b');
    std::vector<cc::services::api::Message> messages;
    cc::services::api::Message assistant;
    assistant.role = "assistant";
    assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolUse,
        .tool_use_id = "bash-override-40k",
        .tool_name = "Bash",
        .tool_input_json = R"({"command":"cat mid.log"})",
    });
    messages.push_back(std::move(assistant));
    cc::services::api::Message result;
    result.role = "user";
    result.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolResult,
        .text = bash_result,
        .tool_use_id = "bash-override-40k",
    });
    messages.push_back(std::move(result));

    cc::tools::agent::AgentContentReplacementState state;
    auto replaced = cc::tools::agent::apply_agent_tool_result_budget(
        "gb-threshold",
        messages,
        state,
        cc::tools::agent::unbounded_tool_result_budget_names(registry.get_visible_definitions()),
        thresholds);
    EXPECT_EQ(replaced.newly_replaced, 0u);
    EXPECT_EQ(messages[1].content[0].text, bash_result);
    EXPECT_TRUE(state.seen_ids.contains("bash-override-40k"));
    EXPECT_FALSE(fs::exists(root / "runtime" / "tool-results" / "gb-threshold-bash-override-40k.txt"));

    auto record = cc::tools::agent_runtime::native_agent_store().get("gb-threshold");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->sidechain_entries.empty());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentToolUsesGrowthBookAggregateBudgetOverride) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_gb_aggregate_override_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard ant_user_guard("USER_TYPE", "ant");
    EnvironmentGuard override_guard("CLAUDE_INTERNAL_FC_OVERRIDES", R"({"tengu_hawthorn_window":10000})");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "gb-aggregate",
        .agent_type = "general-purpose",
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
    });

    EXPECT_EQ(cc::tools::agent::agent_per_message_budget_limit(), 10'000u);
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto thresholds = cc::tools::agent::tool_result_budget_thresholds(registry.get_visible_definitions());
    ASSERT_TRUE(thresholds.contains("grep"));
    EXPECT_EQ(thresholds["grep"], 20'000u);

    std::string larger_result(8'000, 'g');
    std::string smaller_result(7'000, 'h');
    std::vector<cc::services::api::Message> messages;
    cc::services::api::Message assistant;
    assistant.role = "assistant";
    assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolUse,
        .tool_use_id = "grep-8k",
        .tool_name = "Grep",
        .tool_input_json = R"({"pattern":"g"})",
    });
    assistant.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolUse,
        .tool_use_id = "grep-7k",
        .tool_name = "Grep",
        .tool_input_json = R"({"pattern":"h"})",
    });
    messages.push_back(std::move(assistant));
    cc::services::api::Message result;
    result.role = "user";
    result.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolResult,
        .text = larger_result,
        .tool_use_id = "grep-8k",
    });
    result.content.push_back(cc::services::api::ContentBlock{
        .type = cc::services::api::ContentBlockType::ToolResult,
        .text = smaller_result,
        .tool_use_id = "grep-7k",
    });
    messages.push_back(std::move(result));

    cc::tools::agent::AgentContentReplacementState state;
    auto replaced = cc::tools::agent::apply_agent_tool_result_budget(
        "gb-aggregate",
        messages,
        state,
        cc::tools::agent::unbounded_tool_result_budget_names(registry.get_visible_definitions()),
        thresholds);
    EXPECT_EQ(replaced.newly_replaced, 1u);
    EXPECT_NE(messages[1].content[0].text.find("<persisted-output>"), std::string::npos);
    EXPECT_EQ(messages[1].content[1].text, smaller_result);
    EXPECT_TRUE(fs::exists(root / "runtime" / "tool-results" / "gb-aggregate-grep-8k.txt"));
    EXPECT_FALSE(fs::exists(root / "runtime" / "tool-results" / "gb-aggregate-grep-7k.txt"));

    auto record = cc::tools::agent_runtime::native_agent_store().get("gb-aggregate");
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->sidechain_entries.size(), 1u);
    EXPECT_NE(record->sidechain_entries.front().find(R"("toolUseId":"grep-8k")"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeForkAddsDirectiveWorktreeNoticeAndMetadata) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_runtime_fork_worktree_test";
    fs::remove_all(root);
    fs::create_directories(root / "parent");
    fs::create_directories(root / "worktree");
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "fork-parent",
        .agent_type = "runtime",
        .cwd = (root / "parent").string(),
        .status = cc::tools::agent_runtime::NativeAgentStatus::Completed,
        .capabilities = {"Read", "Bash"},
        .transcript = {"user: parent context", "assistant: parent result"},
    });
    cc::tools::agent_runtime::native_agent_store().append_sidechain_message(
        "fork-parent",
        "assistant",
        R"([{"type":"text","text":"parent inspected file"},{"type":"tool_use","id":"fork-parent-tool-1","name":"Read","input":{"file_path":"README.md"}}])",
        "parent inspected file");
    cc::tools::agent_runtime::native_agent_store().append_sidechain_message(
        "fork-parent",
        "user",
        R"([{"type":"tool_result","tool_use_id":"fork-parent-tool-1","content":[{"type":"text","text":"README content"}]}])",
        "README content");
    auto parent_record = cc::tools::agent_runtime::native_agent_store().get("fork-parent");
    ASSERT_TRUE(parent_record.has_value());
    parent_record->sidechain_entries.push_back(
        R"({"type":"content-replacement","sessionId":"session-1","agentId":"fork-parent","replacements":[{"kind":"tool-result","toolUseId":"fork-parent-tool-1","replacement":"[persisted parent preview]"}]})");
    cc::tools::agent_runtime::native_agent_store().upsert(std::move(*parent_record));

    cc::tools::agent_runtime::AgentRuntimeConfig child_config{
        .agent_id = "fork-child",
        .working_dir = (root / "worktree").string(),
        .capabilities = {"Read"},
        .worktree_path = (root / "worktree").string(),
        .worktree_branch = "cc-agent-fork-child",
        .worktree_base_commit = "base-commit",
        .worktree_git_root = root.string(),
        .fork_directive = "Inspect only the parser migration",
        .allow_fork = true,
    };
    auto child = cc::tools::agent_runtime::fork_subagent("fork-parent", child_config);
    ASSERT_TRUE(child.has_value()) << child.error();
    EXPECT_EQ(*child, "fork-child");

    auto child_record = cc::tools::agent_runtime::native_agent_store().get("fork-child");
    ASSERT_TRUE(child_record.has_value());
    EXPECT_TRUE(std::ranges::contains(child_record->capabilities, "fork-subagent"));
    ASSERT_TRUE(child_record->worktree_path.has_value());
    EXPECT_EQ(*child_record->worktree_path, (root / "worktree").string());
    ASSERT_TRUE(child_record->worktree_branch.has_value());
    EXPECT_EQ(*child_record->worktree_branch, "cc-agent-fork-child");
    ASSERT_FALSE(child_record->transcript.empty());
    EXPECT_TRUE(std::ranges::any_of(child_record->transcript, [](const auto& line) {
        return line.find("<fork-boilerplate>") != std::string::npos &&
            line.find("Your directive: Inspect only the parser migration") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(child_record->transcript, [](const auto& line) {
        return line.find("translate them to your worktree root") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(child_record->transcript, [](const auto& line) {
        return line.find("[tool_use:Read]") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(child_record->transcript, [](const auto& line) {
        return line.find("tool_result: README content") != std::string::npos;
    }));
    ASSERT_GE(child_record->sidechain_entries.size(), 4u);
    ASSERT_TRUE(child_record->sidechain_jsonl_path.has_value());
    ASSERT_TRUE(fs::exists(*child_record->sidechain_jsonl_path));

    std::ifstream sidechain_in(*child_record->sidechain_jsonl_path);
    std::string sidechain_text(
        (std::istreambuf_iterator<char>(sidechain_in)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(sidechain_text.find(R"("agentId":"fork-child")"), std::string::npos);
    EXPECT_EQ(sidechain_text.find(R"("agentId":"fork-parent")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("type":"tool_use")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("id":"fork-parent-tool-1")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("name":"Read")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("type":"tool_result")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("tool_use_id":"fork-parent-tool-1")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("type":"content-replacement")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("replacement":"[persisted parent preview]")"), std::string::npos);
    EXPECT_NE(sidechain_text.find("Your directive: Inspect only the parser migration"), std::string::npos);
    auto child_resume_messages = cc::tools::agent::resume_messages_from_sidechain_entries(child_record->sidechain_entries);
    ASSERT_GE(child_resume_messages.size(), 3u);
    auto child_tool_result = std::ranges::find_if(child_resume_messages, [](const auto& message) {
        return message.role == "user" &&
            std::ranges::any_of(message.content, [](const auto& block) {
                return block.type == cc::services::api::ContentBlockType::ToolResult &&
                    block.tool_use_id == "fork-parent-tool-1" &&
                    block.text == "[persisted parent preview]";
            });
    });
    EXPECT_NE(child_tool_result, child_resume_messages.end());

    cc::tools::agent_runtime::AgentRuntimeConfig recursive_config{
        .agent_id = "fork-grandchild",
        .working_dir = (root / "worktree").string(),
        .capabilities = {"Read"},
        .fork_directive = "Try to fork recursively",
        .allow_fork = true,
    };
    auto recursive = cc::tools::agent_runtime::fork_subagent("fork-child", recursive_config);
    ASSERT_FALSE(recursive.has_value());
    EXPECT_NE(recursive.error().find("Fork is not available inside a forked worker"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored_child = cc::tools::agent_runtime::native_agent_store().get("fork-child");
    ASSERT_TRUE(restored_child.has_value());
    ASSERT_GE(restored_child->sidechain_entries.size(), 4u);
    EXPECT_TRUE(std::ranges::any_of(restored_child->transcript, [](const auto& line) {
        return line.find("[tool_use:Read]") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeForkAddsPlaceholderToolResultsForUnresolvedToolUses) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_runtime_fork_placeholder_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "fork-placeholder-parent",
        .agent_type = "runtime",
        .cwd = root.string(),
        .status = cc::tools::agent_runtime::NativeAgentStatus::Running,
        .capabilities = {"Read"},
        .transcript = {"user: parent context"},
    });
    cc::tools::agent_runtime::native_agent_store().append_sidechain_message(
        "fork-placeholder-parent",
        "assistant",
        R"([{"type":"text","text":"about to read"},{"type":"tool_use","id":"unresolved-read-1","name":"Read","input":{"file_path":"README.md"}}])",
        "about to read");

    cc::tools::agent_runtime::AgentRuntimeConfig child_config{
        .agent_id = "fork-placeholder-child",
        .working_dir = root.string(),
        .capabilities = {"Read"},
        .fork_directive = "Continue without waiting for the read result",
        .allow_fork = true,
    };
    auto child = cc::tools::agent_runtime::fork_subagent("fork-placeholder-parent", child_config);
    ASSERT_TRUE(child.has_value()) << child.error();

    auto child_record = cc::tools::agent_runtime::native_agent_store().get("fork-placeholder-child");
    ASSERT_TRUE(child_record.has_value());
    ASSERT_TRUE(child_record->sidechain_jsonl_path.has_value());
    std::ifstream sidechain_in(*child_record->sidechain_jsonl_path);
    std::string sidechain_text(
        (std::istreambuf_iterator<char>(sidechain_in)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(sidechain_text.find(R"("agentId":"fork-placeholder-child")"), std::string::npos);
    EXPECT_EQ(sidechain_text.find(R"("agentId":"fork-placeholder-parent")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("type":"tool_use")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("type":"tool_result")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"("tool_use_id":"unresolved-read-1")"), std::string::npos);
    EXPECT_NE(sidechain_text.find(R"(Fork started \u2014 processing in background)"), std::string::npos);
    EXPECT_NE(sidechain_text.find("Your directive: Continue without waiting for the read result"), std::string::npos);
    EXPECT_TRUE(std::ranges::any_of(child_record->transcript, [](const auto& line) {
        return line.find("tool_result: Fork started") != std::string::npos &&
            line.find("Your directive: Continue without waiting for the read result") != std::string::npos;
    }));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeResumeTouchesExistingWorktreeAndFallsBackWhenMissing) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_runtime_resume_worktree_test";
    fs::remove_all(root);
    fs::create_directories(root / "existing-worktree");
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
    std::error_code ec;
    fs::last_write_time(root / "existing-worktree", old_time, ec);
    ASSERT_FALSE(ec);
    auto before = fs::last_write_time(root / "existing-worktree");

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "resume-existing",
        .agent_type = "runtime",
        .cwd = (root / "existing-worktree").string(),
        .isolation = "worktree",
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .worktree_path = (root / "existing-worktree").string(),
        .worktree_branch = "cc-agent-resume-existing",
        .transcript = {"user: existing worktree"},
    });
    auto resumed_existing = cc::tools::agent_runtime::resume_agent("resume-existing");
    ASSERT_TRUE(resumed_existing.has_value()) << resumed_existing.error();
    auto after = fs::last_write_time(root / "existing-worktree");
    EXPECT_GT(after, before);
    auto existing_record = cc::tools::agent_runtime::native_agent_store().get("resume-existing");
    ASSERT_TRUE(existing_record.has_value());
    EXPECT_TRUE(existing_record->worktree_path.has_value());

    auto missing = root / "missing-worktree";
    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "resume-missing",
        .agent_type = "runtime",
        .cwd = missing.string(),
        .isolation = "worktree",
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .worktree_path = missing.string(),
        .worktree_branch = "cc-agent-resume-missing",
        .transcript = {"user: missing worktree"},
    });
    auto resumed_missing = cc::tools::agent_runtime::resume_agent("resume-missing");
    ASSERT_TRUE(resumed_missing.has_value()) << resumed_missing.error();
    EXPECT_TRUE(std::ranges::any_of(resumed_missing->transcript, [](const auto& line) {
        return line.find("falling back to parent cwd") != std::string::npos;
    }));
    auto missing_record = cc::tools::agent_runtime::native_agent_store().get("resume-missing");
    ASSERT_TRUE(missing_record.has_value());
    EXPECT_TRUE(missing_record->worktree_cleanup_performed);
    EXPECT_FALSE(missing_record->worktree_path.has_value());
    EXPECT_FALSE(missing_record->cwd.has_value());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeTracksLifecycleForkAndResume) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_runtime_lifecycle_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::AgentRuntimeConfig parent_config{
        .agent_id = "runtime-parent",
        .working_dir = root.string(),
        .capabilities = {"Read", "Bash"},
    };
    auto parent = cc::tools::agent_runtime::run_agent(parent_config);
    ASSERT_TRUE(parent.has_value()) << parent.error();
    EXPECT_EQ(parent->agent_id, "runtime-parent");
    EXPECT_EQ(parent->exit_code, 0);
    EXPECT_NE(parent->output.find(root.string()), std::string::npos);
    EXPECT_EQ(
        cc::tools::agent_runtime::get_agent_lifecycle("runtime-parent"),
        cc::tools::agent_runtime::AgentLifecycle::Completed);
    EXPECT_TRUE(fs::exists(root / "runtime" / "runtime-parent.json"));
    EXPECT_TRUE(fs::exists(root / "runtime" / "runtime-parent.transcript"));

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored_parent = cc::tools::agent_runtime::resume_agent("runtime-parent");
    ASSERT_TRUE(restored_parent.has_value()) << restored_parent.error();
    EXPECT_EQ(restored_parent->agent_id, "runtime-parent");
    EXPECT_FALSE(restored_parent->transcript.empty());
    EXPECT_EQ(
        cc::tools::agent_runtime::get_agent_lifecycle("runtime-parent"),
        cc::tools::agent_runtime::AgentLifecycle::Completed);

    cc::tools::agent_runtime::AgentRuntimeConfig child_config{
        .agent_id = "runtime-child",
        .working_dir = root.string(),
        .capabilities = {"Read"},
        .allow_fork = true,
    };
    auto child = cc::tools::agent_runtime::fork_subagent("runtime-parent", child_config);
    ASSERT_TRUE(child.has_value()) << child.error();
    EXPECT_EQ(*child, "runtime-child");

    auto child_record = cc::tools::agent_runtime::native_agent_store().get("runtime-child");
    ASSERT_TRUE(child_record.has_value());
    ASSERT_TRUE(child_record->parent_agent_id.has_value());
    EXPECT_EQ(*child_record->parent_agent_id, "runtime-parent");
    EXPECT_EQ(child_record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    EXPECT_EQ(
        cc::tools::agent_runtime::get_agent_lifecycle("runtime-child"),
        cc::tools::agent_runtime::AgentLifecycle::Starting);

    auto resumed = cc::tools::agent_runtime::resume_agent("runtime-child");
    ASSERT_TRUE(resumed.has_value()) << resumed.error();
    EXPECT_EQ(resumed->agent_id, "runtime-child");
    EXPECT_NE(resumed->output.find("queued"), std::string::npos);
    ASSERT_FALSE(resumed->transcript.empty());
    EXPECT_TRUE(std::ranges::any_of(resumed->transcript, [](const auto& line) {
        return line.find("runtime-parent") != std::string::npos;
    }));
    EXPECT_EQ(resumed->transcript.back(), "system: forked from runtime-parent");

    cc::tools::agent_runtime::AgentRuntimeConfig grandchild_config{
        .agent_id = "runtime-grandchild",
        .working_dir = root.string(),
        .capabilities = {"Read"},
        .allow_fork = true,
    };
    auto recursive_child = cc::tools::agent_runtime::fork_subagent("runtime-child", grandchild_config);
    ASSERT_FALSE(recursive_child.has_value());
    EXPECT_NE(recursive_child.error().find("Fork is not available inside a forked worker"), std::string::npos);

    ASSERT_FALSE(parent->transcript.empty());
    auto parent_record = cc::tools::agent_runtime::native_agent_store().get("runtime-parent");
    ASSERT_TRUE(parent_record.has_value());
    ASSERT_TRUE(parent_record->progress.has_value());
    EXPECT_DOUBLE_EQ(*parent_record->progress, 1.0);

    cc::tools::agent_runtime::native_agent_store().request_cancel("runtime-child", "test cancel");
    EXPECT_EQ(
        cc::tools::agent_runtime::get_agent_lifecycle("runtime-child"),
        cc::tools::agent_runtime::AgentLifecycle::Cancelled);
    auto cancelled = cc::tools::agent_runtime::resume_agent("runtime-child");
    ASSERT_TRUE(cancelled.has_value()) << cancelled.error();
    EXPECT_EQ(cancelled->exit_code, 130);
    ASSERT_TRUE(cancelled->error.has_value());
    EXPECT_EQ(*cancelled->error, "test cancel");

    child_config.allow_fork = false;
    auto denied = cc::tools::agent_runtime::fork_subagent("runtime-parent", child_config);
    EXPECT_FALSE(denied.has_value());

    auto missing = cc::tools::agent_runtime::resume_agent("missing-agent");
    EXPECT_FALSE(missing.has_value());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeSendMessageDeliversToBackgroundAgentQueue) {
    auto root = fs::temp_directory_path() / "cc_repl_send_message_runtime_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::AgentTool tool;
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async",
      "prompt": "Wait for coordination",
      "name": "message-target",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value());
    ASSERT_FALSE(started->is_error);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "message-target",
      "content": "Review the migration diff",
      "priority": "high"
    })"));

    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("Delivered message"), std::string::npos);
    EXPECT_NE(delivered->content.front().text.find("message-target"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("message-target");
    ASSERT_TRUE(record.has_value());
    ASSERT_FALSE(record->transcript.empty());
    EXPECT_NE(record->transcript.back().find("Review the migration diff"), std::string::npos);
    ASSERT_EQ(record->pending_messages.size(), 1u);
    EXPECT_NE(record->pending_messages.front().find("[Message from team-lead priority=high]"), std::string::npos);
    EXPECT_NE(record->pending_messages.front().find("Review the migration diff"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored = cc::tools::agent_runtime::native_agent_store().get("message-target");
    ASSERT_TRUE(restored.has_value());
    ASSERT_FALSE(restored->transcript.empty());
    EXPECT_NE(restored->transcript.back().find("Review the migration diff"), std::string::npos);
    ASSERT_EQ(restored->pending_messages.size(), 1u);
    EXPECT_NE(restored->pending_messages.front().find("Review the migration diff"), std::string::npos);

    auto pending = cc::tools::agent_runtime::native_agent_store().take_pending_messages("message-target");
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_NE(pending.front().find("priority=high"), std::string::npos);
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto consumed = cc::tools::agent_runtime::native_agent_store().get("message-target");
    ASSERT_TRUE(consumed.has_value());
    EXPECT_TRUE(consumed->pending_messages.empty());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeSendMessageQueuesStoppedNativeAgentForResume) {
    auto root = fs::temp_directory_path() / "cc_repl_send_message_resume_runtime_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentUnsetGuard anthropic_key_guard("ANTHROPIC_API_KEY");
    EnvironmentUnsetGuard claude_token_guard("CLAUDE_AUTH_TOKEN");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "resume-target",
        .agent_type = "general-purpose",
        .description = "Stopped native agent",
        .name = "stopped-agent",
        .cwd = root.string(),
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Completed,
        .output = "old completed output",
        .capabilities = {"Read"},
        .transcript = {"user: original prompt", "assistant: old completed output"},
        .progress = 1.0,
        .notification_delivered = true,
    });

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "stopped-agent",
      "content": "Resume with this follow-up",
      "priority": "normal"
    })"));

    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("queued for background resume"), std::string::npos);
    EXPECT_NE(delivered->content.front().text.find("background resume deferred: no Anthropic API credentials"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("resume-target");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    EXPECT_FALSE(record->output.has_value());
    EXPECT_FALSE(record->error.has_value());
    ASSERT_TRUE(record->progress.has_value());
    EXPECT_DOUBLE_EQ(*record->progress, 0.0);
    EXPECT_FALSE(record->cancel_requested);
    EXPECT_FALSE(record->notification_delivered);
    ASSERT_EQ(record->pending_messages.size(), 1u);
    EXPECT_NE(record->pending_messages.front().find("Resume with this follow-up"), std::string::npos);
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("resume requested from pending message") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(record->transcript, [](const auto& line) {
        return line.find("Resume with this follow-up") != std::string::npos;
    }));

    auto notifications = cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications();
    EXPECT_TRUE(notifications.empty());

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored = cc::tools::agent_runtime::native_agent_store().get("resume-target");
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    ASSERT_EQ(restored->pending_messages.size(), 1u);
    EXPECT_NE(restored->pending_messages.front().find("Resume with this follow-up"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeRemoteTriggerUsesTypedValidationAndFallbackCommand) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto blocked = registry.execute("remote_trigger", cc::core::ToolInput::from_json(R"({
      "target": "http://127.0.0.1:65535/hook",
      "message": "should be blocked",
      "params": {"branch": "main"}
    })"));

    ASSERT_TRUE(blocked.has_value());
    ASSERT_TRUE(blocked->is_error);
    ASSERT_FALSE(blocked->content.empty());
    EXPECT_NE(blocked->content.front().text.find("Cannot trigger internal network addresses"), std::string::npos);

    EnvironmentGuard fallback_guard("CC_REPL_REMOTE_TRIGGER_COMMAND", "printf remote-fallback");
    auto fallback = registry.execute("remote_trigger", cc::core::ToolInput::from_json(R"({
      "payload": "fallback payload"
    })"));

    ASSERT_TRUE(fallback.has_value());
    EXPECT_FALSE(fallback->is_error);
    ASSERT_FALSE(fallback->content.empty());
    EXPECT_NE(fallback->content.front().text.find("remote-fallback"), std::string::npos);
}

TEST(Tools, RuntimeTeamCreateRegistersMembersAndSharedTasks) {
    auto root = fs::temp_directory_path() / "cc_repl_team_runtime_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto created = registry.execute("team_create", cc::core::ToolInput::from_json(R"({
      "team_id": "runtime-team-members",
      "team_name": "Runtime Team Members",
      "members": [
        {"agent_id": "team-researcher", "role": "worker"},
        {"agent_id": "team-reviewer", "role": "reviewer"}
      ],
      "task_list": [
        {"id": "task-1", "description": "Inspect migration parity", "assigned_to": "team-researcher"}
      ]
    })"));

    ASSERT_TRUE(created.has_value());
    ASSERT_FALSE(created->is_error);
    ASSERT_FALSE(created->content.empty());
    auto created_json = cc::utils::json::parse(created->content.front().text);
    ASSERT_TRUE(created_json.has_value());
    auto created_root = created_json->root();
    EXPECT_EQ(created_root.get_string("team_name"), "Runtime Team Members");
    EXPECT_EQ(created_root.get_string("team_id"), "runtime-team-members");
    EXPECT_EQ(created_root.get_string("lead_agent_id"), "team-lead@Runtime Team Members");
    EXPECT_EQ(created_root.get_int("members"), 2);
    EXPECT_EQ(created_root.get_int("tasks"), 1);
    EXPECT_EQ(created_root.get_int("member_inboxes_initialized"), 2);
    EXPECT_EQ(created_root.get_int("task_assignments_enqueued"), 1);
    EXPECT_TRUE(created_root.get("team_config_written").as_bool());
    EXPECT_TRUE(created_root.get("task_list_written").as_bool());

    auto record = cc::tools::agent_runtime::native_agent_store().get("team-reviewer");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->team_name.has_value());
    EXPECT_EQ(*record->team_name, "Runtime Team Members");

    auto team = cc::tools::global_team_store().get("runtime-team-members");
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));
    ASSERT_EQ((*team)->task_list.size(), 1u);
    ASSERT_TRUE((*team)->task_list.front().assigned_to.has_value());
    EXPECT_EQ(*(*team)->task_list.front().assigned_to, "team-researcher");
    auto member = std::ranges::find_if((*team)->members, [](const auto& candidate) {
        return candidate.agent_id == "team-researcher";
    });
    ASSERT_NE(member, (*team)->members.end());
    EXPECT_EQ(member->status, cc::tools::MemberStatus::Working);
    ASSERT_TRUE(member->current_task.has_value());
    EXPECT_EQ(*member->current_task, "task-1");

    auto researcher = cc::tools::agent_runtime::native_agent_store().get("team-researcher");
    ASSERT_TRUE(researcher.has_value());
    EXPECT_EQ(researcher->status, cc::tools::agent_runtime::NativeAgentStatus::Running);
    ASSERT_EQ(researcher->pending_messages.size(), 1u);
    EXPECT_NE(researcher->pending_messages.front().find("[Team task task-1 assigned by Runtime Team Members]"), std::string::npos);
    EXPECT_NE(researcher->pending_messages.front().find("Inspect migration parity"), std::string::npos);
    EXPECT_TRUE(std::ranges::any_of(researcher->transcript, [](const auto& entry) {
        return entry.find("team task assigned task-1: Inspect migration parity") != std::string::npos;
    }));
    EXPECT_TRUE(fs::exists(root / "teams" / "runtime-team-members.json"));
    auto team_dir = root / "teams" / "runtime-team-members";
    EXPECT_EQ(created_root.get_string("team_dir"), team_dir.string());
    EXPECT_EQ(created_root.get_string("team_file_path"), (team_dir / "config.json").string());
    EXPECT_TRUE(fs::exists(team_dir / "config.json"));
    EXPECT_TRUE(fs::exists(team_dir / "inboxes" / "team-researcher.json"));
    EXPECT_TRUE(fs::exists(team_dir / "inboxes" / "team-reviewer.json"));
    ASSERT_TRUE(fs::exists(team_dir / "tasks.json"));
    std::ifstream tasks_in(team_dir / "tasks.json");
    std::string tasks_text(
        (std::istreambuf_iterator<char>(tasks_in)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(tasks_text.find(R"("id":"task-1")"), std::string::npos);
    EXPECT_NE(tasks_text.find(R"("assigned_to":"team-researcher")"), std::string::npos);

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    auto restored_team = cc::tools::global_team_store().get("runtime-team-members");
    ASSERT_TRUE(restored_team.has_value()) << std::string(cc::tools::format_error(restored_team.error()));
    ASSERT_EQ((*restored_team)->task_list.size(), 1u);
    EXPECT_EQ((*restored_team)->task_list.front().id, "task-1");
    auto restored_member = std::ranges::find_if((*restored_team)->members, [](const auto& candidate) {
        return candidate.agent_id == "team-researcher";
    });
    ASSERT_NE(restored_member, (*restored_team)->members.end());
    EXPECT_EQ(restored_member->status, cc::tools::MemberStatus::Working);
    auto restored_researcher = cc::tools::agent_runtime::native_agent_store().get("team-researcher");
    ASSERT_TRUE(restored_researcher.has_value());
    ASSERT_EQ(restored_researcher->pending_messages.size(), 1u);
    EXPECT_NE(restored_researcher->pending_messages.front().find("Inspect migration parity"), std::string::npos);

    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "team-reviewer",
      "content": "Review team output"
    })"));

    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    EXPECT_NE(delivered->content.front().text.find("team-reviewer"), std::string::npos);

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTeamCreateCanStartNativeAgentsAndResumeThemWithSendMessage) {
    auto root = fs::temp_directory_path() / "cc_repl_team_create_native_start_test";
    fs::remove_all(root);
    fs::create_directories(root);
    LocalSlowAnthropicStreamServer server(std::chrono::milliseconds(1));
    ASSERT_TRUE(server.valid());
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "team-create-native-start-model");
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto created = registry.execute("team_create", cc::core::ToolInput::from_json(R"({
      "team_id": "native-start-team-id",
      "team_name": "Native Start Team",
      "start_native_agents": true,
      "members": [
        {"agent_id": "planner@native-start-team", "prompt": "Plan the native team launch", "subagent_type": "general-purpose"},
        {"agent_id": "reviewer@native-start-team", "subagent_type": "general-purpose"}
      ],
      "task_list": [
        {"id": "review-task", "description": "Review the native team launch", "assigned_to": "reviewer@native-start-team"}
      ]
    })"));

    ASSERT_TRUE(created.has_value());
    ASSERT_FALSE(created->is_error);
    ASSERT_FALSE(created->content.empty());
    auto created_json = cc::utils::json::parse(created->content.front().text);
    ASSERT_TRUE(created_json.has_value());
    auto created_root = created_json->root();
    EXPECT_EQ(created_root.get_string("team_name"), "Native Start Team");
    EXPECT_EQ(created_root.get_string("team_id"), "native-start-team-id");
    EXPECT_EQ(created_root.get_string("lead_agent_id"), "team-lead@Native Start Team");
    EXPECT_EQ(created_root.get_int("native_agents_started"), 2);
    EXPECT_TRUE(created_root.get("team_config_written").as_bool());
    EXPECT_TRUE(fs::exists(created_root.get_string("team_file_path")));
    ASSERT_TRUE(server.wait_for_request_count(2));
    EXPECT_TRUE(wait_for_native_agent_status(
        "planner@native-start-team",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));
    EXPECT_TRUE(wait_for_native_agent_status(
        "reviewer@native-start-team",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));

    auto planner = cc::tools::agent_runtime::native_agent_store().get("planner@native-start-team");
    ASSERT_TRUE(planner.has_value());
    ASSERT_TRUE(planner->team_name.has_value());
    EXPECT_EQ(*planner->team_name, "Native Start Team");
    ASSERT_TRUE(planner->output_file_path.has_value());
    EXPECT_TRUE(fs::is_symlink(*planner->output_file_path));
    EXPECT_TRUE(std::ranges::any_of(planner->transcript, [](const auto& line) {
        return line.find("Plan the native team launch") != std::string::npos;
    }));

    auto reviewer = cc::tools::agent_runtime::native_agent_store().get("reviewer@native-start-team");
    ASSERT_TRUE(reviewer.has_value());
    EXPECT_TRUE(std::ranges::any_of(reviewer->transcript, [](const auto& line) {
        return line.find("[Team task review-task assigned by Native Start Team]") != std::string::npos;
    }));
    EXPECT_EQ(reviewer->output, std::optional<std::string>{"late stream response"});

    auto wait_for_member_done = [](std::string_view member_id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            auto team = cc::tools::global_team_store().get("native-start-team-id");
            if (team) {
                auto member = std::ranges::find_if((*team)->members, [&](const auto& candidate) {
                    return candidate.agent_id == member_id;
                });
                if (member != (*team)->members.end() &&
                    member->status == cc::tools::MemberStatus::Done &&
                    member->last_result &&
                    member->last_result->find("late stream response") != std::string::npos) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    EXPECT_TRUE(wait_for_member_done("planner@native-start-team"));
    EXPECT_TRUE(wait_for_member_done("reviewer@native-start-team"));

    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "reviewer@native-start-team",
      "content": "Continue reviewing the launched team path",
      "from_agent": "team-lead"
    })"));
    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("background resume started"), std::string::npos);
    ASSERT_TRUE(server.wait_for_request_count(3));
    EXPECT_TRUE(wait_for_native_agent_status(
        "reviewer@native-start-team",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));
    reviewer = cc::tools::agent_runtime::native_agent_store().get("reviewer@native-start-team");
    ASSERT_TRUE(reviewer.has_value());
    EXPECT_TRUE(std::ranges::any_of(reviewer->transcript, [](const auto& line) {
        return line.find("Continue reviewing the launched team path") != std::string::npos;
    }));

    const auto planner_output_file = fs::path{*planner->output_file_path};
    const auto reviewer_output_file = fs::path{*reviewer->output_file_path};
    auto deleted = registry.execute("team_delete", cc::core::ToolInput::from_json(R"({
      "team_id": "native-start-team-id"
    })"));
    ASSERT_TRUE(deleted.has_value());
    ASSERT_FALSE(deleted->is_error);
    ASSERT_FALSE(deleted->content.empty());
    EXPECT_NE(deleted->content.front().text.find("Deleted team Native Start Team (native-start-team-id)"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "teams" / "native-start-team-id.json"));
    EXPECT_FALSE(fs::exists(root / "teams" / "native-start-team"));
    EXPECT_FALSE(fs::exists(planner_output_file) || fs::is_symlink(planner_output_file));
    EXPECT_FALSE(fs::exists(reviewer_output_file) || fs::is_symlink(reviewer_output_file));

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTeamCreateStartedNativeTeammateResumesAfterRegistryRestart) {
    auto root = fs::temp_directory_path() / "cc_repl_team_create_restart_resume_test";
    { std::error_code ec; fs::remove_all(root, ec); }
    fs::create_directories(root);
    LocalSlowAnthropicStreamServer server(std::chrono::milliseconds(1));
    ASSERT_TRUE(server.valid());
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "team-create-restart-resume-model");
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    {
        cc::core::ToolRegistry initial_registry;
        cc::tools::register_runtime_tools(initial_registry);
        auto created = initial_registry.execute("team_create", cc::core::ToolInput::from_json(R"({
          "team_id": "restart-resume-team-id",
          "team_name": "Restart Resume Team",
          "start_native_agents": true,
          "members": [
            {"agent_id": "reviewer@restart-resume-team", "prompt": "Run the initial restart-resume task", "subagent_type": "general-purpose"}
          ]
        })"));
        ASSERT_TRUE(created.has_value());
        ASSERT_FALSE(created->is_error);
        ASSERT_TRUE(server.wait_for_request_count(1));
        ASSERT_TRUE(wait_for_native_agent_status(
            "reviewer@restart-resume-team",
            cc::tools::agent_runtime::NativeAgentStatus::Completed,
            std::chrono::seconds(3)));
    }

    auto completed = cc::tools::agent_runtime::native_agent_store().get("reviewer@restart-resume-team");
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->output, std::optional<std::string>{"late stream response"});
    ASSERT_TRUE(completed->team_name.has_value());
    EXPECT_EQ(*completed->team_name, "Restart Resume Team");
    EXPECT_FALSE(completed->name.has_value());

    // Wait for the detached background thread to finish update_teammate_completion_status()
    // after mark_completed(). The thread holds references to team store data.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry restarted_registry;
    cc::tools::register_runtime_tools(restarted_registry);
    auto delivered = restarted_registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "reviewer",
      "team_name": "Restart Resume Team",
      "content": "Continue after a fresh runtime registry",
      "summary": "restart resume follow-up"
    })"));
    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("reviewer@restart-resume-team"), std::string::npos);
    EXPECT_NE(delivered->content.front().text.find("background resume started"), std::string::npos);
    ASSERT_TRUE(server.wait_for_request_count(2));
    ASSERT_TRUE(wait_for_native_agent_status(
        "reviewer@restart-resume-team",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));

    auto resumed = cc::tools::agent_runtime::native_agent_store().get("reviewer@restart-resume-team");
    ASSERT_TRUE(resumed.has_value());
    EXPECT_TRUE(resumed->pending_messages.empty());
    EXPECT_TRUE(std::ranges::any_of(resumed->transcript, [](const auto& line) {
        return line.find("Continue after a fresh runtime registry") != std::string::npos;
    }));
    auto last_request = server.last_body();
    ASSERT_TRUE(last_request.has_value());
    EXPECT_NE(last_request->find("Continue after a fresh runtime registry"), std::string::npos);

    auto inbox = cc::utils::read_inbox("reviewer", std::optional<std::string_view>{"Restart Resume Team"});
    ASSERT_TRUE(inbox.has_value()) << inbox.error();
    ASSERT_EQ(inbox->size(), 1u);
    EXPECT_EQ(inbox->front().text, "Continue after a fresh runtime registry");
    ASSERT_TRUE(inbox->front().summary.has_value());
    EXPECT_EQ(*inbox->front().summary, "restart resume follow-up");

    auto team = [&]() -> std::expected<cc::tools::Team*, cc::tools::TeamError> {
        for (int i = 0; i < 50; ++i) {
            auto t = cc::tools::global_team_store().get("restart-resume-team-id");
            if (t.has_value()) return t;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return cc::tools::global_team_store().get("restart-resume-team-id");
    }();
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));
    auto member = std::ranges::find_if((*team)->members, [](const auto& candidate) {
        return candidate.agent_id == "reviewer@restart-resume-team";
    });
    ASSERT_NE(member, (*team)->members.end());
    EXPECT_EQ(member->status, cc::tools::MemberStatus::Done);
    ASSERT_TRUE(member->last_result.has_value());
    EXPECT_NE(member->last_result->find("late stream response"), std::string::npos);

    // Allow detached background threads to exit before destroying the local HTTP server.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    { std::error_code ec; fs::remove_all(root, ec); }
}

TEST(Tools, RuntimeTeamCreateStartsNativeAgentsWithWorktreeIsolation) {
    if (std::system("git --version >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "git is required for team worktree isolation";
    }

    auto root = fs::temp_directory_path() / "cc_repl_team_create_worktree_test";
    fs::remove_all(root);
    fs::create_directories(root);
    {
        std::ofstream readme(root / "README.md");
        readme << "team worktree isolation\n";
    }
    ASSERT_EQ(std::system(std::format("git -C \"{}\" init -q --template=", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" config user.email test@example.com", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" config user.name Test", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" add README.md", root.string()).c_str()), 0);
    ASSERT_EQ(std::system(std::format("git -C \"{}\" commit -q --no-verify -m init", root.string()).c_str()), 0);

    LocalPerTurnBashCommandAnthropicServer server(
        "printf team-worktree > team_member_marker.txt; pwd",
        "team worktree complete");
    ASSERT_TRUE(server.valid());
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "team-worktree-test-model");
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto created = registry.execute("team_create", cc::core::ToolInput::from_json(std::format(R"({{
      "team_id": "worktree-team-id",
      "team_name": "Worktree Team",
      "start_native_agents": true,
      "cwd": "{}",
      "isolation": "worktree",
      "mode": "acceptEdits",
      "members": [
        {{"agent_id": "alpha@worktree-team", "prompt": "Write the worktree marker", "subagent_type": "general-purpose"}},
        {{"agent_id": "beta@worktree-team", "prompt": "Write the worktree marker", "subagent_type": "general-purpose"}}
      ]
    }})", cc::tools::agent::json_escape_string(root.string()))));

    ASSERT_TRUE(created.has_value());
    ASSERT_FALSE(created->is_error);
    ASSERT_FALSE(created->content.empty());
    auto created_json = cc::utils::json::parse(created->content.front().text);
    ASSERT_TRUE(created_json.has_value());
    auto created_root = created_json->root();
    EXPECT_EQ(created_root.get_string("team_name"), "Worktree Team");
    EXPECT_EQ(created_root.get_int("native_agents_started"), 2);

    ASSERT_TRUE(server.wait_for_request_count(4, std::chrono::seconds(5)));
    EXPECT_TRUE(wait_for_native_agent_status(
        "alpha@worktree-team",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));
    EXPECT_TRUE(wait_for_native_agent_status(
        "beta@worktree-team",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));

    auto alpha = cc::tools::agent_runtime::native_agent_store().get("alpha@worktree-team");
    auto beta = cc::tools::agent_runtime::native_agent_store().get("beta@worktree-team");
    ASSERT_TRUE(alpha.has_value());
    ASSERT_TRUE(beta.has_value());
    ASSERT_TRUE(alpha->worktree_path.has_value());
    ASSERT_TRUE(beta->worktree_path.has_value());
    ASSERT_TRUE(alpha->worktree_branch.has_value());
    ASSERT_TRUE(beta->worktree_branch.has_value());
    ASSERT_TRUE(alpha->cwd.has_value());
    ASSERT_TRUE(beta->cwd.has_value());
    ASSERT_TRUE(alpha->isolation.has_value());
    ASSERT_TRUE(beta->isolation.has_value());
    ASSERT_TRUE(alpha->mode.has_value());
    ASSERT_TRUE(beta->mode.has_value());
    ASSERT_TRUE(alpha->worktree_git_root.has_value());
    ASSERT_TRUE(beta->worktree_git_root.has_value());
    const auto alpha_path = fs::path{*alpha->worktree_path};
    const auto beta_path = fs::path{*beta->worktree_path};
    EXPECT_NE(alpha_path, beta_path);
    EXPECT_EQ(alpha_path, fs::weakly_canonical(root) / ".claude" / "worktrees" / "alpha_worktree-team");
    EXPECT_EQ(beta_path, fs::weakly_canonical(root) / ".claude" / "worktrees" / "beta_worktree-team");
    EXPECT_EQ(*alpha->cwd, alpha_path.string());
    EXPECT_EQ(*beta->cwd, beta_path.string());
    EXPECT_EQ(*alpha->isolation, "worktree");
    EXPECT_EQ(*beta->isolation, "worktree");
    EXPECT_EQ(*alpha->mode, "acceptEdits");
    EXPECT_EQ(*beta->mode, "acceptEdits");
    EXPECT_EQ(*alpha->worktree_git_root, fs::weakly_canonical(root).string());
    EXPECT_EQ(*beta->worktree_git_root, fs::weakly_canonical(root).string());
    const auto config_text = read_file(root / "teams" / "worktree-team" / "config.json");
    EXPECT_NE(config_text.find(R"("agentId":"alpha@worktree-team")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("agentId":"beta@worktree-team")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("tmuxPaneId":"in-process")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("backendType":"in-process")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("mode":"acceptEdits")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("cwd":")" + alpha_path.string() + R"(")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("cwd":")" + beta_path.string() + R"(")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("worktreePath":")" + alpha_path.string() + R"(")"), std::string::npos);
    EXPECT_NE(config_text.find(R"("worktreePath":")" + beta_path.string() + R"(")"), std::string::npos);
    EXPECT_TRUE(fs::exists(alpha_path / ".git"));
    EXPECT_TRUE(fs::exists(beta_path / ".git"));
    EXPECT_EQ(read_file(alpha_path / "team_member_marker.txt"), "team-worktree");
    EXPECT_EQ(read_file(beta_path / "team_member_marker.txt"), "team-worktree");
    EXPECT_FALSE(fs::exists(root / "team_member_marker.txt"));
    EXPECT_FALSE(alpha->worktree_cleanup_performed);
    EXPECT_FALSE(beta->worktree_cleanup_performed);
    EXPECT_TRUE(std::ranges::any_of(alpha->transcript, [&](const auto& entry) {
        return entry.find(alpha_path.string()) != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(beta->transcript, [&](const auto& entry) {
        return entry.find(beta_path.string()) != std::string::npos;
    }));

    auto deleted = registry.execute("team_delete", cc::core::ToolInput::from_json(R"({
      "team_id": "worktree-team-id"
    })"));
    ASSERT_TRUE(deleted.has_value());
    ASSERT_FALSE(deleted->is_error);
    ASSERT_FALSE(deleted->content.empty());
    EXPECT_NE(deleted->content.front().text.find("worktree_cleanup_attempts: 2"), std::string::npos);
    EXPECT_NE(deleted->content.front().text.find("worktrees_retained: 2"), std::string::npos);

    auto remove_worktree = [&](const fs::path& path, std::string_view branch) {
        (void)std::system(std::format(
            "git -C {} worktree remove --force {} >/dev/null 2>&1",
            shell_quote_for_test(root.string()),
            shell_quote_for_test(path.string())).c_str());
        (void)std::system(std::format(
            "git -C {} branch -D {} >/dev/null 2>&1",
            shell_quote_for_test(root.string()),
            shell_quote_for_test(branch)).c_str());
    };
    remove_worktree(alpha_path, *alpha->worktree_branch);
    remove_worktree(beta_path, *beta->worktree_branch);
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    { std::error_code ec; fs::remove_all(root, ec); }
}

TEST(Tools, AgentToolBackgroundAgentCwdIsScopedPerToolWithoutChangingProcessCwd) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_cwd_isolation_test";
    fs::remove_all(root);
    fs::create_directories(root / "agent-a");
    fs::create_directories(root / "agent-b");
    const auto original_cwd = fs::current_path();
    LocalPerTurnBashPwdAnthropicServer server;
    ASSERT_TRUE(server.valid());
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "cwd-isolation-test-model");
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::tools::AgentTool tool({}, 0, &registry);

    auto first = tool.execute(cc::core::ToolInput::from_json(std::format(R"({{
      "description": "Run pwd in agent A",
      "prompt": "Run pwd",
      "agent_id": "cwd-agent-a",
      "run_in_background": true,
      "cwd": "{}"
    }})", cc::tools::agent::json_escape_string((root / "agent-a").string()))));
    ASSERT_TRUE(first.has_value()) << first.error().format();
    ASSERT_FALSE(first->is_error);

    auto second = tool.execute(cc::core::ToolInput::from_json(std::format(R"({{
      "description": "Run pwd in agent B",
      "prompt": "Run pwd",
      "agent_id": "cwd-agent-b",
      "run_in_background": true,
      "cwd": "{}"
    }})", cc::tools::agent::json_escape_string((root / "agent-b").string()))));
    ASSERT_TRUE(second.has_value()) << second.error().format();
    ASSERT_FALSE(second->is_error);

    ASSERT_TRUE(server.wait_for_request_count(4));
    EXPECT_TRUE(wait_for_native_agent_status(
        "cwd-agent-a",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));
    EXPECT_TRUE(wait_for_native_agent_status(
        "cwd-agent-b",
        cc::tools::agent_runtime::NativeAgentStatus::Completed,
        std::chrono::seconds(3)));

    auto agent_a = cc::tools::agent_runtime::native_agent_store().get("cwd-agent-a");
    auto agent_b = cc::tools::agent_runtime::native_agent_store().get("cwd-agent-b");
    ASSERT_TRUE(agent_a.has_value());
    ASSERT_TRUE(agent_b.has_value());
    EXPECT_TRUE(std::ranges::any_of(agent_a->transcript, [&](const auto& entry) {
        return entry.find((root / "agent-a").string()) != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(agent_b->transcript, [&](const auto& entry) {
        return entry.find((root / "agent-b").string()) != std::string::npos;
    }));

    std::error_code cwd_error;
    const auto cwd_after_agents = fs::current_path(cwd_error);
    EXPECT_FALSE(cwd_error);
    if (!cwd_error) {
        EXPECT_EQ(cwd_after_agents, original_cwd);
    }
    fs::current_path(original_cwd, cwd_error);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, StandaloneTeamCreateAndDeleteDelegateToRuntimeTeamStore) {
    auto root = fs::temp_directory_path() / "cc_repl_standalone_team_tools_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::team_create::TeamCreateTool create_tool;
    auto created = create_tool.execute(cc::core::ToolInput::from_json(R"({
      "team_id": "standalone-team-id",
      "team_name": "Standalone Team",
      "members": [
        {"agent_id": "standalone-worker", "role": "worker"}
      ],
      "task_list": [
        {"id": "standalone-task", "description": "Exercise standalone team tool", "assigned_to": "standalone-worker"}
      ]
    })"));

    ASSERT_TRUE(created.has_value()) << created.error().format();
    ASSERT_FALSE(created->is_error);
    ASSERT_FALSE(created->content.empty());
    auto created_json = cc::utils::json::parse(created->content.front().text);
    ASSERT_TRUE(created_json.has_value());
    auto created_root = created_json->root();
    EXPECT_EQ(created_root.get_string("team_name"), "Standalone Team");
    EXPECT_EQ(created_root.get_string("team_id"), "standalone-team-id");
    EXPECT_EQ(created_root.get_string("lead_agent_id"), "team-lead@Standalone Team");
    EXPECT_TRUE(fs::exists(root / "teams" / "standalone-team-id.json"));
    EXPECT_TRUE(fs::exists(created_root.get_string("team_file_path")));
    EXPECT_TRUE(fs::exists(root / "teams" / "standalone-team" / "inboxes" / "standalone-worker.json"));
    EXPECT_TRUE(fs::exists(root / "teams" / "standalone-team" / "tasks.json"));

    auto team = cc::tools::global_team_store().get("standalone-team-id");
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));
    ASSERT_EQ((*team)->members.size(), 1u);
    EXPECT_EQ((*team)->members.front().agent_id, "standalone-worker");
    ASSERT_EQ((*team)->task_list.size(), 1u);
    ASSERT_TRUE((*team)->task_list.front().assigned_to.has_value());
    EXPECT_EQ(*(*team)->task_list.front().assigned_to, "standalone-worker");

    cc::tools::team_delete::TeamDeleteTool delete_tool;
    auto deleted = delete_tool.execute(cc::core::ToolInput::from_json(R"({
      "team_id": "standalone-team-id"
    })"));

    ASSERT_TRUE(deleted.has_value()) << deleted.error().format();
    ASSERT_FALSE(deleted->is_error);
    ASSERT_FALSE(deleted->content.empty());
    EXPECT_NE(deleted->content.front().text.find("Deleted team Standalone Team (standalone-team-id)"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "teams" / "standalone-team-id.json"));
    EXPECT_FALSE(fs::exists(root / "teams" / "standalone-team"));

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeTeamDeleteCancelsNativeTeammatesAndCleansArtifacts) {
    auto root = fs::temp_directory_path() / "cc_repl_team_delete_cleanup_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard backend_guard("CC_REPL_TEAMMATE_BACKEND", "in-process");
    cc::utils::swarm_backends::BackendRegistry::reset();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto created = registry.execute("team_create", cc::core::ToolInput::from_json(R"({
      "team_id": "cleanup-team-id",
      "team_name": "cleanup-team",
      "members": [
        {"agent_id": "reviewer@cleanup-team", "role": "reviewer"}
      ]
    })"));
    ASSERT_TRUE(created.has_value());
    ASSERT_FALSE(created->is_error);

    auto executor = cc::utils::swarm_backends::BackendRegistry::get_teammate_executor(true);
    cc::utils::swarm_backends::TeammateSpawnConfig spawn_config{
        .name = "reviewer",
        .team_name = "cleanup-team",
        .color = std::nullopt,
        .plan_mode_required = false,
        .permission_mode = std::nullopt,
        .agent_type = std::nullopt,
        .prompt = "Review cleanup behavior",
        .cwd = root.string(),
        .model = std::nullopt,
        .system_prompt = std::nullopt,
        .system_prompt_mode = "default",
        .worktree_path = std::nullopt,
        .parent_session_id = {},
        .permissions = {},
        .allow_permission_prompts = false,
    };
    auto spawned = executor->spawn(spawn_config);
    ASSERT_TRUE(spawned.success) << spawned.error.value_or("spawn failed");
    EXPECT_TRUE(executor->is_active("reviewer@cleanup-team"));

    cc::tools::agent_runtime::NativeAgentRecord record{};
    record.agent_id = "reviewer@cleanup-team";
    record.agent_type = "reviewer";
    record.name = "reviewer";
    record.team_name = "cleanup-team";
    record.cwd = root.string();
    record.background = true;
    record.status = cc::tools::agent_runtime::NativeAgentStatus::Running;
    record.teammate_backend = "in-process";
    record.teammate_task_id = spawned.task_id;
    cc::tools::agent_runtime::native_agent_store().upsert(std::move(record));
    cc::tools::agent_runtime::native_agent_store().set_worktree_metadata(
        "reviewer@cleanup-team",
        (root / "missing-worktree").string(),
        "cc-agent-reviewer",
        "base",
        root.string());
    auto artifact_record = cc::tools::agent_runtime::native_agent_store().get("reviewer@cleanup-team");
    ASSERT_TRUE(artifact_record.has_value());
    ASSERT_TRUE(artifact_record->output_file_path.has_value());
    ASSERT_TRUE(artifact_record->transcript_path.has_value());
    ASSERT_TRUE(artifact_record->sidechain_jsonl_path.has_value());
    const auto output_artifact = fs::path{*artifact_record->output_file_path};
    const auto transcript_artifact = fs::path{*artifact_record->transcript_path};
    const auto sidechain_artifact = fs::path{*artifact_record->sidechain_jsonl_path};
    EXPECT_TRUE(fs::is_symlink(output_artifact));
    EXPECT_TRUE(fs::exists(transcript_artifact));
    EXPECT_TRUE(fs::exists(sidechain_artifact));

    auto mailbox = cc::utils::write_to_mailbox(
        "reviewer",
        cc::utils::TeammateMessage{
            .from = "team-lead",
            .text = "Initial message",
            .timestamp = {},
            .read = false,
            .color = std::nullopt,
            .summary = std::nullopt,
        },
        std::optional<std::string_view>{"cleanup-team"});
    ASSERT_TRUE(mailbox.has_value()) << mailbox.error();
    EXPECT_TRUE(fs::exists(root / "teams" / "cleanup-team" / "inboxes" / "reviewer.json"));
    EXPECT_TRUE(fs::exists(root / "teams" / "cleanup-team-id.json"));

    cc::tools::BashTool bash_tool;
    auto shell_task = bash_tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "trap 'printf stopped-by-team-delete; exit 0' TERM; printf team-shell-ready; sleep 5",
      "run_in_background": true,
      "agentId": "reviewer@cleanup-team"
    })"));
    ASSERT_TRUE(shell_task.has_value());
    ASSERT_FALSE(shell_task->is_error);
    ASSERT_FALSE(shell_task->content.empty());
    auto shell_task_id = extract_background_task_id(shell_task->content.front().text);
    ASSERT_TRUE(shell_task_id.has_value()) << shell_task->content.front().text;
    auto shell_before_delete = cc::tools::bash::get_background_task_snapshot(*shell_task_id);
    ASSERT_TRUE(shell_before_delete.has_value());
    ASSERT_TRUE(shell_before_delete->agent_id.has_value());
    EXPECT_EQ(*shell_before_delete->agent_id, "reviewer@cleanup-team");
    EXPECT_TRUE(shell_before_delete->running);

    auto deleted = registry.execute("team_delete", cc::core::ToolInput::from_json(R"({
      "team_name": "cleanup-team"
    })"));
    ASSERT_TRUE(deleted.has_value());
    ASSERT_FALSE(deleted->is_error);
    ASSERT_FALSE(deleted->content.empty());
    EXPECT_NE(deleted->content.front().text.find("Deleted team cleanup-team (cleanup-team-id)"), std::string::npos);
    EXPECT_NE(deleted->content.front().text.find("cancelled_agents: 1"), std::string::npos);
    EXPECT_NE(deleted->content.front().text.find("teammate_terminations: 1"), std::string::npos);
    EXPECT_NE(deleted->content.front().text.find("teammate_kills: 1"), std::string::npos);
    EXPECT_NE(deleted->content.front().text.find("background_shell_tasks_stopped: 1"), std::string::npos);
    EXPECT_NE(deleted->content.front().text.find("transcript_artifacts_removed: 3"), std::string::npos);
    EXPECT_NE(deleted->content.front().text.find("worktree_cleanup_attempts: 1"), std::string::npos);

    EXPECT_FALSE(executor->is_active("reviewer@cleanup-team"));
    auto shell_after_delete = cc::tools::bash::get_background_task_snapshot(*shell_task_id);
    ASSERT_TRUE(shell_after_delete.has_value());
    EXPECT_TRUE(shell_after_delete->stopped);
    EXPECT_FALSE(fs::exists(output_artifact));
    EXPECT_FALSE(fs::exists(transcript_artifact));
    EXPECT_FALSE(fs::exists(sidechain_artifact));
    EXPECT_FALSE(fs::exists(root / "teams" / "cleanup-team-id.json"));
    EXPECT_FALSE(fs::exists(root / "teams" / "cleanup-team"));
    auto missing_team = cc::tools::global_team_store().get("cleanup-team-id");
    EXPECT_FALSE(missing_team.has_value());

    auto cancelled = cc::tools::agent_runtime::native_agent_store().get("reviewer@cleanup-team");
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->status, cc::tools::agent_runtime::NativeAgentStatus::Cancelled);
    EXPECT_TRUE(cancelled->cancel_requested);
    EXPECT_TRUE(cancelled->worktree_cleanup_performed);
    EXPECT_FALSE(cancelled->worktree_path.has_value());
    ASSERT_TRUE(cancelled->error.has_value());
    EXPECT_EQ(*cancelled->error, "team deleted: cleanup-team");

    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::utils::swarm_backends::BackendRegistry::reset();
    fs::remove_all(root);
}

TEST(Tools, TeamStoreUpdatesMemberStatusAndPersists) {
    auto root = fs::temp_directory_path() / "cc_repl_team_member_status_test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    cc::tools::global_team_store().clear_for_testing();

    auto created = cc::tools::global_team_store().create("status-team", "Status Team", {
        cc::tools::TeamMember{
            .agent_id = "status-agent",
            .role = cc::tools::MemberRole::Reviewer,
            .status = cc::tools::MemberStatus::Working,
            .current_task = "task-1",
        },
    });
    ASSERT_TRUE(created.has_value()) << std::string(cc::tools::format_error(created.error()));

    auto updated = cc::tools::global_team_store().update_member_status(
        "Status Team",
        "status-agent",
        cc::tools::MemberStatus::Done,
        "review complete");
    ASSERT_TRUE(updated.has_value()) << std::string(cc::tools::format_error(updated.error()));

    cc::tools::global_team_store().clear_for_testing();
    auto restored = cc::tools::global_team_store().get("status-team");
    ASSERT_TRUE(restored.has_value()) << std::string(cc::tools::format_error(restored.error()));
    ASSERT_EQ((*restored)->members.size(), 1u);
    EXPECT_EQ((*restored)->members.front().status, cc::tools::MemberStatus::Done);
    EXPECT_FALSE((*restored)->members.front().current_task.has_value());
    ASSERT_TRUE((*restored)->members.front().last_result.has_value());
    EXPECT_EQ(*(*restored)->members.front().last_result, "review complete");

    auto missing = cc::tools::global_team_store().update_member_status(
        "status-team",
        "missing-agent",
        cc::tools::MemberStatus::Error,
        "failed");
    EXPECT_FALSE(missing.has_value());

    cc::tools::global_team_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(Tools, RuntimeWorkflowExecutesJsonDefinition) {
    auto root = fs::temp_directory_path() / "cc_repl_runtime_workflow_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto workflow_path = root / "workflow.json";
    {
        std::ofstream workflow(workflow_path);
        workflow << R"JSON({
  "name": "migration-check",
  "variables": {
    "greeting": "hello",
    "run_extra": "false"
  },
  "steps": [
    {"id": "assign", "type": "assign", "action": "target=world"},
    {"id": "log", "type": "log", "action": "${greeting}-${target}"},
    {"id": "condition", "type": "condition", "action": "true"},
    {"id": "command", "type": "command", "command": "printf cmd-${target}"},
    {"id": "repeat", "type": "loop", "command": "printf ${repeat.index}", "maxIterations": 3},
    {"id": "skipped", "type": "log", "action": "should-not-run", "condition": "${run_extra}"}
  ]
})JSON";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::utils::json::JsonMutDoc doc;
    auto input = doc.object();
    input.add("file", doc.string(workflow_path.string()));
    doc.set_root(input);

    auto result = registry.execute("workflow", cc::core::ToolInput::from_json(doc.to_string()));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error) << result->content.front().text;
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Workflow migration-check completed"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("Steps executed: 5"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("Steps skipped: 1"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("hello-world"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("cmd-world"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("012"), std::string::npos);

    fs::remove_all(root);
}

TEST(Tools, TodoWriteParsesTypeScriptInputShape) {
    cc::tools::clear_all_todos_for_testing();
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto result = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Inspect migration gaps","status":"in_progress","activeForm":"Inspecting migration gaps"},
        {"content":"Run native validation","status":"pending","activeForm":"Running native validation"}
      ]
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("2 total, 2 added"), std::string::npos);
}

TEST(Tools, TodoWriteClearsAllDoneReplacementLists) {
    cc::tools::clear_all_todos_for_testing();
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto initial = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Implement parser","status":"in_progress","activeForm":"Implementing parser"},
        {"content":"Verify parser","status":"pending","activeForm":"Verifying parser"}
      ]
    })"));
    ASSERT_TRUE(initial.has_value());
    ASSERT_FALSE(initial->is_error);

    auto completed = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Implement parser","status":"completed","activeForm":"Implementing parser"},
        {"content":"Verify parser","status":"completed","activeForm":"Verifying parser"}
      ]
    })"));

    ASSERT_TRUE(completed.has_value());
    ASSERT_FALSE(completed->is_error);
    ASSERT_FALSE(completed->content.empty());
    EXPECT_NE(completed->content.front().text.find("0 total"), std::string::npos);
}

TEST(Tools, TodoWriteScopesItemsByAgentId) {
    cc::tools::clear_all_todos_for_testing();
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto agent_a = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "agent_id": "agent-a",
      "todos": [
        {"content":"Implement agent A work","status":"in_progress","activeForm":"Implementing agent A work"}
      ]
    })"));
    ASSERT_TRUE(agent_a.has_value());
    ASSERT_FALSE(agent_a->is_error);

    auto agent_b = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "agentId": "agent-b",
      "todos": [
        {"content":"Implement agent B work","status":"in_progress","activeForm":"Implementing agent B work"}
      ]
    })"));
    ASSERT_TRUE(agent_b.has_value());
    ASSERT_FALSE(agent_b->is_error);

    EXPECT_EQ(cc::tools::todo_count_for_agent("agent-a"), 1U);
    EXPECT_EQ(cc::tools::todo_count_for_agent("agent-b"), 1U);

    auto clear_a = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "agent_id": "agent-a",
      "todos": [
        {"content":"Implement agent A work","status":"completed","activeForm":"Implementing agent A work"}
      ]
    })"));
    ASSERT_TRUE(clear_a.has_value());
    ASSERT_FALSE(clear_a->is_error);

    EXPECT_EQ(cc::tools::todo_count_for_agent("agent-a"), 0U);
    EXPECT_EQ(cc::tools::todo_count_for_agent("agent-b"), 1U);
}

TEST(Tools, TodoWriteCleanupRemovesAgentScopedTodos) {
    cc::tools::clear_all_todos_for_testing();
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto result = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "agent_id": "cleanup-agent",
      "todos": [
        {"content":"Clean up scoped todos","status":"in_progress","activeForm":"Cleaning up scoped todos"}
      ]
    })"));
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    EXPECT_EQ(cc::tools::todo_count_for_agent("cleanup-agent"), 1U);

    EXPECT_TRUE(cc::tools::clear_todos_for_agent("cleanup-agent"));
    EXPECT_EQ(cc::tools::todo_count_for_agent("cleanup-agent"), 0U);
    EXPECT_EQ(cc::tools::todo_scope_count_for_testing(), 0U);
}

TEST(Tools, GlobFiltersByPattern) {
    auto root = fs::temp_directory_path() / "cc_repl_glob_test";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    {
        std::ofstream(root / "src" / "match.cpp") << "int main() {}\n";
        std::ofstream(root / "src" / "skip.txt") << "not source\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("Glob", cc::core::ToolInput::from_json(
        std::format(R"({{"pattern":"**/*.cpp","path":"{}"}})", root.string())));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    EXPECT_NE(result->content.front().text.find("match.cpp"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("skip.txt"), std::string::npos);
    fs::remove_all(root);
}

TEST(Tools, GrepUsesPathAndRegex) {
    auto root = fs::temp_directory_path() / "cc_repl_grep_test";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    {
        std::ofstream(root / "src" / "match.cpp") << "alpha_123\nbeta\n";
        std::ofstream(root / "src" / "skip.cpp") << "gamma\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("Grep", cc::core::ToolInput::from_json(
        std::format(R"({{"pattern":"alpha_[0-9]+","path":"{}"}})", (root / "src").string())));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    EXPECT_NE(result->content.front().text.find("match.cpp"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("alpha_123"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("gamma"), std::string::npos);
    fs::remove_all(root);
}

TEST(Tools, McpToolCallsNativeStdioServer) {
    auto root = fs::temp_directory_path() / "cc_repl_mcp_stdio_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {}, resources: {} },
        serverInfo: { name: 'fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'echo', description: 'Echo value', inputSchema: { type: 'object' } }]
      }
    });
    return;
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{ type: 'text', text: 'echo:' + request.params.arguments.value }]
      }
    });
    return;
  }
  if (request.method === 'resources/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: { resources: [{ uri: 'fixture://one', name: 'one', mimeType: 'text/plain' }] }
    });
    return;
  }
  if (request.method === 'resources/read') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: { contents: [{ uri: request.params.uri, mimeType: 'text/plain', text: 'resource-body' }] }
    });
  }
});
)JS";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "echo_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());

    auto restarted = cc::tools::restart_native_mcp_server("echo_fixture");
    ASSERT_TRUE(restarted.has_value()) << restarted.error();
    EXPECT_EQ(restarted->status, "ready");
    ASSERT_EQ(restarted->tools.size(), 1u);
    EXPECT_EQ(restarted->tools.front().name, "echo");

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("mcp", cc::core::ToolInput::from_json(
        R"({"server_name":"echo_fixture","tool_name":"echo","arguments":{"value":"hello"}})"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_EQ(result->content.front().text, "echo:hello");

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, NativeMcpRuntimeLoadsRemoteConfigWithOAuthFromConfigFiles) {
    auto root = fs::weakly_canonical(fs::temp_directory_path()) / "cc_repl_mcp_remote_config_runtime_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude");
    EnvironmentGuard home_guard("HOME", root.string());

    {
        std::ofstream config(root / ".claude" / "config.json");
        config << R"JSON({
  "mcpServers": {
    "remote_fixture": {
      "type": "http",
      "url": "https://mcp.example.com/mcp",
      "headers": {"X-Test": "present"},
      "headersHelper": "node headers.js",
      "oauth": {
        "authServerMetadataUrl": "https://auth.example.com/.well-known/oauth-authorization-server",
        "callbackPort": 19485,
        "clientId": "client-1",
        "xaa": true
      }
    }
  }
})JSON";
    }

    {
        CurrentPathGuard cwd(root);
        auto reloaded = cc::tools::reload_native_mcp_servers_from_config();
        ASSERT_TRUE(reloaded.has_value()) << reloaded.error();
        auto configured = cc::tools::native_mcp_configured_server("remote_fixture");
        ASSERT_TRUE(configured.has_value());
        EXPECT_EQ(configured->transport, cc::services::mcp::TransportType::StreamableHttp);
        EXPECT_EQ(configured->url, "https://mcp.example.com/mcp");
        EXPECT_EQ(configured->headers.at("X-Test"), "present");
        EXPECT_EQ(configured->headers_helper, "node headers.js");
        ASSERT_TRUE(configured->oauth.has_value());
        ASSERT_TRUE(configured->oauth->auth_server_metadata_url.has_value());
        EXPECT_EQ(*configured->oauth->auth_server_metadata_url, "https://auth.example.com/.well-known/oauth-authorization-server");
        ASSERT_TRUE(configured->oauth->callback_port.has_value());
        EXPECT_EQ(*configured->oauth->callback_port, 19485);
        ASSERT_TRUE(configured->oauth->client_id.has_value());
        EXPECT_EQ(*configured->oauth->client_id, "client-1");
        EXPECT_TRUE(configured->oauth->xaa);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, McpAuthUsesNativeOAuthFlowForConfiguredRemoteServers) {
    EnvironmentGuard xaa_guard("CLAUDE_CODE_ENABLE_XAA", "0");
    cc::tools::NativeMcpConfiguredServer server;
    server.name = "auth_fixture";
    server.transport = cc::services::mcp::TransportType::StreamableHttp;
    server.url = "https://mcp.example.com/mcp";
    server.oauth = cc::services::mcp::McpOAuthConfig{.xaa = true};
    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({server}).has_value());

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("mcp_auth", cc::core::ToolInput::from_json(
        R"({"server_name":"auth_fixture"})"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Failed to start OAuth flow"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("XAA is not enabled"), std::string::npos);

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
}

TEST(Tools, McpToolReturnsErrorWhenNativeServerIsMissing) {
    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("mcp", cc::core::ToolInput::from_json(
        R"({"server_name":"missing_fixture","tool_name":"echo","arguments":{"value":"hello"}})"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("MCP server not found"), std::string::npos);
}

TEST(Tools, McpRuntimeLoadsPluginManifestMcpServers) {
    auto root = fs::weakly_canonical(fs::temp_directory_path()) / "cc_repl_plugin_mcp_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude");
    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard plugin_cache_guard(
        "CLAUDE_CODE_PLUGIN_CACHE_DIR",
        (root / ".claude" / "plugins").string()
    );

    const auto plugin_root = root / ".claude" / "plugins" / "mcp-fixture";
    fs::create_directories(plugin_root);
    const auto server_path = plugin_root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'plugin-mcp-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'plugin_echo', description: 'Echo from plugin MCP', inputSchema: { type: 'object' } }]
      }
    });
    return;
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{
          type: 'text',
          text: [
            'plugin',
            request.params.arguments.value,
            process.env.PLUGIN_MCP_FIXTURE,
            process.env.PLUGIN_MCP_TOKEN,
            process.env.CLAUDE_PLUGIN_ROOT
          ].join(':')
        }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream settings(root / ".claude" / "settings.json");
        settings << R"JSON({
  "pluginConfigs": {
    "mcp-fixture": {
      "options": {
        "suffix": "top-level",
        "token": "shared-token"
      },
      "mcpServers": {
        "echo": {
          "suffix": "configured",
          "token": "secret-token"
        }
      }
    }
  }
})JSON";
    }
    {
        std::ofstream defaults(plugin_root / ".mcp.json");
        defaults << R"JSON({
  "mcpServers": {
    "echo": {
      "command": "missing-plugin-mcp-command"
    }
  }
})JSON";
    }
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << std::format(R"JSON({{
  "name": "mcp-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "mcpServers": {{
    "echo": {{
      "type": "stdio",
      "command": "node",
      "args": ["${{CLAUDE_PLUGIN_ROOT}}/server.js"],
      "env": {{
        "PLUGIN_MCP_FIXTURE": "${{user_config.suffix}}",
        "PLUGIN_MCP_TOKEN": "${{user_config.token}}"
      }}
    }}
  }}
}})JSON");
    }
    {
        std::ofstream entry(plugin_root / "plugin.js");
        entry << "process.exit(0)\n";
    }

    {
        CurrentPathGuard cwd(root);
        auto servers = cc::tools::discover_plugin_native_mcp_servers();
        auto it = std::ranges::find_if(servers, [](const auto& server) {
            return server.name == "plugin:mcp-fixture:echo";
        });
        ASSERT_NE(it, servers.end());
        EXPECT_EQ(it->command, "node");
        ASSERT_EQ(it->args.size(), 1u);
        EXPECT_EQ(it->args.front(), server_path.string());
        EXPECT_EQ(it->env.at("PLUGIN_MCP_FIXTURE"), "configured");
        EXPECT_EQ(it->env.at("PLUGIN_MCP_TOKEN"), "secret-token");
        EXPECT_EQ(it->env.at("CLAUDE_PLUGIN_ROOT"), plugin_root.string());
        EXPECT_EQ(
            it->env.at("CLAUDE_PLUGIN_DATA"),
            (root / ".claude" / "plugins" / "data" / "mcp-fixture").string()
        );

        auto synced = cc::tools::sync_native_mcp_servers(std::move(servers));
        ASSERT_TRUE(synced.has_value()) << synced.error();

        auto restarted = cc::tools::restart_native_mcp_server("plugin:mcp-fixture:echo");
        ASSERT_TRUE(restarted.has_value()) << restarted.error();
        EXPECT_EQ(restarted->status, "ready");
        ASSERT_EQ(restarted->tools.size(), 1u);
        EXPECT_EQ(restarted->tools.front().name, "plugin_echo");

        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        auto result = registry.execute("mcp", cc::core::ToolInput::from_json(R"({
          "server_name": "plugin:mcp-fixture:echo",
          "tool_name": "plugin_echo",
          "arguments": {"value": "hello"}
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_EQ(
            result->content.front().text,
            "plugin:hello:configured:secret-token:" + plugin_root.string()
        );
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, McpRuntimeLoadsPluginMcpbServers) {
    auto root = fs::weakly_canonical(fs::temp_directory_path()) / "cc_repl_plugin_mcpb_test";
    fs::remove_all(root);
    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard plugin_cache_guard(
        "CLAUDE_CODE_PLUGIN_CACHE_DIR",
        (root / ".claude" / "plugins").string()
    );

    const auto plugin_root = root / ".claude" / "plugins" / "mcpb-fixture";
    const auto bundle_src = root / "bundle-src";
    fs::create_directories(plugin_root);
    fs::create_directories(bundle_src);
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << R"JSON({
  "name": "mcpb-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "mcpServers": ["bundle.mcpb"]
})JSON";
    }
    {
        std::ofstream entry(plugin_root / "plugin.js");
        entry << "process.exit(0)\n";
    }
    {
        std::ofstream manifest(bundle_src / "manifest.json");
        manifest << R"JSON({
  "name": "bundle",
  "version": "1.0.0",
  "server": {
    "type": "stdio",
    "command": "node",
    "args": ["${CLAUDE_PLUGIN_ROOT}/server.js"],
    "env": {
      "PLUGIN_MCPB_VALUE": "from-bundle"
    }
  }
})JSON";
    }
    {
        std::ofstream server(bundle_src / "server.js");
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'mcpb-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'bundle_echo', description: 'Echo from MCPB', inputSchema: { type: 'object' } }]
      }
    });
    return;
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{
          type: 'text',
          text: ['mcpb', request.params.arguments.value, process.env.PLUGIN_MCPB_VALUE, process.env.CLAUDE_PLUGIN_ROOT].join(':')
        }]
      }
    });
  }
});
)JS";
    }

    const auto bundle_path = plugin_root / "bundle.mcpb";
    const auto zip_command = std::format(
        "cd {} && zip -qr {} .",
        shell_quote_for_test(bundle_src.string()),
        shell_quote_for_test(bundle_path.string()));
    if (std::system(zip_command.c_str()) != 0) {
        GTEST_SKIP() << "zip command is not available";
    }

    {
        CurrentPathGuard cwd(root);
        auto servers = cc::tools::discover_plugin_native_mcp_servers();
        auto it = std::ranges::find_if(servers, [](const auto& server) {
            return server.name == "plugin:mcpb-fixture:bundle";
        });
        ASSERT_NE(it, servers.end());
        EXPECT_EQ(it->command, "node");
        ASSERT_EQ(it->args.size(), 1u);
        EXPECT_NE(it->args.front().find("mcpb/bundle/server.js"), std::string::npos);
        EXPECT_EQ(it->env.at("PLUGIN_MCPB_VALUE"), "from-bundle");
        EXPECT_NE(it->env.at("CLAUDE_PLUGIN_ROOT").find("mcpb/bundle"), std::string::npos);

        auto synced = cc::tools::sync_native_mcp_servers(std::move(servers));
        ASSERT_TRUE(synced.has_value()) << synced.error();
        auto restarted = cc::tools::restart_native_mcp_server("plugin:mcpb-fixture:bundle");
        ASSERT_TRUE(restarted.has_value()) << restarted.error();
        EXPECT_EQ(restarted->status, "ready");
        ASSERT_EQ(restarted->tools.size(), 1u);
        EXPECT_EQ(restarted->tools.front().name, "bundle_echo");

        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        auto result = registry.execute("mcp", cc::core::ToolInput::from_json(R"({
          "server_name": "plugin:mcpb-fixture:bundle",
          "tool_name": "bundle_echo",
          "arguments": {"value": "hello"}
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("mcpb:hello:from-bundle:"), std::string::npos);
        EXPECT_NE(result->content.front().text.find("mcpb/bundle"), std::string::npos);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}
