// C++23 Module: CLI transport layer

module;
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module cc.cli.transports;


export namespace cc::cli::transports {


enum class TransportState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Closed
};


enum class TransportType : uint8_t {
    Stdio,
    Sse,        // Server-Sent Events
    WebSocket,
    Hybrid
};


struct TransportConfig {
    TransportType type{TransportType::Stdio};
    std::string url;
    std::string auth_token;
    uint32_t timeout_ms{30000};
    uint32_t reconnect_delay_ms{1000};
    uint32_t max_reconnect_attempts{5};
    bool auto_reconnect{true};
};


struct Message {
    std::string id;
    std::string method;   // JSON-RPC method
    std::string payload;
    bool is_notification{false};

    [[nodiscard]] bool is_response() const { return !id.empty() && method.empty(); }
    [[nodiscard]] bool is_request() const { return !method.empty(); }
};


template<typename T>
concept Transport = requires(T t, std::string_view data, Message msg) {
    { t.send(data) } -> std::same_as<std::expected<void, std::string>>;
    { t.receive() } -> std::same_as<std::expected<Message, std::string>>;
    { t.close() } -> std::same_as<void>;
    { t.state() } -> std::same_as<TransportState>;
    { t.is_connected() } -> std::same_as<bool>;
};


class StdioTransport {
public:
    explicit StdioTransport(TransportConfig config = {})
        : config_(std::move(config)) {
        state_ = TransportState::Connected;
    }


    [[nodiscard]] std::expected<void, std::string> send(std::string_view data) {
        if (state_ != TransportState::Connected) {
            return std::unexpected("Transport not connected");
        }

        auto frame = std::format("Content-Length: {}\r\n\r\n{}", data.size(), data);

        output_buffer_.push_back(std::string(frame));
        return {};
    }


    [[nodiscard]] std::expected<Message, std::string> receive() {
        if (state_ != TransportState::Connected) {
            return std::unexpected("Transport not connected");
        }
        if (input_queue_.empty()) {
            return std::unexpected("No message available");
        }
        auto msg = std::move(input_queue_.front());
        input_queue_.erase(input_queue_.begin());
        return msg;
    }

    void close() { state_ = TransportState::Closed; }
    [[nodiscard]] TransportState state() const { return state_; }
    [[nodiscard]] bool is_connected() const { return state_ == TransportState::Connected; }


    void feed_input(std::string_view raw_data) {
        read_buffer_ += raw_data;
        parse_messages();
    }

private:
    TransportConfig config_;
    TransportState state_{TransportState::Disconnected};
    std::string read_buffer_;
    std::vector<Message> input_queue_;
    std::vector<std::string> output_buffer_;


    void parse_messages() {
        while (true) {
            auto header_end = read_buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) break;


            auto header = std::string_view(read_buffer_).substr(0, header_end);
            auto cl_pos = header.find("Content-Length: ");
            if (cl_pos == std::string_view::npos) {
                read_buffer_.erase(0, header_end + 4);
                continue;
            }
            auto len_str = header.substr(cl_pos + 16);
            auto len_end = len_str.find('\r');
            size_t content_length = std::stoul(std::string(len_str.substr(0, len_end)));

            size_t total = header_end + 4 + content_length;
            if (read_buffer_.size() < total) break;

            auto payload = read_buffer_.substr(header_end + 4, content_length);
            Message msg;
            msg.payload = payload;
            input_queue_.push_back(std::move(msg));
            read_buffer_.erase(0, total);
        }
    }
};


class SseTransport {
public:
    explicit SseTransport(TransportConfig config)
        : config_(std::move(config)) {}


    [[nodiscard]] std::expected<void, std::string> send(std::string_view data) {
        if (!is_connected()) {
            return std::unexpected("SSE transport not connected");
        }

        pending_sends_.emplace_back(data);
        return {};
    }


    [[nodiscard]] std::expected<Message, std::string> receive() {
        if (!is_connected()) {
            return std::unexpected("SSE transport not connected");
        }
        if (event_queue_.empty()) {
            return std::unexpected("No events available");
        }
        auto msg = std::move(event_queue_.front());
        event_queue_.erase(event_queue_.begin());
        return msg;
    }

    void close() {
        state_ = TransportState::Closed;
        event_queue_.clear();
    }

    [[nodiscard]] TransportState state() const { return state_; }
    [[nodiscard]] bool is_connected() const { return state_ == TransportState::Connected; }


    [[nodiscard]] std::expected<void, std::string> connect() {
        if (config_.url.empty()) {
            return std::unexpected("No URL configured for SSE transport");
        }
        state_ = TransportState::Connecting;

        state_ = TransportState::Connected;
        return {};
    }


    void feed_event(std::string_view event_data) {

        if (event_data.starts_with("data: ")) {
            Message msg;
            msg.payload = std::string(event_data.substr(6));
            event_queue_.push_back(std::move(msg));
        }
    }

private:
    TransportConfig config_;
    TransportState state_{TransportState::Disconnected};
    std::vector<Message> event_queue_;
    std::vector<std::string> pending_sends_;
};


class WebSocketTransport {
public:
    explicit WebSocketTransport(TransportConfig config)
        : config_(std::move(config)) {}

    [[nodiscard]] std::expected<void, std::string> send(std::string_view data) {
        if (!is_connected()) {
            return std::unexpected("WebSocket not connected");
        }

        send_queue_.emplace_back(data);
        return {};
    }

    [[nodiscard]] std::expected<Message, std::string> receive() {
        if (!is_connected()) {
            return std::unexpected("WebSocket not connected");
        }
        if (recv_queue_.empty()) {
            return std::unexpected("No message available");
        }
        auto msg = std::move(recv_queue_.front());
        recv_queue_.erase(recv_queue_.begin());
        return msg;
    }

    void close() {

        state_ = TransportState::Closed;
    }

    [[nodiscard]] TransportState state() const { return state_; }
    [[nodiscard]] bool is_connected() const { return state_ == TransportState::Connected; }


    [[nodiscard]] std::expected<void, std::string> connect() {
        if (config_.url.empty()) {
            return std::unexpected("No URL configured for WebSocket");
        }
        state_ = TransportState::Connecting;

        state_ = TransportState::Connected;
        return {};
    }


    void feed_frame(std::string_view frame_data) {
        Message msg;
        msg.payload = std::string(frame_data);
        recv_queue_.push_back(std::move(msg));
    }

private:
    TransportConfig config_;
    TransportState state_{TransportState::Disconnected};
    std::vector<std::string> send_queue_;
    std::vector<Message> recv_queue_;
};


class HybridTransport {
public:
    explicit HybridTransport(TransportConfig config)
        : config_(std::move(config)) {
        select_transport();
    }

    [[nodiscard]] std::expected<void, std::string> send(std::string_view data) {
        return std::visit([&](auto& transport) { return transport.send(data); }, active_);
    }

    [[nodiscard]] std::expected<Message, std::string> receive() {
        return std::visit([&](auto& transport) { return transport.receive(); }, active_);
    }

    void close() {
        std::visit([](auto& transport) { transport.close(); }, active_);
    }

    [[nodiscard]] TransportState state() const {
        return std::visit([](const auto& transport) { return transport.state(); }, active_);
    }

    [[nodiscard]] bool is_connected() const {
        return std::visit([](const auto& transport) { return transport.is_connected(); }, active_);
    }

    [[nodiscard]] TransportType active_type() const { return active_type_; }

private:
    TransportConfig config_;
    TransportType active_type_{TransportType::Stdio};
    std::variant<StdioTransport, SseTransport, WebSocketTransport> active_{
        StdioTransport{}};


    void select_transport() {
        switch (config_.type) {
            case TransportType::Stdio:
                active_ = StdioTransport(config_);
                active_type_ = TransportType::Stdio;
                break;
            case TransportType::Sse:
                active_ = SseTransport(config_);
                active_type_ = TransportType::Sse;
                break;
            case TransportType::WebSocket:
                active_ = WebSocketTransport(config_);
                active_type_ = TransportType::WebSocket;
                break;
            case TransportType::Hybrid:

                if (!config_.url.empty()) {
                    active_ = WebSocketTransport(config_);
                    active_type_ = TransportType::WebSocket;
                } else {
                    active_ = StdioTransport(config_);
                    active_type_ = TransportType::Stdio;
                }
                break;
        }
    }
};


static_assert(Transport<StdioTransport>);
static_assert(Transport<SseTransport>);
static_assert(Transport<WebSocketTransport>);
static_assert(Transport<HybridTransport>);

} // namespace cc::cli::transports
