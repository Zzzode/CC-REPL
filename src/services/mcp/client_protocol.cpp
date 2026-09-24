// Implementation unit for cc.services.mcp.client — McpClient connection/
// protocol lifecycle and JSON-RPC machinery. This unit (together with
// client_requests.cpp) is the only one that imports cc.utils.json, so the
// textual <yyjson.h> closure leaves the interface BMI.
module;

#include <cstdint> // global int64_t used unqualified; import std gives std::int64_t only

module cc.services.mcp.client;

import std;

import cc.services.mcp.types;
import cc.utils.json;

namespace cc::services::mcp {

using namespace cc::utils::json;

namespace {

// Demoted from private McpClient members: naming JsonVal in a member
// declaration pinned cc.utils.json in the producer BMI. They are pure
// functions of their argument (the recursive one additionally takes the
// client name previously read from config_).
[[nodiscard]] std::optional<RequestId> parse_request_id(JsonVal id_node) {
    if (!id_node.valid() || id_node.is_null()) return std::nullopt;
    if (id_node.is_num()) return RequestId{static_cast<int64_t>(id_node.as_int())};
    if (id_node.is_str()) return RequestId{std::string(id_node.as_str())};
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> serialize_json_value(JsonVal value) {
    if (!value.valid() || value.is_null()) return std::nullopt;
    JsonMutDoc doc;
    auto copy = doc.copy_val(value);
    if (!copy.valid()) return std::nullopt;
    doc.set_root(copy);
    return doc.to_string();
}

[[nodiscard]] std::string prompt_message_content_to_text(JsonVal content,
                                                         std::string_view client_name) {
    if (content.is_str()) {
        return std::string(content.as_str());
    }
    if (content.is_arr()) {
        std::string joined;
        content.iter([&](JsonVal item) {
            auto text = prompt_message_content_to_text(item, client_name);
            if (text.empty()) return;
            if (!joined.empty()) joined += "\n";
            joined += std::move(text);
        });
        return joined;
    }
    if (!content.is_obj()) return {};

    const auto type = std::string(content.get("type").as_str());
    if (type == "text") {
        return std::string(content.get("text").as_str());
    }
    if (type == "resource") {
        auto resource = content.get("resource");
        if (!resource.is_obj()) return {};
        const auto uri = std::string(resource.get("uri").as_str());
        std::string prefix = std::format("[Resource from {} at {}] ", client_name, uri);
        if (auto text = resource.get("text"); text.is_str()) {
            return prefix + std::string(text.as_str());
        }
        if (auto blob = resource.get("blob"); blob.is_str()) {
            const auto mime_type = std::string(resource.get("mimeType").as_str());
            const auto mime_label = mime_type.empty() ? std::string{"unknown type"} : mime_type;
            return prefix + std::format(
                "Binary content ({}, {} base64 characters)",
                mime_label,
                blob.as_str().size());
        }
        return prefix;
    }
    if (type == "resource_link") {
        const auto name = std::string(content.get("name").as_str());
        const auto uri = std::string(content.get("uri").as_str());
        std::string text = std::format("[Resource link: {}] {}", name.empty() ? uri : name, uri);
        if (auto description = content.get("description"); description.is_str() && !description.as_str().empty()) {
            text += std::format(" ({})", description.as_str());
        }
        return text;
    }
    if (type == "image" || type == "audio") {
        const auto mime_type = std::string(content.get("mimeType").as_str());
        const auto mime_label = mime_type.empty() ? std::string{"unknown type"} : mime_type;
        const auto data = content.get("data");
        return std::format(
            "[{} from {}] Binary content ({}, {} base64 characters)",
            type == "image" ? "Image" : "Audio",
            client_name,
            mime_label,
            data.is_str() ? data.as_str().size() : 0);
    }

    return serialize_json_value(content).value_or(std::string{});
}

} // namespace

McpClient::McpClient(Config config)
    : config_(std::move(config))
    , state_(ServerState::NotStarted)
    , next_request_id_(1)
    , running_(false) {}

McpClient::~McpClient() {
    shutdown();
}

// Connect to server using stdio transport
McpResult<void> McpClient::connect_stdio(
    std::string command, std::vector<std::string> args,
    std::map<std::string, std::string> env) {

    if (state_ != ServerState::NotStarted && state_ != ServerState::Stopped) {
        return std::unexpected(McpClientError::AlreadyConnected);
    }

    state_ = ServerState::Starting;
    transport_ = std::make_unique<StdioTransport>(std::move(command), std::move(args), std::move(env));

    auto result = transport_->start();
    if (!result) {
        state_ = ServerState::Error;
        return result;
    }

    return initialize();
}

// Connect to server using SSE transport
McpResult<void> McpClient::connect_sse(
    std::string url, std::map<std::string, std::string> headers) {

    if (state_ != ServerState::NotStarted && state_ != ServerState::Stopped) {
        return std::unexpected(McpClientError::AlreadyConnected);
    }

    state_ = ServerState::Starting;
    transport_ = std::make_unique<SseTransport>(std::move(url), std::move(headers));

    auto result = transport_->start();
    if (!result) {
        state_ = ServerState::Error;
        return result;
    }

    return initialize();
}

// Connect to server using streamable HTTP transport
McpResult<void> McpClient::connect_streamable_http(
    std::string url, std::map<std::string, std::string> headers) {

    if (state_ != ServerState::NotStarted && state_ != ServerState::Stopped) {
        return std::unexpected(McpClientError::AlreadyConnected);
    }

    state_ = ServerState::Starting;
    transport_ = std::make_unique<StreamableHttpTransport>(std::move(url), std::move(headers));

    auto result = transport_->start();
    if (!result) {
        state_ = ServerState::Error;
        return result;
    }

    return initialize();
}

McpResult<PromptGetResult> McpClient::get_prompt(
    std::string_view name,
    const std::map<std::string, std::string>& arguments) {

    if (state_ != ServerState::Ready) {
        return std::unexpected(McpClientError::NotConnected);
    }

    JsonMutDoc doc;
    auto params = doc.object();
    params.add("name", doc.string(name));

    auto args_obj = doc.object();
    for (const auto& [key, value] : arguments) {
        args_obj.add(key, doc.string(value));
    }
    params.add("arguments", args_obj);

    doc.set_root(params);

    auto response = send_request_sync("prompts/get", doc.to_string());
    if (!response) {
        return std::unexpected(response.error());
    }

    PromptGetResult result;
    auto resp_doc = parse(*response);
    if (resp_doc) {
        auto root = resp_doc->root();
        auto result_node = root.get("result");
        if (result_node.is_obj()) {
            result.description = std::string(result_node.get("description").as_str());
            // Parse messages
            auto messages_node = result_node.get("messages");
            if (messages_node.is_arr()) {
                messages_node.iter([&result, this](JsonVal msg_val) {
                    if (msg_val.is_obj()) {
                        McpPromptMessage msg;
                        auto role_str = std::string(msg_val.get("role").as_str());
                        msg.role = (role_str == "assistant") ? PromptRole::Assistant : PromptRole::User;
                        msg.content = prompt_message_content_to_text(msg_val.get("content"), config_.name);
                        result.messages.push_back(std::move(msg));
                    }
                });
            }
        }
    }

    return result;
}

// Graceful shutdown
void McpClient::shutdown() {
    if (state_ == ServerState::Ready || state_ == ServerState::Initializing) {
        state_ = ServerState::ShuttingDown;
        send_notification("notifications/cancelled", std::nullopt);
    }

    running_ = false;
    if (transport_) {
        transport_->close();
    }
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    state_ = ServerState::Stopped;
}

// Initialize the MCP connection (handshake)
McpResult<void> McpClient::initialize() {
    state_ = ServerState::Initializing;

    running_ = true;
    if (!receive_thread_.joinable()) {
        receive_thread_ = std::thread(&McpClient::receive_loop, this);
    }

    // Build initialize params
    JsonMutDoc doc;
    auto root = doc.object();
    root.add("protocolVersion", doc.string("2024-11-05"));

    auto capabilities = doc.object();
    if (config_.capabilities.roots) {
        auto roots_cap = doc.object();
        roots_cap.add("listChanged", doc.boolean(config_.capabilities.roots_capabilities.list_changed));
        capabilities.add("roots", roots_cap);
    }
    root.add("capabilities", capabilities);

    auto client_info = doc.object();
    client_info.add("name", doc.string(config_.client_info.name));
    client_info.add("version", doc.string(config_.client_info.version));
    root.add("clientInfo", client_info);

    doc.set_root(root);

    auto response = send_request_sync("initialize", doc.to_string());
    if (!response) {
        state_ = ServerState::Error;
        running_ = false;
        if (transport_) {
            transport_->close();
        }
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        return std::unexpected(response.error());
    }

    // Parse server capabilities from response
    auto init_result = parse_initialize_result(*response);
    if (!init_result) {
        state_ = ServerState::Error;
        running_ = false;
        if (transport_) {
            transport_->close();
        }
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        return std::unexpected(McpClientError::InitializationFailed);
    }

    server_info_ = init_result->server_info;
    server_caps_ = init_result->capabilities;

    // Send initialized notification
    send_notification("notifications/initialized", std::nullopt);

    state_ = ServerState::Ready;
    return {};
}

// Send a JSON-RPC request and wait for response (synchronous)
McpResult<std::string> McpClient::send_request_sync(
    std::string_view method, std::optional<std::string> params) {

    std::promise<McpResult<std::string>> promise;
    auto future = promise.get_future();

    auto callback = [&promise](const std::string& response, std::optional<McpClientError> error) {
        if (error) {
            promise.set_value(std::unexpected(*error));
        } else {
            promise.set_value(response);
        }
    };

    send_request_async(method, std::move(params), std::move(callback));

    auto status = future.wait_for(config_.request_timeout);
    if (status == std::future_status::timeout) {
        return std::unexpected(McpClientError::Timeout);
    }

    return future.get();
}

// Send a JSON-RPC request asynchronously
void McpClient::send_request_async(
    std::string_view method,
    std::optional<std::string> params,
    RequestCallback callback) {

    RequestId id = next_request_id_++;
    auto request = make_request(id, std::string(method), std::move(params));
    auto serialized = serialize_request(request);

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[id] = PendingRequest{
            .id = id,
            .sent_at = std::chrono::steady_clock::now(),
            .timeout = config_.request_timeout,
            .callback = std::move(callback)
        };
    }

    auto result = transport_send(serialized);
    if (!result) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(id);
        if (it != pending_requests_.end()) {
            if (it->second.callback) {
                it->second.callback("", result.error());
            }
            pending_requests_.erase(it);
        }
    }
}

// Send a JSON-RPC notification (no response expected)
void McpClient::send_notification(std::string_view method, std::optional<std::string> params) {
    auto notif = make_notification(std::string(method), std::move(params));
    auto serialized = serialize_notification(notif);
    (void)transport_send(serialized);
}

// Transport layer abstraction
McpResult<void> McpClient::transport_send(std::string_view message) {
    if (!transport_ || !transport_->is_connected()) {
        return std::unexpected(McpClientError::NotConnected);
    }
    return transport_->send(message);
}

void McpClient::send_error_response(const RequestId& id, JsonRpcErrorCode code, std::string_view message) {
    JsonMutDoc doc;
    auto root = doc.object();
    root.add("jsonrpc", doc.string("2.0"));
    if (std::holds_alternative<int64_t>(id)) {
        root.add("id", doc.number(std::get<int64_t>(id)));
    } else {
        root.add("id", doc.string(std::get<std::string>(id)));
    }
    auto error = doc.object();
    error.add("code", doc.number(static_cast<int64_t>(code)));
    error.add("message", doc.string(message));
    root.add("error", error);
    doc.set_root(root);
    (void)transport_send(doc.to_string());
}

// Receive loop (runs in background thread)
void McpClient::receive_loop() {
    using namespace std::chrono_literals;
    while (running_) {
        if (!transport_ || !transport_->is_connected()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        auto result = transport_->receive();
        if (!result) {
            if (result.error() == McpClientError::ServerClosed) {
                state_ = ServerState::Stopped;
                running_ = false;
            } else if (result.error() == McpClientError::Timeout) {
                // Check for timed-out pending requests
                check_pending_timeouts();
            }
            continue;
        }

        handle_incoming_message(*result);
    }
}

// Handle incoming message
void McpClient::handle_incoming_message(const std::string& message) {
    // Parse message
    auto doc = parse(message);
    if (!doc) {
        return;
    }

    auto root = doc->root();
    if (!root.is_obj()) {
        return;
    }

    auto method_node = root.get("method");
    auto id_node = root.get("id");

    if (method_node.is_str()) {
        std::string method = std::string(method_node.as_str());
        auto request_id = parse_request_id(id_node);

        if (request_id) {
            if (method == "roots/list") {
                handle_roots_list_request(*request_id);
            } else {
                send_error_response(
                    *request_id,
                    JsonRpcErrorCode::MethodNotFound,
                    "Unsupported server request");
            }
            return;
        }

        JsonRpcNotification notif;
        notif.method = std::move(method);
        notif.params_json = serialize_json_value(root.get("params"));

        if (notification_callback_) {
            notification_callback_(notif);
        }
        return;
    }

    if (auto response_id = parse_request_id(id_node)) {
        RequestId id = std::move(*response_id);

        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(id);
        if (it != pending_requests_.end() && !it->second.completed) {
            it->second.completed = true;
            if (it->second.callback) {
                it->second.callback(message, std::nullopt);
            }
            pending_requests_.erase(it);
        }
    }
}

// Handle roots/list request from server
void McpClient::handle_roots_list_request(const RequestId& id) {
    // Build response
    JsonMutDoc resp_doc;
    auto resp_root = resp_doc.object();
    resp_root.add("jsonrpc", resp_doc.string("2.0"));

    if (std::holds_alternative<int64_t>(id)) {
        resp_root.add("id", resp_doc.number(std::get<int64_t>(id)));
    } else {
        resp_root.add("id", resp_doc.string(std::get<std::string>(id)));
    }

    // Get roots from handler
    auto result = resp_doc.object();
    auto roots_arr = resp_doc.array();

    if (roots_handler_) {
        auto roots = roots_handler_();
        for (const auto& r : roots) {
            auto root_obj = resp_doc.object();
            root_obj.add("uri", resp_doc.string(r.uri));
            if (r.name) {
                root_obj.add("name", resp_doc.string(*r.name));
            }
            roots_arr.append(root_obj);
        }
    }

    result.add("roots", roots_arr);
    resp_root.add("result", result);

    resp_doc.set_root(resp_root);
    (void)transport_send(resp_doc.to_string());
}

// Check for timed-out pending requests
void McpClient::check_pending_timeouts() {
    auto now = std::chrono::steady_clock::now();

    std::vector<RequestId> timed_out;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (const auto& [id, req] : pending_requests_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - req.sent_at);
            if (elapsed > req.timeout) {
                timed_out.push_back(id);
            }
        }

        for (const auto& id : timed_out) {
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end() && !it->second.completed) {
                it->second.completed = true;
                if (it->second.callback) {
                    it->second.callback("", McpClientError::Timeout);
                }
                pending_requests_.erase(it);
            }
        }
    }
}

} // namespace cc::services::mcp
