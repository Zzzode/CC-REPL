module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sstream>

export module cc.services.remote_session;


export namespace cc::services {


enum class RemoteSessionState { connecting, connected, disconnected, error };


struct RemoteSession {
    std::string id;
    std::string name;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_active;
    RemoteSessionState state{RemoteSessionState::disconnected};
    std::string transport_url;
};


enum class RemoteMessageType { 
    command, response, event, heartbeat, permission_request, permission_response 
};


struct RemoteMessage {
    RemoteMessageType type;
    std::string payload;
    std::string session_id;
    std::chrono::system_clock::time_point timestamp;
};


struct RemoteConfig {
    std::string bridge_url;
    std::string auth_token;
    uint32_t heartbeat_interval_ms{30'000};
    uint32_t reconnect_max_retries{5};
    uint32_t reconnect_base_delay_ms{1'000};
};


struct RemoteError {
    enum Code { connection_failed, auth_failed, timeout, session_not_found, protocol_error };
    Code code;
    std::string message;
};


using PermissionHandler = std::function<bool(std::string_view tool_name, std::string_view args_json)>;

using MessageCallback = std::function<void(const RemoteMessage&)>;

using UnsubscribeFn = std::function<void()>;


class RemoteSessionManager {
    RemoteConfig config_;
    RemoteSessionState state_{RemoteSessionState::disconnected};
    std::optional<RemoteSession> current_session_;
    std::vector<MessageCallback> message_handlers_;
    std::vector<RemoteMessage> outbound_messages_;
    PermissionHandler permission_handler_;
    int reconnect_attempts_{0};

public:
    explicit RemoteSessionManager(RemoteConfig config) : config_(std::move(config)) {}



    [[nodiscard]] auto create_session(std::string name) 
        -> std::expected<RemoteSession, RemoteError> {
        RemoteSession session{
            .id = generate_session_id(),
            .name = std::move(name),
            .created_at = std::chrono::system_clock::now(),
            .last_active = std::chrono::system_clock::now(),
            .state = RemoteSessionState::connected,
            .transport_url = config_.bridge_url + "/ws"
        };
        current_session_ = session;
        state_ = RemoteSessionState::connected;
        send_message(RemoteMessage{
            .type = RemoteMessageType::command,
            .payload = "create:" + session.name,
            .session_id = session.id,
            .timestamp = std::chrono::system_clock::now(),
        });
        return session;
    }


    [[nodiscard]] auto resume_session(std::string_view id) 
        -> std::expected<void, RemoteError> {
        if (!current_session_ || current_session_->id != id)
            return std::unexpected(RemoteError{RemoteError::session_not_found, "session not found"});
        state_ = RemoteSessionState::connected;
        send_message(RemoteMessage{
            .type = RemoteMessageType::command,
            .payload = "resume",
            .session_id = std::string{id},
            .timestamp = std::chrono::system_clock::now(),
        });
        return {};
    }


    [[nodiscard]] auto list_sessions() const -> std::vector<RemoteSession> {
        if (current_session_) return {*current_session_};
        return {};
    }


    [[nodiscard]] auto delete_session(std::string_view id) 
        -> std::expected<void, RemoteError> {
        if (current_session_ && current_session_->id == id) {
            disconnect();
            current_session_.reset();
            return {};
        }
        return std::unexpected(RemoteError{RemoteError::session_not_found, "session not found"});
    }


    void send_message(RemoteMessage msg) {
        msg.timestamp = std::chrono::system_clock::now();
        if (current_session_) msg.session_id = current_session_->id;
        outbound_messages_.push_back(msg);
        handle_incoming(msg);
    }


    [[nodiscard]] auto on_message(MessageCallback callback) -> UnsubscribeFn {
        message_handlers_.push_back(std::move(callback));
        size_t idx = message_handlers_.size() - 1;
        return [this, idx]() {
            if (idx < message_handlers_.size())
                message_handlers_[idx] = nullptr;
        };
    }


    [[nodiscard]] auto get_state() const -> RemoteSessionState { return state_; }


    void disconnect() {
        state_ = RemoteSessionState::disconnected;
        reconnect_attempts_ = 0;
    }


    void set_permission_handler(PermissionHandler handler) {
        permission_handler_ = std::move(handler);
    }

private:
    static auto generate_session_id() -> std::string {

        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "rs_" + std::to_string(now);
    }

    void handle_incoming(const RemoteMessage& msg) {
        if (msg.type == RemoteMessageType::permission_request && permission_handler_) {
            auto separator = msg.payload.find('|');
            auto tool_name = separator == std::string::npos ? std::string_view{msg.payload} : std::string_view{msg.payload}.substr(0, separator);
            auto args_json = separator == std::string::npos ? std::string_view{} : std::string_view{msg.payload}.substr(separator + 1);
            auto allowed = permission_handler_(tool_name, args_json);
            outbound_messages_.push_back(RemoteMessage{
                .type = RemoteMessageType::permission_response,
                .payload = allowed ? "allow" : "deny",
                .session_id = msg.session_id,
                .timestamp = std::chrono::system_clock::now(),
            });
        }
        for (const auto& handler : message_handlers_) {
            if (handler) handler(msg);
        }
    }
};

} // namespace cc::services
