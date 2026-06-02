// C++23 Module: CLI transport layer
// 传输层抽象：支持 Stdio/SSE/WebSocket 多种传输方式
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

// 传输状态
enum class TransportState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Closed
};

// 传输类型枚举
enum class TransportType : uint8_t {
    Stdio,      // 标准输入输出
    Sse,        // Server-Sent Events
    WebSocket,  // WebSocket 双向通信
    Hybrid      // 自动选择最佳传输
};

// 传输配置
struct TransportConfig {
    TransportType type{TransportType::Stdio};
    std::string url;            // SSE/WebSocket 的 URL
    std::string auth_token;     // 认证 token
    uint32_t timeout_ms{30000}; // 超时时间 (毫秒)
    uint32_t reconnect_delay_ms{1000};  // 重连延迟
    uint32_t max_reconnect_attempts{5}; // 最大重连次数
    bool auto_reconnect{true};
};

// 接收到的消息
struct Message {
    std::string id;       // 消息 ID
    std::string method;   // JSON-RPC method
    std::string payload;  // JSON 内容
    bool is_notification{false};  // 是否为通知

    [[nodiscard]] bool is_response() const { return !id.empty() && method.empty(); }
    [[nodiscard]] bool is_request() const { return !method.empty(); }
};

// Transport concept: 所有传输实现必须满足的接口约束
template<typename T>
concept Transport = requires(T t, std::string_view data, Message msg) {
    { t.send(data) } -> std::same_as<std::expected<void, std::string>>;
    { t.receive() } -> std::same_as<std::expected<Message, std::string>>;
    { t.close() } -> std::same_as<void>;
    { t.state() } -> std::same_as<TransportState>;
    { t.is_connected() } -> std::same_as<bool>;
};

// Stdio 传输：通过 stdin/stdout 的 JSON-RPC
class StdioTransport {
public:
    explicit StdioTransport(TransportConfig config = {})
        : config_(std::move(config)) {
        state_ = TransportState::Connected;  // stdio 始终可用
    }

    // 发送数据到 stdout (Content-Length 协议)
    [[nodiscard]] std::expected<void, std::string> send(std::string_view data) {
        if (state_ != TransportState::Connected) {
            return std::unexpected("Transport not connected");
        }
        // 格式: Content-Length: N\r\n\r\n{payload}
        auto frame = std::format("Content-Length: {}\r\n\r\n{}", data.size(), data);
        // 实际写入 stdout (libuv uv_write)
        output_buffer_.push_back(std::string(frame));
        return {};
    }

    // 从 stdin 接收消息
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

    // 供外部 IO loop 调用：将原始输入数据解析为消息
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

    // 解析 Content-Length 协议帧
    void parse_messages() {
        while (true) {
            auto header_end = read_buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) break;

            // 解析 Content-Length
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

// SSE 传输：Server-Sent Events over HTTP
class SseTransport {
public:
    explicit SseTransport(TransportConfig config)
        : config_(std::move(config)) {}

    // SSE 是单向的，发送通过独立的 POST 请求
    [[nodiscard]] std::expected<void, std::string> send(std::string_view data) {
        if (!is_connected()) {
            return std::unexpected("SSE transport not connected");
        }
        // 通过 HTTP POST 发送到 endpoint
        pending_sends_.emplace_back(data);
        return {};
    }

    // 从 SSE 事件流接收
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

    // 连接到 SSE endpoint
    [[nodiscard]] std::expected<void, std::string> connect() {
        if (config_.url.empty()) {
            return std::unexpected("No URL configured for SSE transport");
        }
        state_ = TransportState::Connecting;
        // libuv HTTP 连接逻辑
        state_ = TransportState::Connected;
        return {};
    }

    // 供外部 IO loop 调用：解析 SSE 事件
    void feed_event(std::string_view event_data) {
        // SSE 格式: "data: {...}\n\n"
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

// WebSocket 传输：双向通信
class WebSocketTransport {
public:
    explicit WebSocketTransport(TransportConfig config)
        : config_(std::move(config)) {}

    [[nodiscard]] std::expected<void, std::string> send(std::string_view data) {
        if (!is_connected()) {
            return std::unexpected("WebSocket not connected");
        }
        // 构建 WebSocket 帧并发送
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
        // 发送 WebSocket close frame
        state_ = TransportState::Closed;
    }

    [[nodiscard]] TransportState state() const { return state_; }
    [[nodiscard]] bool is_connected() const { return state_ == TransportState::Connected; }

    // 连接
    [[nodiscard]] std::expected<void, std::string> connect() {
        if (config_.url.empty()) {
            return std::unexpected("No URL configured for WebSocket");
        }
        state_ = TransportState::Connecting;
        // WebSocket 握手 (libuv + HTTP upgrade)
        state_ = TransportState::Connected;
        return {};
    }

    // 供外部 IO loop 调用
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

// 混合传输：自动选择最佳传输方式
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

    // 根据配置选择传输方式
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
                // 自动检测: 有 URL 则用 WebSocket，否则 Stdio
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

// 验证 concept 约束
static_assert(Transport<StdioTransport>);
static_assert(Transport<SseTransport>);
static_assert(Transport<WebSocketTransport>);
static_assert(Transport<HybridTransport>);

} // namespace cc::cli::transports
