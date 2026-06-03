module;

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <optional>
#include <print>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module cc.bridge.transport;


export namespace cc::bridge {


enum class MessagePriority { low, normal, high, system };

namespace detail {

[[nodiscard]] auto json_escape(std::string_view value) -> std::string {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}
[[nodiscard]] auto priority_to_string(MessagePriority priority) -> std::string_view {
    switch (priority) {
        case MessagePriority::low: return "low";
        case MessagePriority::normal: return "normal";
        case MessagePriority::high: return "high";
        case MessagePriority::system: return "system";
    }
    return "normal";
}

} // namespace detail


struct BridgeMessage {
    std::string id;
    std::string type;         // "request", "response", "event", "heartbeat"
    std::string method;
    std::string payload;      // JSON payload
    MessagePriority priority{MessagePriority::normal};
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> correlation_id;
};


struct InboundAttachment {
    std::string filename;
    std::string mime_type;
    std::vector<uint8_t> data;
    size_t size_bytes;
};


enum class TransportState { disconnected, connecting, connected, reconnecting, error };


struct TransportError {
    enum Code { connection_refused, timeout, auth_failed, protocol_error, closed_by_peer };
    Code code;
    std::string message;
    bool retryable{true};
};


using MessageHandler = std::function<void(BridgeMessage)>;
using AttachmentHandler = std::function<void(InboundAttachment)>;
using StateChangeHandler = std::function<void(TransportState, TransportState)>;  // old, new
using ErrorHandler = std::function<void(TransportError)>;


class TransportFlushGate {
    std::atomic<bool> open_{true};
    std::queue<BridgeMessage> pending_;
    std::function<void(BridgeMessage)> sender_;
public:
    void close() { open_ = false; }
    void open() {
        open_ = true;
        while (sender_ && !pending_.empty()) {
            auto msg = std::move(pending_.front());
            pending_.pop();
            sender_(std::move(msg));
        }
    }
    void enqueue(BridgeMessage msg) {
        if (open_ && sender_) {
            sender_(std::move(msg));
        } else {
            pending_.push(std::move(msg));
        }
    }
    void set_sender(std::function<void(BridgeMessage)> sender) { sender_ = std::move(sender); }
    [[nodiscard]] auto pending_count() const -> size_t { return pending_.size(); }
    [[nodiscard]] auto is_open() const -> bool { return open_.load(); }
};


class CapacityWake {
    size_t capacity_{100};
    size_t current_load_{0};
    std::function<void()> on_capacity_available_;
public:
    void set_capacity(size_t cap) { capacity_ = cap; }
    void record_usage(size_t units = 1) { current_load_ += units; }
    void release(size_t units = 1) {
        if (current_load_ >= units) current_load_ -= units;
        if (current_load_ < capacity_ && on_capacity_available_) on_capacity_available_();
    }
    [[nodiscard]] auto has_capacity() const -> bool { return current_load_ < capacity_; }
    void on_available(std::function<void()> cb) { on_capacity_available_ = std::move(cb); }
};


class BridgeTransport {
protected:
    TransportState state_{TransportState::disconnected};
    std::vector<MessageHandler> message_handlers_;
    std::vector<StateChangeHandler> state_handlers_;
    std::vector<ErrorHandler> error_handlers_;
    TransportFlushGate flush_gate_;
    CapacityWake capacity_wake_;

public:
    BridgeTransport() {
        flush_gate_.set_sender([this](BridgeMessage msg) { emit_message(std::move(msg)); });
    }

    virtual ~BridgeTransport() = default;
    

    virtual auto connect(std::string_view url, std::optional<std::string_view> token) 
        -> std::expected<void, TransportError> = 0;
    virtual void disconnect() = 0;
    

    virtual auto send(BridgeMessage msg) -> std::expected<void, TransportError> = 0;
    

    [[nodiscard]] auto get_state() const -> TransportState { return state_; }
    [[nodiscard]] auto is_connected() const -> bool { return state_ == TransportState::connected; }
    

    void on_message(MessageHandler handler) { message_handlers_.push_back(std::move(handler)); }
    void on_state_change(StateChangeHandler handler) { state_handlers_.push_back(std::move(handler)); }
    void on_error(ErrorHandler handler) { error_handlers_.push_back(std::move(handler)); }
    

    auto& flush_gate() { return flush_gate_; }
    auto& capacity() { return capacity_wake_; }

protected:
    [[nodiscard]] static auto serialize_message(const BridgeMessage& msg) -> std::string {
        std::ostringstream json;
        json << R"({"id":")";
        json << detail::json_escape(msg.id) << R"(","type":")" << detail::json_escape(msg.type)
             << R"(","method":")" << detail::json_escape(msg.method)
             << R"(","payload":)" << (msg.payload.empty() ? "{}" : msg.payload)
             << R"(,"priority":")" << detail::priority_to_string(msg.priority) << '"';
        if (msg.correlation_id) {
            json << R"(,"correlation_id":")" << detail::json_escape(*msg.correlation_id) << '"';
        }
        json << '}';
        return json.str();
    }

    void set_state(TransportState new_state) {
        auto old = state_;
        state_ = new_state;
        for (const auto& h : state_handlers_) if (h) h(old, new_state);
    }
    void emit_message(BridgeMessage msg) {
        for (const auto& h : message_handlers_) if (h) h(msg);
    }
    void emit_error(TransportError err) {
        for (const auto& h : error_handlers_) if (h) h(err);
    }
};


class WebSocketTransport : public BridgeTransport {
    std::string url_;
    std::vector<std::string> outbound_frames_;
    uint32_t reconnect_attempts_{0};
    uint32_t max_reconnects_{10};
public:
    auto connect(std::string_view url, std::optional<std::string_view> token)
        -> std::expected<void, TransportError> override {
        url_ = std::string(url);
        set_state(TransportState::connecting);
        (void)token;
        set_state(TransportState::connected);
        reconnect_attempts_ = 0;
        return {};
    }
    
    void disconnect() override {
        if (state_ == TransportState::connected) {
            outbound_frames_.push_back(R"({"type":"close","code":1000})");
        }
        set_state(TransportState::disconnected);
    }
    
    auto send(BridgeMessage msg) -> std::expected<void, TransportError> override {
        if (state_ != TransportState::connected)
            return std::unexpected(TransportError{TransportError::connection_refused, "未连接"});
        outbound_frames_.push_back(serialize_message(msg));
        flush_gate_.enqueue(std::move(msg));
        return {};
    }

    [[nodiscard]] auto sent_frames() const -> const std::vector<std::string>& { return outbound_frames_; }
    [[nodiscard]] auto max_reconnects() const -> uint32_t { return max_reconnects_; }
};


class HttpPollingTransport : public BridgeTransport {
    std::string base_url_;
    std::chrono::milliseconds poll_interval_{1000};
    std::vector<std::string> posted_messages_;
public:
    auto connect(std::string_view url, std::optional<std::string_view> token)
        -> std::expected<void, TransportError> override {
        base_url_ = std::string(url);
        set_state(TransportState::connecting);
        (void)token;
        set_state(TransportState::connected);
        return {};
    }
    void disconnect() override { set_state(TransportState::disconnected); }
    auto send(BridgeMessage msg) -> std::expected<void, TransportError> override {
        if (state_ != TransportState::connected)
            return std::unexpected(TransportError{TransportError::connection_refused, "未连接"});
        posted_messages_.push_back(serialize_message(msg));
        flush_gate_.enqueue(std::move(msg));
        return {};
    }
    [[nodiscard]] auto posted_messages() const -> const std::vector<std::string>& { return posted_messages_; }
    [[nodiscard]] auto poll_interval() const -> std::chrono::milliseconds { return poll_interval_; }
};


class StdioTransport : public BridgeTransport {
public:
    auto connect(std::string_view, std::optional<std::string_view>)
        -> std::expected<void, TransportError> override {
        set_state(TransportState::connected);
        return {};
    }
    void disconnect() override { set_state(TransportState::disconnected); }
    auto send(BridgeMessage msg) -> std::expected<void, TransportError> override {
        if (state_ != TransportState::connected)
            return std::unexpected(TransportError{TransportError::connection_refused, "未连接"});
        std::println(std::cout, "{}", serialize_message(msg));
        flush_gate_.enqueue(std::move(msg));
        return {};
    }
};

} // namespace cc::bridge
