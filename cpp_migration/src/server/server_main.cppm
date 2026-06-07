module;
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <openssl/sha.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <arpa/inet.h>

export module cc.server.server_main;

import cc.server.server_routes;
import cc.hooks.tool_permissions;
import cc.session.storage;
import cc.utils.json;

export namespace cc::server {

struct ServerConfig {
    uint16_t port = 3000;
    std::string host = "127.0.0.1";
    bool cors = false;
    std::optional<std::string> auth_token;
};

namespace detail {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct WsFrame {
    std::uint8_t opcode = 0;
    std::string payload;
};

[[nodiscard]] inline std::string lowercase(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

[[nodiscard]] inline std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

[[nodiscard]] inline std::string server_json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out += ch; break;
        }
    }
    return out;
}

[[nodiscard]] inline int from_hex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

[[nodiscard]] inline std::string url_decode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = from_hex(value[i + 1]);
            const int lo = from_hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

[[nodiscard]] inline std::map<std::string, std::string> parse_query(std::string_view target) {
    std::map<std::string, std::string> params;
    const auto question = target.find('?');
    if (question == std::string_view::npos) return params;
    auto query = target.substr(question + 1);
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto amp = query.find('&', start);
        const auto part = query.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
        if (!part.empty()) {
            const auto eq = part.find('=');
            auto key = url_decode(eq == std::string_view::npos ? part : part.substr(0, eq));
            auto value = eq == std::string_view::npos ? std::string{} : url_decode(part.substr(eq + 1));
            if (!key.empty()) params[std::move(key)] = std::move(value);
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return params;
}

[[nodiscard]] inline std::string path_without_query(std::string_view target) {
    const auto question = target.find('?');
    return std::string(target.substr(0, question == std::string_view::npos ? target.size() : question));
}

[[nodiscard]] inline bool send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        auto sent = ::send(fd, data.data(), data.size(), 0);
        if (sent <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

[[nodiscard]] inline bool read_exact(int fd, char* out, std::size_t size) {
    while (size > 0) {
        auto n = ::recv(fd, out, size, 0);
        if (n <= 0) return false;
        out += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

[[nodiscard]] inline std::optional<HttpRequest> read_http_request(int fd) {
    std::string raw;
    std::array<char, 4096> buffer{};
    std::size_t header_end = std::string::npos;
    while ((header_end = raw.find("\r\n\r\n")) == std::string::npos) {
        auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) return std::nullopt;
        raw.append(buffer.data(), static_cast<std::size_t>(n));
        if (raw.size() > 1024 * 1024) return std::nullopt;
    }

    HttpRequest req;
    std::istringstream stream(raw.substr(0, header_end));
    std::string line;
    if (!std::getline(stream, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream first(line);
        first >> req.method >> req.target;
    }
    if (req.method.empty() || req.target.empty()) return std::nullopt;
    req.path = path_without_query(req.target);
    req.query = parse_query(req.target);

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = lowercase(trim(std::string_view(line).substr(0, colon)));
        auto value = trim(std::string_view(line).substr(colon + 1));
        req.headers[std::move(key)] = std::move(value);
    }

    std::size_t content_length = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
        try {
            content_length = static_cast<std::size_t>(std::stoull(it->second));
        } catch (...) {
            content_length = 0;
        }
    }

    const auto body_start = header_end + 4;
    while (raw.size() - body_start < content_length) {
        auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) return std::nullopt;
        raw.append(buffer.data(), static_cast<std::size_t>(n));
        if (raw.size() > content_length + 1024 * 1024) return std::nullopt;
    }
    if (content_length > 0) req.body = raw.substr(body_start, content_length);
    return req;
}

inline void add_json_value_to_params(
    std::map<std::string, std::string>& params,
    std::string key,
    cc::utils::json::JsonVal value
) {
    if (value.is_str()) {
        params[std::move(key)] = std::string(value.as_str());
    } else if (value.is_bool()) {
        params[std::move(key)] = value.as_bool() ? "true" : "false";
    } else if (value.is_num()) {
        params[std::move(key)] = std::to_string(value.as_int());
    }
}

[[nodiscard]] inline std::map<std::string, std::string> request_params(const HttpRequest& request) {
    auto params = request.query;
    if (request.body.empty()) return params;

    auto parsed = cc::utils::json::parse(request.body);
    if (!parsed || !parsed->root().is_obj()) return params;
    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        add_json_value_to_params(params, std::string(key.as_str()), value);
    });
    return params;
}

[[nodiscard]] inline std::string base64_encode(const unsigned char* data, std::size_t len) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(table[(n >> 18) & 0x3f]);
        out.push_back(table[(n >> 12) & 0x3f]);
        out.push_back((i + 1 < len) ? table[(n >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < len) ? table[n & 0x3f] : '=');
    }
    return out;
}

[[nodiscard]] inline std::string websocket_accept_key(std::string_view key) {
    const std::string input = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> hash{};
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash.data());
    return base64_encode(hash.data(), hash.size());
}

[[nodiscard]] inline std::optional<WsFrame> read_ws_frame(int fd) {
    unsigned char header[2]{};
    if (!read_exact(fd, reinterpret_cast<char*>(header), 2)) return std::nullopt;

    WsFrame frame;
    frame.opcode = header[0] & 0x0f;
    const bool masked = (header[1] & 0x80) != 0;
    std::uint64_t len = header[1] & 0x7f;
    if (len == 126) {
        unsigned char ext[2]{};
        if (!read_exact(fd, reinterpret_cast<char*>(ext), 2)) return std::nullopt;
        len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8]{};
        if (!read_exact(fd, reinterpret_cast<char*>(ext), 8)) return std::nullopt;
        len = 0;
        for (unsigned char byte : ext) len = (len << 8) | byte;
    }
    if (len > 16 * 1024 * 1024) return std::nullopt;

    std::array<unsigned char, 4> mask{};
    if (masked && !read_exact(fd, reinterpret_cast<char*>(mask.data()), mask.size())) {
        return std::nullopt;
    }

    frame.payload.resize(static_cast<std::size_t>(len));
    if (len > 0 && !read_exact(fd, frame.payload.data(), frame.payload.size())) {
        return std::nullopt;
    }
    if (masked) {
        for (std::size_t i = 0; i < frame.payload.size(); ++i) {
            frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i % 4]);
        }
    }
    return frame;
}

[[nodiscard]] inline std::optional<WsFrame> read_ws_frame_available(
    int fd,
    std::chrono::milliseconds timeout,
    bool& closed
) {
    closed = false;
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const auto timeout_ms = static_cast<int>(std::max<std::int64_t>(0, timeout.count()));
    const auto ready = ::poll(&pfd, 1, timeout_ms);
    if (ready == 0) return std::nullopt;
    if (ready < 0) {
        closed = true;
        return std::nullopt;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        closed = true;
        return std::nullopt;
    }
    auto frame = read_ws_frame(fd);
    if (!frame) closed = true;
    return frame;
}

[[nodiscard]] inline bool send_ws_frame(int fd, std::uint8_t opcode, std::string_view payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    const auto len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xffff) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xff));
        frame.push_back(static_cast<char>(len & 0xff));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((len >> shift) & 0xff));
        }
    }
    frame.append(payload);
    return send_all(fd, frame);
}

[[nodiscard]] inline bool send_ws_text_line(int fd, std::string_view payload) {
    std::string line(payload);
    if (!line.ends_with('\n')) line.push_back('\n');
    return send_ws_frame(fd, 0x1, line);
}

[[nodiscard]] inline std::string default_sessions_dir_string() {
    if (const char* env = std::getenv("CC_REPL_SERVER_SESSIONS_DIR"); env && *env) {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return (std::filesystem::path{home} / ".config" / "claude" / "sessions").string();
    }
    return (std::filesystem::current_path() / ".claude" / "sessions").string();
}

[[nodiscard]] inline std::string make_id(std::string_view prefix) {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::format("{}_{}_{}", prefix, now, counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

[[nodiscard]] inline std::string public_host_for_url(std::string_view host) {
    if (host.empty() || host == "0.0.0.0" || host == "::") return "127.0.0.1";
    if (host == "localhost") return "127.0.0.1";
    return std::string(host);
}

[[nodiscard]] inline std::string title_from_cwd(const std::filesystem::path& cwd) {
    auto filename = cwd.filename().string();
    return filename.empty() ? std::string("Direct connect session") : "Direct connect: " + filename;
}

[[nodiscard]] inline std::string create_session_response(
    const ServerConfig& config,
    const std::map<std::string, std::string>& params
) {
    std::filesystem::path cwd = std::filesystem::current_path();
    if (auto it = params.find("cwd"); it != params.end() && !it->second.empty()) {
        cwd = std::filesystem::path{it->second};
        if (cwd.is_relative()) cwd = std::filesystem::current_path() / cwd;
    }

    std::error_code ec;
    cwd = std::filesystem::weakly_canonical(cwd, ec);
    if (ec || !std::filesystem::exists(cwd) || !std::filesystem::is_directory(cwd)) {
        std::ostringstream out;
        out << R"({"error":"invalid cwd","message":")" << server_json_escape(cwd.string()) << R"("})";
        return out.str();
    }

    const auto now = std::chrono::system_clock::now();
    cc::session::SessionMetadata metadata{
        .session_id = make_id("server"),
        .model = params.contains("model") ? params.at("model") : std::string("default"),
        .cwd = cwd,
        .created_at = now,
        .last_active = now,
        .message_count = 0,
        .title = title_from_cwd(cwd),
        .is_archived = false,
    };
    const auto sessions_dir = std::filesystem::path{default_sessions_dir_string()};
    if (!cc::session::save_session_metadata(sessions_dir, metadata)) {
        return R"({"error":"failed to save session metadata"})";
    }

    std::ostringstream out;
    out << R"({"session_id":")" << server_json_escape(metadata.session_id)
        << R"(","ws_url":"ws://)" << server_json_escape(public_host_for_url(config.host))
        << ":" << config.port << "/sessions/ws/" << server_json_escape(metadata.session_id)
        << R"(","work_dir":")" << server_json_escape(cwd.string()) << "\"}";
    return out.str();
}

[[nodiscard]] inline std::optional<std::string> extract_user_content(std::string_view payload) {
    auto parsed = cc::utils::json::parse(payload);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto type = root.get("type");
    if (!type.is_str() || type.as_str() != std::string_view("user")) return std::nullopt;

    auto message = root.get("message");
    if (!message.is_obj()) return std::nullopt;
    auto content = message.get("content");
    if (content.is_str()) return std::string(content.as_str());
    if (content.is_arr()) {
        std::string out;
        content.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) {
                if (!out.empty()) out += "\n";
                out += std::string(item.as_str());
            } else if (item.is_obj()) {
                auto text = item.get("text");
                if (text.is_str()) {
                    if (!out.empty()) out += "\n";
                    out += std::string(text.as_str());
                }
            }
        });
        return out;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> control_request_subtype(std::string_view payload) {
    auto parsed = cc::utils::json::parse(payload);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto type = root.get("type");
    if (!type.is_str() || type.as_str() != std::string_view("control_request")) return std::nullopt;
    auto request = root.get("request");
    if (!request.is_obj()) return std::nullopt;
    auto subtype = request.get("subtype");
    if (!subtype.is_str()) return std::nullopt;
    return std::string(subtype.as_str());
}

[[nodiscard]] inline std::string control_request_id(std::string_view payload) {
    auto parsed = cc::utils::json::parse(payload);
    if (!parsed || !parsed->root().is_obj()) return make_id("control");
    auto id = parsed->root().get("request_id");
    return id.is_str() ? std::string(id.as_str()) : make_id("control");
}

struct ControlResponseDecision {
    std::string request_id;
    cc::hooks::PermissionResponse response;
};

[[nodiscard]] inline std::optional<ControlResponseDecision> control_response_decision(
    std::string_view payload
) {
    auto parsed = cc::utils::json::parse(payload);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto type = root.get("type");
    if (!type.is_str() || type.as_str() != std::string_view("control_response")) return std::nullopt;
    auto response = root.get("response");
    if (!response.is_obj()) return std::nullopt;
    auto request_id = response.get("request_id");
    if (!request_id.is_str()) return std::nullopt;
    cc::hooks::PermissionResponse permission_response{};
    permission_response.decision = cc::hooks::PermissionDecision::deny;
    auto subtype = response.get("subtype");
    if (subtype.is_str() && subtype.as_str() == std::string_view("error")) {
        auto error = response.get("error");
        if (error.is_str()) {
            permission_response.message = std::string(error.as_str());
        }
        return ControlResponseDecision{
            .request_id = std::string(request_id.as_str()),
            .response = std::move(permission_response),
        };
    }
    auto body = response.get("response");
    if (body.is_obj()) {
        auto behavior = body.get("behavior");
        if (behavior.is_str() && behavior.as_str() == std::string_view("allow")) {
            permission_response.decision = cc::hooks::PermissionDecision::allow;
        }
        auto updated_input = body.get("updatedInput");
        if (updated_input.is_obj()) {
            permission_response.updated_input_json = updated_input.to_string();
        }
        auto updated_permissions = body.get("updatedPermissions");
        if (updated_permissions.is_arr()) {
            permission_response.updated_permissions_json = updated_permissions.to_string();
        }
        auto message = body.get("message");
        if (message.is_str()) {
            permission_response.message = std::string(message.as_str());
        }
    }
    return ControlResponseDecision{
        .request_id = std::string(request_id.as_str()),
        .response = std::move(permission_response),
    };
}

[[nodiscard]] inline std::string permission_input_json(std::string_view input_json) {
    auto parsed = cc::utils::json::parse(input_json);
    if (!parsed || !parsed->root().is_obj()) return "{}";
    return cc::utils::json::to_string(parsed->root());
}

[[nodiscard]] inline std::string permission_control_request_json(
    const detail::DirectPermissionRequest& request
) {
    std::ostringstream out;
    out << R"({"type":"control_request","request_id":")" << server_json_escape(request.request_id)
        << R"(","request":{"subtype":"can_use_tool","tool_name":")" << server_json_escape(request.tool_name)
        << R"(","input":)" << permission_input_json(request.input_json)
        << R"(,"tool_use_id":")" << server_json_escape(request.tool_use_id)
        << R"(","description":"Allow )" << server_json_escape(request.tool_name)
        << R"( to run?"}})";
    return out.str();
}

[[nodiscard]] inline std::string sdk_assistant_message(
    std::string_view session_id,
    cc::utils::json::JsonVal response
) {
    const auto uuid = response.get("id").is_str()
        ? std::string(response.get("id").as_str())
        : make_id("msg");
    const auto text = response.get("response").is_str()
        ? std::string(response.get("response").as_str())
        : std::string{};
    const auto model = response.get("model").is_str()
        ? std::string(response.get("model").as_str())
        : std::string("default");
    std::ostringstream out;
    out << R"({"type":"assistant","message":{"id":")" << server_json_escape(uuid)
        << R"(","role":"assistant","model":")" << server_json_escape(model)
        << R"(","content":[{"type":"text","text":")" << server_json_escape(text)
        << R"("}]},"parent_tool_use_id":null,"uuid":")" << server_json_escape(uuid)
        << R"(","session_id":")" << server_json_escape(session_id) << "\"}";
    return out.str();
}

[[nodiscard]] inline std::string sdk_result_message(
    std::string_view session_id,
    cc::utils::json::JsonVal response
) {
    const auto uuid = make_id("result");
    const auto text = response.get("response").is_str()
        ? std::string(response.get("response").as_str())
        : std::string{};
    const auto elapsed = response.get("elapsed_ms").is_num() ? response.get("elapsed_ms").as_int() : 0;
    auto usage = response.get("usage");
    const auto input_tokens = usage.is_obj() && usage.get("input_tokens").is_num()
        ? usage.get("input_tokens").as_int()
        : 0;
    const auto output_tokens = usage.is_obj() && usage.get("output_tokens").is_num()
        ? usage.get("output_tokens").as_int()
        : 0;
    std::ostringstream out;
    out << R"({"type":"result","subtype":"success","duration_ms":)" << elapsed
        << R"(,"duration_api_ms":)" << elapsed
        << R"(,"is_error":false,"num_turns":1,"result":")" << server_json_escape(text)
        << R"(","stop_reason":"end_turn","total_cost_usd":0)"
        << R"(,"usage":{"input_tokens":)" << input_tokens
        << R"(,"output_tokens":)" << output_tokens
        << R"(,"cache_creation_input_tokens":0,"cache_read_input_tokens":0,"server_tool_use":{"web_search_requests":0}})"
        << R"(,"modelUsage":{},"permission_denials":[],"uuid":")" << server_json_escape(uuid)
        << R"(","session_id":")" << server_json_escape(session_id) << "\"}";
    return out.str();
}

[[nodiscard]] inline std::string sdk_error_result(
    std::string_view session_id,
    std::string_view error
) {
    const auto uuid = make_id("result");
    std::ostringstream out;
    out << R"({"type":"result","subtype":"error_during_execution","duration_ms":0,"duration_api_ms":0)"
        << R"(,"is_error":true,"num_turns":1,"stop_reason":null,"total_cost_usd":0)"
        << R"(,"usage":{"input_tokens":0,"output_tokens":0,"cache_creation_input_tokens":0,"cache_read_input_tokens":0,"server_tool_use":{"web_search_requests":0}})"
        << R"(,"modelUsage":{},"permission_denials":[],"errors":[")" << server_json_escape(error)
        << R"("],"uuid":")" << server_json_escape(uuid)
        << R"(","session_id":")" << server_json_escape(session_id) << "\"}";
    return out.str();
}

[[nodiscard]] inline bool is_websocket_upgrade(const HttpRequest& request) {
    auto upgrade = request.headers.find("upgrade");
    if (upgrade == request.headers.end()) return false;
    return lowercase(upgrade->second) == "websocket";
}

} // namespace detail

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer() { stop(); }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    auto start(ServerConfig config) -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        if (running_.load()) return std::unexpected("Server is already running");
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return std::unexpected("Failed to create server socket");

        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config.port);
        if (config.host == "localhost") {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        } else if (::inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1) {
            ::close(fd);
            return std::unexpected("Server host must be an IPv4 address or localhost");
        }

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return std::unexpected("Failed to bind HTTP server");
        }
        if (::listen(fd, 64) != 0) {
            ::close(fd);
            return std::unexpected("Failed to listen on HTTP server socket");
        }

        socklen_t bound_len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &bound_len) == 0) {
            config.port = ntohs(addr.sin_port);
        }

        config_ = std::move(config);
        listen_fd_ = fd;
        routes_ = get_default_routes();
        running_.store(true);
        server_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
        return {};
    }

    void stop() {
        std::jthread worker;
        {
            std::lock_guard lock(mutex_);
            running_.store(false);
            if (listen_fd_ >= 0) {
                ::shutdown(listen_fd_, SHUT_RDWR);
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            worker = std::move(server_thread_);
        }
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
    }

    [[nodiscard]] bool is_running() const {
        return running_.load();
    }

    [[nodiscard]] std::string get_url() const {
        if (!running_.load()) return {};
        return "http://" + detail::public_host_for_url(config_.host) + ":" + std::to_string(config_.port);
    }

    [[nodiscard]] const ServerConfig& get_config() const {
        return config_;
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            std::thread([this, client_fd] {
                handle_client(client_fd);
                ::close(client_fd);
            }).detach();
        }
    }

    [[nodiscard]] bool authorized(const detail::HttpRequest& request) const {
        if (!config_.auth_token || config_.auth_token->empty()) return true;
        auto it = request.headers.find("authorization");
        if (it == request.headers.end()) return false;
        return it->second == "Bearer " + *config_.auth_token;
    }

    void send_http(
        int fd,
        std::string_view status,
        std::string_view body,
        std::string_view content_type = "application/json"
    ) const {
        std::ostringstream response;
        response << "HTTP/1.1 " << status << "\r\n"
                 << "Content-Type: " << content_type << "\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Connection: close\r\n";
        if (config_.cors) {
            response << "Access-Control-Allow-Origin: *\r\n"
                     << "Access-Control-Allow-Headers: authorization, content-type\r\n"
                     << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        }
        response << "\r\n" << body;
        (void)detail::send_all(fd, response.str());
    }

    void handle_client(int fd) {
        auto request = detail::read_http_request(fd);
        if (!request) {
            send_http(fd, "400 Bad Request", R"({"error":"bad request"})");
            return;
        }
        if (!authorized(*request)) {
            send_http(fd, "401 Unauthorized", R"({"error":"unauthorized"})");
            return;
        }
        if (detail::is_websocket_upgrade(*request)) {
            handle_websocket(fd, *request);
            return;
        }
        handle_http(fd, *request);
    }

    void handle_http(int fd, const detail::HttpRequest& request) {
        if (request.method == "OPTIONS") {
            send_http(fd, "204 No Content", "", "text/plain");
            return;
        }
        if (request.method == "GET" && request.path == "/health") {
            send_http(fd, "200 OK", R"({"status":"ok"})");
            return;
        }
        if (request.method == "GET" && request.path == "/status") {
            std::ostringstream out;
            out << R"({"status":"running","url":")" << detail::server_json_escape(get_url()) << "\"}";
            send_http(fd, "200 OK", out.str());
            return;
        }
        if (request.method == "POST" && request.path == "/sessions") {
            send_http(fd, "200 OK", detail::create_session_response(config_, detail::request_params(request)));
            return;
        }

        const auto params = detail::request_params(request);
        auto route_it = std::ranges::find_if(routes_, [&](const Route& route) {
            return route.method == request.method && route.path == request.path;
        });
        if (route_it == routes_.end()) {
            send_http(fd, "404 Not Found", R"({"error":"not found"})");
            return;
        }
        send_http(fd, "200 OK", route_it->handler(params));
    }

    void handle_websocket(int fd, const detail::HttpRequest& request) {
        constexpr std::string_view prefix = "/sessions/ws/";
        if (!request.path.starts_with(prefix)) {
            send_http(fd, "404 Not Found", R"({"error":"unknown websocket path"})");
            return;
        }
        auto key_it = request.headers.find("sec-websocket-key");
        if (key_it == request.headers.end() || key_it->second.empty()) {
            send_http(fd, "400 Bad Request", R"({"error":"missing websocket key"})");
            return;
        }

        const auto session_id = detail::url_decode(std::string_view(request.path).substr(prefix.size()));
        const auto accept = detail::websocket_accept_key(key_it->second);
        const auto response = std::format(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: {}\r\n\r\n",
            accept);
        if (!detail::send_all(fd, response)) return;

        run_websocket_session(fd, session_id);
    }

    void run_websocket_session(int fd, const std::string& session_id) {
        std::mutex ws_send_mutex;
        auto send_ws_text = [&](std::string_view payload) {
            std::lock_guard lock(ws_send_mutex);
            return detail::send_ws_text_line(fd, payload);
        };
        auto send_ws_frame = [&](std::uint8_t opcode, std::string_view payload) {
            std::lock_guard lock(ws_send_mutex);
            return detail::send_ws_frame(fd, opcode, payload);
        };

        std::mutex permission_mutex;
        std::condition_variable permission_cv;
        std::unordered_map<std::string, std::optional<cc::hooks::PermissionResponse>> pending_permissions;
        bool closing_permissions = false;

        auto record_control_response = [&](std::string_view payload) {
            auto decision = detail::control_response_decision(payload);
            if (!decision) return false;
            std::lock_guard lock(permission_mutex);
            auto it = pending_permissions.find(decision->request_id);
            if (it == pending_permissions.end()) return true;
            it->second = std::move(decision->response);
            permission_cv.notify_all();
            return true;
        };

        register_direct_permission_handler(
            session_id,
            [&](const detail::DirectPermissionRequest& request) -> cc::hooks::PermissionResponse {
                {
                    std::lock_guard lock(permission_mutex);
                    if (closing_permissions) {
                        cc::hooks::PermissionResponse response{};
                        response.decision = cc::hooks::PermissionDecision::deny;
                        response.message = "Permission request closed";
                        return response;
                    }
                    pending_permissions.emplace(request.request_id, std::nullopt);
                }

                if (!send_ws_text(detail::permission_control_request_json(request))) {
                    std::lock_guard lock(permission_mutex);
                    pending_permissions.erase(request.request_id);
                    cc::hooks::PermissionResponse response{};
                    response.decision = cc::hooks::PermissionDecision::deny;
                    response.message = "Failed to send permission request";
                    return response;
                }

                std::unique_lock lock(permission_mutex);
                const bool resolved = permission_cv.wait_for(lock, std::chrono::minutes(5), [&] {
                    auto it = pending_permissions.find(request.request_id);
                    return closing_permissions ||
                           (it != pending_permissions.end() && it->second.has_value());
                });
                auto it = pending_permissions.find(request.request_id);
                cc::hooks::PermissionResponse result{};
                result.decision = cc::hooks::PermissionDecision::deny;
                result.message = "Permission request timed out";
                if (resolved && it != pending_permissions.end() && it->second) {
                    result = std::move(*(it->second));
                }
                if (it != pending_permissions.end()) pending_permissions.erase(it);
                return result;
            });
        struct PermissionHandlerGuard {
            std::string session_id;
            std::mutex& mutex;
            std::condition_variable& cv;
            bool& closing;
            ~PermissionHandlerGuard() {
                unregister_direct_permission_handler(session_id);
                {
                    std::lock_guard lock(mutex);
                    closing = true;
                }
                cv.notify_all();
            }
        } permission_guard{session_id, permission_mutex, permission_cv, closing_permissions};

        while (running_.load()) {
            auto frame = detail::read_ws_frame(fd);
            if (!frame) break;
            if (frame->opcode == 0x8) {
                (void)send_ws_frame(0x8, {});
                break;
            }
            if (frame->opcode == 0x9) {
                (void)send_ws_frame(0xA, frame->payload);
                continue;
            }
            if (frame->opcode != 0x1 && frame->opcode != 0x2) continue;

            const auto subtype = detail::control_request_subtype(frame->payload);
            if (subtype == "interrupt") {
                const auto request_id = detail::control_request_id(frame->payload);
                const bool interrupted = request_direct_query_cancel(session_id);
                {
                    std::lock_guard lock(cancel_mutex_);
                    cancelled_sessions_.insert(session_id);
                }
                (void)send_ws_text(std::format(
                    R"({{"type":"control_response","response":{{"subtype":"success","request_id":"{}","response":{{"interrupted":{}}}}}}})",
                    detail::server_json_escape(request_id),
                    interrupted ? "true" : "false"));
                continue;
            }
            if (record_control_response(frame->payload) ||
                frame->payload.find(R"("type":"control_response")") != std::string::npos) {
                continue;
            }

            auto content = detail::extract_user_content(frame->payload);
            if (!content || content->empty()) {
                continue;
            }

            {
                std::lock_guard lock(cancel_mutex_);
                cancelled_sessions_.erase(session_id);
            }

            auto route_it = std::ranges::find_if(routes_, [](const Route& route) {
                return route.method == "POST" && route.path == "/message";
            });
            if (route_it == routes_.end()) {
                (void)send_ws_text(detail::sdk_error_result(session_id, "message route is not available"));
                continue;
            }

            auto handler = route_it->handler;
            auto content_value = *content;
            auto response_future = std::async(
                std::launch::async,
                [handler, session_id, content_value = std::move(content_value)]() mutable {
                    return handler({
                        {"session_id", session_id},
                        {"content", content_value},
                    });
                });
            while (running_.load() && response_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                bool closed = false;
                auto pending_frame = detail::read_ws_frame_available(fd, std::chrono::milliseconds(25), closed);
                if (closed) {
                    (void)request_direct_query_cancel(session_id);
                    return;
                }
                if (!pending_frame) continue;
                if (pending_frame->opcode == 0x8) {
                    (void)request_direct_query_cancel(session_id);
                    (void)send_ws_frame(0x8, {});
                    return;
                }
                if (pending_frame->opcode == 0x9) {
                    (void)send_ws_frame(0xA, pending_frame->payload);
                    continue;
                }
                if (pending_frame->opcode != 0x1 && pending_frame->opcode != 0x2) continue;

                const auto pending_subtype = detail::control_request_subtype(pending_frame->payload);
                if (pending_subtype == "interrupt") {
                    const auto request_id = detail::control_request_id(pending_frame->payload);
                    const bool interrupted = request_direct_query_cancel(session_id);
                    {
                        std::lock_guard lock(cancel_mutex_);
                        cancelled_sessions_.insert(session_id);
                    }
                    (void)send_ws_text(std::format(
                        R"({{"type":"control_response","response":{{"subtype":"success","request_id":"{}","response":{{"interrupted":{}}}}}}})",
                        detail::server_json_escape(request_id),
                        interrupted ? "true" : "false"));
                    continue;
                }
                if (record_control_response(pending_frame->payload) ||
                    pending_frame->payload.find(R"("type":"control_response")") != std::string::npos) {
                    continue;
                }
            }
            if (!running_.load()) {
                (void)request_direct_query_cancel(session_id);
                return;
            }
            auto response_text = response_future.get();
            auto parsed = cc::utils::json::parse(response_text);
            if (!parsed || !parsed->root().is_obj()) {
                (void)send_ws_text(detail::sdk_error_result(session_id, response_text));
                continue;
            }
            auto root = parsed->root();
            auto error = root.get("error");
            if (error.is_str()) {
                (void)send_ws_text(detail::sdk_error_result(session_id, error.as_str()));
                continue;
            }
            (void)send_ws_text(detail::sdk_assistant_message(session_id, root));
            (void)send_ws_text(detail::sdk_result_message(session_id, root));
        }
    }

    ServerConfig config_;
    std::atomic<bool> running_{false};
    int listen_fd_{-1};
    std::jthread server_thread_;
    std::mutex mutex_;
    std::vector<Route> routes_;
    std::mutex cancel_mutex_;
    std::unordered_set<std::string> cancelled_sessions_;
};

} // namespace cc::server
