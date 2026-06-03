/// @file voice_stream_stt.cppm


module;

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <random>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

export module cc.services.voice_stream_stt;

import cc.utils.error;

export namespace cc::services::voice_stream_stt {

using cc::utils::Error;
using cc::utils::ErrorCode;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================

// ============================================================

constexpr std::string_view VOICE_STREAM_PATH = "/api/ws/speech_to_text/voice_stream";
constexpr std::uint32_t KEEPALIVE_INTERVAL_MS = 8000;
constexpr std::uint32_t FINALIZE_TIMEOUT_SAFETY_MS = 5000;
constexpr std::uint32_t FINALIZE_TIMEOUT_NO_DATA_MS = 1500;

// ============================================================

// ============================================================


enum class ConnectionState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Finalizing,
    Closed
};


using TranscriptCallback = std::function<void(std::string_view text, bool is_final)>;

using ErrorCallback = std::function<void(std::string_view error, bool is_fatal)>;

using CloseCallback = std::function<void()>;

using ReadyCallback = std::function<void()>;


struct VoiceStreamConfig {
    std::string language = "en";
    std::vector<std::string> keyterms;
    bool use_conversation_engine = false;
    std::string stt_provider;
};


enum class FinalizeSource : std::uint8_t {
    PostClosestreamEndpoint,
    NoDataTimeout,
    SafetyTimeout,
    WebSocketClose,
    WebSocketAlreadyClosed
};

// ============================================================

// ============================================================

class VoiceStreamSTTService {
public:
    explicit VoiceStreamSTTService() = default;

    ~VoiceStreamSTTService() {
        close();
    }

    VoiceStreamSTTService(const VoiceStreamSTTService&) = delete;
    VoiceStreamSTTService& operator=(const VoiceStreamSTTService&) = delete;


    [[nodiscard]] static bool is_available() noexcept {
        return std::getenv("ANTHROPIC_API_KEY") != nullptr ||
               std::getenv("CLAUDE_CODE_OAUTH_TOKEN") != nullptr ||
               std::getenv("CLAUDE_CODE_OAUTH_REFRESH_TOKEN") != nullptr;
    }


    std::expected<void, Error> connect(
        const VoiceStreamConfig& config,
        TranscriptCallback on_transcript,
        ErrorCallback on_error,
        CloseCallback on_close,
        ReadyCallback on_ready)
    {
        if (state_.load() != ConnectionState::Disconnected) {
            return std::unexpected(Error(ErrorCode::invalid_argument, "already connected"));
        }

        state_.store(ConnectionState::Connecting);
        config_ = config;
        on_transcript_ = std::move(on_transcript);
        on_error_ = std::move(on_error);
        on_close_ = std::move(on_close);
        on_ready_ = std::move(on_ready);

        // Resolve STT host from environment or default
        auto* host_env = std::getenv("CC_STT_HOST");
        std::string host = host_env ? host_env : "api.anthropic.com";
        uint16_t port = 443;

        // Establish TCP connection
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        auto port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
            state_.store(ConnectionState::Disconnected);
            return std::unexpected(Error(ErrorCode::network_error, "DNS resolution failed for STT host"));
        }

        socket_fd_ = -1;
        for (auto* rp = res; rp; rp = rp->ai_next) {
            socket_fd_ = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (socket_fd_ < 0) continue;
            struct timeval tv{}; tv.tv_sec = 10;
            setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (::connect(socket_fd_, rp->ai_addr, rp->ai_addrlen) == 0) break;
            ::close(socket_fd_); socket_fd_ = -1;
        }
        freeaddrinfo(res);

        if (socket_fd_ < 0) {
            state_.store(ConnectionState::Disconnected);
            return std::unexpected(Error(ErrorCode::network_error, "Failed to connect to STT host"));
        }

        // Send WebSocket upgrade request
        std::string path = std::string(VOICE_STREAM_PATH);
        path += "?language=" + config_.language;
        if (!config_.stt_provider.empty()) path += "&provider=" + config_.stt_provider;

        // Generate random WebSocket key
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint8_t> dist(0, 255);
        uint8_t key_bytes[16];
        for (auto& b : key_bytes) b = dist(gen);
        // Simple base64 for the key (16 bytes → 24 chars)
        static constexpr char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string ws_key;
        for (int i = 0; i < 16; i += 3) {
            uint32_t triple = (static_cast<uint32_t>(key_bytes[i]) << 16);
            if (i+1 < 16) triple |= (static_cast<uint32_t>(key_bytes[i+1]) << 8);
            if (i+2 < 16) triple |= key_bytes[i+2];
            ws_key += b64[(triple >> 18) & 0x3F];
            ws_key += b64[(triple >> 12) & 0x3F];
            ws_key += (i+1 < 16) ? b64[(triple >> 6) & 0x3F] : '=';
            ws_key += (i+2 < 16) ? b64[triple & 0x3F] : '=';
        }

        std::string upgrade = "GET " + path + " HTTP/1.1\r\n";
        upgrade += "Host: " + host + "\r\n";
        upgrade += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
        upgrade += "Sec-WebSocket-Key: " + ws_key + "\r\n";
        upgrade += "Sec-WebSocket-Version: 13\r\n";

        // Add auth token
        auto* token = std::getenv("ANTHROPIC_API_KEY");
        if (token) upgrade += std::string("x-api-key: ") + token + "\r\n";
        auto* oauth = std::getenv("CLAUDE_CODE_OAUTH_TOKEN");
        if (oauth) upgrade += std::string("Authorization: Bearer ") + oauth + "\r\n";

        upgrade += "\r\n";
        ::send(socket_fd_, upgrade.data(), upgrade.size(), 0);

        // Read upgrade response
        std::string resp_buf;
        char c;
        while (resp_buf.size() < 4096) {
            if (::recv(socket_fd_, &c, 1, 0) <= 0) break;
            resp_buf += c;
            if (resp_buf.ends_with("\r\n\r\n")) break;
        }

        if (resp_buf.find("101") == std::string::npos) {
            ::close(socket_fd_); socket_fd_ = -1;
            state_.store(ConnectionState::Disconnected);
            return std::unexpected(Error(ErrorCode::network_error, "WebSocket upgrade rejected"));
        }

        state_.store(ConnectionState::Connected);
        finalized_.store(false);
        finalizing_.store(false);

        // Start receiver thread
        recv_thread_ = std::thread([this]() { receive_loop(); });

        // Start keepalive thread
        keepalive_active_.store(true);
        keepalive_thread_ = std::thread([this]() { keepalive_loop(); });

        if (on_ready_) on_ready_();
        return {};
    }


    void send_audio(const std::vector<std::uint8_t>& audio_data) {
        if (state_.load() != ConnectionState::Connected) return;
        if (finalized_.load()) return;

        // Send as WebSocket binary frame (opcode 0x02, masked)
        send_ws_frame(0x02, audio_data.data(), audio_data.size());
    }


    FinalizeSource finalize() {
        if (finalizing_.exchange(true)) {
            return FinalizeSource::WebSocketAlreadyClosed;
        }

        finalized_.store(true);
        state_.store(ConnectionState::Finalizing);

        // Send CloseStream control message as text frame
        std::string close_msg = R"({"type":"CloseStream"})";
        send_ws_frame(0x01, reinterpret_cast<const uint8_t*>(close_msg.data()), close_msg.size());

        // Wait for final transcript (with timeout)
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(FINALIZE_TIMEOUT_SAFETY_MS);

        while (std::chrono::steady_clock::now() < deadline) {
            if (state_.load() == ConnectionState::Closed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return FinalizeSource::PostClosestreamEndpoint;
    }


    void close() noexcept {
        finalized_.store(true);
        keepalive_active_.store(false);

        if (state_.load() == ConnectionState::Connected ||
            state_.load() == ConnectionState::Finalizing) {
            // Send WebSocket close frame
            uint8_t close_payload[2] = {0x03, 0xE8};  // 1000 = normal closure
            send_ws_frame(0x08, close_payload, 2);
        }

        state_.store(ConnectionState::Closed);

        if (socket_fd_ >= 0) {
            ::shutdown(socket_fd_, SHUT_RDWR);
            ::close(socket_fd_);
            socket_fd_ = -1;
        }

        if (recv_thread_.joinable()) recv_thread_.join();
        if (keepalive_thread_.joinable()) keepalive_thread_.join();

        if (on_close_) on_close_();
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return state_.load() == ConnectionState::Connected;
    }

private:
    /// Send a WebSocket frame (masked, per RFC 6455)
    void send_ws_frame(uint8_t opcode, const uint8_t* payload, size_t len) {
        if (socket_fd_ < 0) return;
        std::lock_guard lock(send_mutex_);

        std::vector<uint8_t> frame;
        frame.push_back(0x80 | opcode);  // FIN + opcode

        // Length encoding + mask bit
        if (len < 126) {
            frame.push_back(static_cast<uint8_t>(0x80 | len));
        } else if (len <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }

        // Masking key
        std::random_device rd;
        uint8_t mask[4];
        auto r = rd();
        std::memcpy(mask, &r, 4);
        frame.insert(frame.end(), mask, mask + 4);

        // Masked payload
        for (size_t i = 0; i < len; ++i) {
            frame.push_back(payload[i] ^ mask[i % 4]);
        }

        ::send(socket_fd_, frame.data(), frame.size(), 0);
    }

    /// Receive loop — reads WebSocket frames and dispatches
    void receive_loop() {
        while (state_.load() == ConnectionState::Connected ||
               state_.load() == ConnectionState::Finalizing) {
            // Read frame header
            uint8_t hdr[2];
            if (recv_exact(hdr, 2) != 2) break;

            bool fin = (hdr[0] & 0x80) != 0;
            uint8_t opcode = hdr[0] & 0x0F;
            bool masked = (hdr[1] & 0x80) != 0;
            uint64_t payload_len = hdr[1] & 0x7F;

            if (payload_len == 126) {
                uint8_t ext[2];
                if (recv_exact(ext, 2) != 2) break;
                payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
            } else if (payload_len == 127) {
                uint8_t ext[8];
                if (recv_exact(ext, 8) != 8) break;
                payload_len = 0;
                for (int i = 0; i < 8; ++i)
                    payload_len = (payload_len << 8) | ext[i];
            }

            uint8_t mask_key[4] = {};
            if (masked && recv_exact(mask_key, 4) != 4) break;

            // Read payload
            std::vector<uint8_t> payload(payload_len);
            if (payload_len > 0 && recv_exact(payload.data(), payload_len) != payload_len) break;

            if (masked) {
                for (size_t i = 0; i < payload_len; ++i)
                    payload[i] ^= mask_key[i % 4];
            }

            (void)fin;
            handle_frame(opcode, payload);
        }
    }

    ssize_t recv_exact(uint8_t* buf, size_t len) {
        size_t total = 0;
        while (total < len) {
            auto n = ::recv(socket_fd_, buf + total, len - total, 0);
            if (n <= 0) return static_cast<ssize_t>(total);
            total += static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(total);
    }

    void handle_frame(uint8_t opcode, const std::vector<uint8_t>& payload) {
        if (opcode == 0x01) {  // Text frame — JSON message
            std::string msg(payload.begin(), payload.end());
            parse_stt_message(msg);
        } else if (opcode == 0x08) {  // Close
            state_.store(ConnectionState::Closed);
        } else if (opcode == 0x09) {  // Ping → send Pong
            send_ws_frame(0x0A, payload.data(), payload.size());
        }
        // 0x0A = Pong — ignore
    }

    void parse_stt_message(const std::string& msg) {
        // Parse type from JSON: {"type": "TranscriptText|TranscriptEndpoint|Error", ...}
        auto type_pos = msg.find("\"type\"");
        if (type_pos == std::string::npos) return;
        auto colon = msg.find(':', type_pos);
        auto quote1 = msg.find('"', colon + 1);
        auto quote2 = msg.find('"', quote1 + 1);
        if (quote1 == std::string::npos || quote2 == std::string::npos) return;
        auto type = msg.substr(quote1 + 1, quote2 - quote1 - 1);

        // Extract text field
        auto extract_text = [&]() -> std::string {
            auto pos = msg.find("\"text\"");
            if (pos == std::string::npos) return {};
            auto c = msg.find(':', pos);
            auto q1 = msg.find('"', c + 1);
            auto q2 = msg.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) return {};
            return msg.substr(q1 + 1, q2 - q1 - 1);
        };

        if (type == "TranscriptText") {
            auto text = extract_text();
            if (!text.empty() && on_transcript_) {
                last_transcript_text_ = text;
                on_transcript_(text, false);
            }
        } else if (type == "TranscriptEndpoint") {
            auto text = extract_text();
            if (!text.empty()) last_transcript_text_ = text;
            if (on_transcript_) on_transcript_(last_transcript_text_, true);
        } else if (type == "Error") {
            auto text = extract_text();
            if (on_error_) on_error_(text.empty() ? "Unknown STT error" : text, true);
        } else if (type == "Ready") {
            // Server confirmed ready state
        }
    }

    /// Keepalive loop — send periodic pings
    void keepalive_loop() {
        while (keepalive_active_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(KEEPALIVE_INTERVAL_MS));
            if (!keepalive_active_.load()) break;
            if (state_.load() == ConnectionState::Connected) {
                std::string ka = R"({"type":"KeepAlive"})";
                send_ws_frame(0x01, reinterpret_cast<const uint8_t*>(ka.data()), ka.size());
            }
        }
    }

    VoiceStreamConfig config_;
    int socket_fd_ = -1;
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::atomic<bool> finalized_{false};
    std::atomic<bool> finalizing_{false};
    std::atomic<bool> keepalive_active_{false};

    std::mutex send_mutex_;
    std::thread recv_thread_;
    std::thread keepalive_thread_;

    TranscriptCallback on_transcript_;
    ErrorCallback on_error_;
    CloseCallback on_close_;
    ReadyCallback on_ready_;

    std::string last_transcript_text_;
};

// ============================================================

// ============================================================


[[nodiscard]] inline std::expected<std::unique_ptr<VoiceStreamSTTService>, Error> create_and_connect(
    const VoiceStreamConfig& config,
    TranscriptCallback on_transcript,
    ErrorCallback on_error,
    CloseCallback on_close,
    ReadyCallback on_ready)
{
    auto service = std::make_unique<VoiceStreamSTTService>();
    auto result = service->connect(config, std::move(on_transcript),
                                   std::move(on_error), std::move(on_close),
                                   std::move(on_ready));
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    return service;
}

} // namespace cc::services::voice_stream_stt
