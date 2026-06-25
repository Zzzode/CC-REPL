module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

export module cc.bridge.security;


export namespace cc::bridge {

namespace detail {
[[nodiscard]] std::string base64url_decode(std::string_view input) {
    static constexpr std::string_view chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64(input);
    for (auto& ch : b64) {
        if (ch == '-') ch = '+';
        if (ch == '_') ch = '/';
    }
    while (b64.size() % 4 != 0) b64.push_back('=');

    std::string out;
    int value = 0;
    int bits = -8;
    for (unsigned char ch : b64) {
        if (ch == '=') break;
        auto pos = chars.find(static_cast<char>(ch));
        if (pos == std::string_view::npos) continue;
        value = (value << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

[[nodiscard]] std::string extract_json_string(std::string_view json, std::string_view key) {
    auto marker = std::string{"\""} + std::string{key} + "\":\"";
    auto pos = json.find(marker);
    if (pos == std::string_view::npos) return {};
    pos += marker.size();
    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(pos, end - pos));
}

[[nodiscard]] int64_t extract_json_int(std::string_view json, std::string_view key) {
    auto marker = std::string{"\""} + std::string{key} + "\":";
    auto pos = json.find(marker);
    if (pos == std::string_view::npos) return 0;
    pos += marker.size();
    auto end = json.find_first_not_of("0123456789", pos);
    auto digits = json.substr(pos, end == std::string_view::npos ? json.size() - pos : end - pos);
    return digits.empty() ? 0 : std::stoll(std::string(digits));
}
} // namespace detail



struct BasicJwtPayload {
    std::string sub;      // subject (session ID)
    std::string iss;      // issuer
    int64_t iat{0};       // issued at (unix timestamp)
    int64_t exp{0};       // expiry (unix timestamp)
    std::vector<std::string> scopes;
};

class JwtUtils {
public:

    [[nodiscard]] static auto decode_payload(std::string_view token) -> std::expected<BasicJwtPayload, std::string> {

        auto first_dot = token.find('.');
        if (first_dot == std::string_view::npos) return std::unexpected("invalid JWT format");
        auto second_dot = token.find('.', first_dot + 1);
        if (second_dot == std::string_view::npos) return std::unexpected("invalid JWT format");
        
        auto payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
        auto payload_json = detail::base64url_decode(payload_b64);
        if (payload_json.empty()) return std::unexpected("JWT payload decode failed");
        return BasicJwtPayload{
            .sub = detail::extract_json_string(payload_json, "sub"),
            .iss = detail::extract_json_string(payload_json, "iss"),
            .iat = detail::extract_json_int(payload_json, "iat"),
            .exp = detail::extract_json_int(payload_json, "exp"),
            .scopes = {},
        };
    }
    

    [[nodiscard]] static auto is_expired(const BasicJwtPayload& payload) -> bool {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        return payload.exp > 0 && now_secs > payload.exp;
    }
    

    [[nodiscard]] static auto generate_session_token() -> std::string {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "ccs_" + std::to_string(now);
    }
};



struct TrustedDevice {
    std::string device_id;
    std::string device_name;
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_seen;
    bool is_current{false};
};

class TrustedDeviceManager {
    std::vector<TrustedDevice> devices_;
    std::string current_device_id_;
public:

    [[nodiscard]] auto get_current_device_id() -> std::string {
        if (current_device_id_.empty()) {
            char hostname[256]{};
            (void)gethostname(hostname, sizeof(hostname) - 1);
            auto user = std::getenv("USER") ? std::getenv("USER") : "unknown";
            current_device_id_ = "dev_" + std::to_string(
                std::hash<std::string>{}(std::string(hostname) + ":" + user));
        }
        return current_device_id_;
    }
    

    void register_device(std::string name) {
        auto id = get_current_device_id();
        auto now = std::chrono::system_clock::now();
        devices_.push_back({id, std::move(name), now, now, true});
    }
    

    [[nodiscard]] auto is_trusted(std::string_view device_id) const -> bool {
        for (const auto& d : devices_) if (d.device_id == device_id) return true;
        return false;
    }
    

    [[nodiscard]] auto list_devices() const -> const std::vector<TrustedDevice>& { return devices_; }
    

    void revoke(std::string_view device_id) {
        std::erase_if(devices_, [&](const auto& d) { return d.device_id == device_id; });
    }
};



class WorkSecret {
    std::string secret_;
    std::chrono::system_clock::time_point created_at_;
    std::chrono::hours max_age_{24};
public:

    void generate() {
        std::random_device rd;
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        secret_ = "ws_" + std::to_string(rd()) + "_" + std::to_string(rd()) + "_" + std::to_string(now);
        created_at_ = std::chrono::system_clock::now();
    }
    

    [[nodiscard]] auto verify(std::string_view candidate) const -> bool {
        if (secret_.empty()) return false;
        if (is_expired()) return false;
        return candidate == secret_;
    }
    

    [[nodiscard]] auto is_expired() const -> bool {
        return (std::chrono::system_clock::now() - created_at_) > max_age_;
    }
    

    [[nodiscard]] auto get() const -> std::string_view { return secret_; }
};




enum class BridgePermissionType { 
    tool_execution, file_read, file_write, shell_command, network_access 
};

struct BridgePermissionRequest {
    BridgePermissionType type;
    std::string resource;
    std::string requester;
    std::string context;
};

enum class BridgePermissionResult { allow, deny, ask_user };

using PermissionCallback = std::function<BridgePermissionResult(const BridgePermissionRequest&)>;

class BridgePermissionManager {
    std::vector<PermissionCallback> callbacks_;
    std::vector<std::pair<std::string, BridgePermissionResult>> remembered_;
public:

    void register_callback(PermissionCallback cb) { callbacks_.push_back(std::move(cb)); }
    

    [[nodiscard]] auto check(const BridgePermissionRequest& req) -> BridgePermissionResult {

        for (const auto& [resource, result] : remembered_) {
            if (resource == req.resource) return result;
        }

        for (const auto& cb : callbacks_) {
            auto result = cb(req);
            if (result != BridgePermissionResult::ask_user) return result;
        }
        return BridgePermissionResult::ask_user;
    }
    

    void remember(std::string resource, BridgePermissionResult result) {
        remembered_.emplace_back(std::move(resource), result);
    }
    

    void clear_remembered() { remembered_.clear(); }
};



// ---------------------------------------------------------------------------
// Bidirectional RPC-style permission protocol
// ---------------------------------------------------------------------------

// Response sent back over the bridge after a permission decision is made.
struct BridgePermissionResponse {
    std::string permission_request_id;
    std::string behavior;  // "allow" or "deny"
    std::optional<std::string> updated_input_json;  // serialized JSON, if any
};

// Abstract interface for the bidirectional send/receive permission protocol.
// Concrete implementations bridge the transport layer (WebSocket, IPC, etc.).
class BridgePermissionCallbacks {
public:
    virtual ~BridgePermissionCallbacks() = default;

    // Send a permission request to the remote bridge peer.
    virtual auto send_request(std::string_view request_json) -> std::expected<void, std::string> = 0;

    // Send a permission response back to the remote bridge peer.
    virtual auto send_response(const BridgePermissionResponse& response) -> std::expected<void, std::string> = 0;
};

// Type guard: returns true when the JSON string contains a permission response
// envelope, i.e.  {"type": "permission_response", ...}.
[[nodiscard]] inline auto is_bridge_permission_response(std::string_view json) -> bool {
    // Quick scan for the discriminator field.
    auto pos = json.find(R"("type")");
    if (pos == std::string_view::npos) return false;

    // Skip whitespace / colon between "type" and its value.
    auto scan = json.find_first_not_of(" \t\n\r:", pos + 5);
    if (scan == std::string_view::npos) return false;

    // Accept both "permission_response" and 'permission_response'.
    char quote = json[scan];
    if (quote != '"' && quote != '\'') return false;
    auto value_start = scan + 1;
    auto value_end = json.find(quote, value_start);
    if (value_end == std::string_view::npos) return false;
    return json.substr(value_start, value_end - value_start) == "permission_response";
}

// Manages the full bidirectional permission request/response flow.
// Thread-safe: all state is guarded by an internal mutex.
class BridgePermissionRpcChannel {
public:
    // Register (or replace) the callback handler used for outbound messages.
    void register_callback(std::shared_ptr<BridgePermissionCallbacks> cb) {
        auto lock = std::lock_guard{mutex_};
        callbacks_ = std::move(cb);
    }

    // Called when a permission request arrives from the bridge peer.
    // Forwards the request through the registered callback and installs a
    // one-shot handler keyed by request_id so that the eventual response
    // can be routed back.
    auto on_request_received(std::string_view request_json,
                             std::string_view request_id)
        -> std::expected<void, std::string>
    {
        (void)request_id;
        auto lock = std::lock_guard{mutex_};
        if (!callbacks_) return std::unexpected("no callback registered");
        return callbacks_->send_request(request_json);
    }

    // Called when a permission response arrives from the bridge peer.
    // Looks up the pending callback by permission_request_id, invokes it,
    // and removes it from the pending map.
    auto on_response_received(const BridgePermissionResponse& response)
        -> std::expected<void, std::string>
    {
        std::function<void(BridgePermissionResponse)> handler;
        {
            auto lock = std::lock_guard{mutex_};
            auto it = pending_.find(response.permission_request_id);
            if (it == pending_.end()) {
                return std::unexpected("unknown permission_request_id: "
                                       + response.permission_request_id);
            }
            handler = std::move(it->second);
            pending_.erase(it);
        }
        handler(response);
        return {};
    }

    // Enqueue a pending response handler for a given request ID.
    // Typically called before sending the request so the handler is ready.
    void add_pending_handler(
        std::string request_id,
        std::function<void(BridgePermissionResponse)> handler)
    {
        auto lock = std::lock_guard{mutex_};
        pending_.emplace(std::move(request_id), std::move(handler));
    }

    // Send a response back through the registered callback transport.
    auto send_response(const BridgePermissionResponse& response)
        -> std::expected<void, std::string>
    {
        auto lock = std::lock_guard{mutex_};
        if (!callbacks_) return std::unexpected("no callback registered");
        return callbacks_->send_response(response);
    }

    // Remove all pending handlers (e.g. on session teardown).
    void clear_pending() {
        auto lock = std::lock_guard{mutex_};
        pending_.clear();
    }

private:
    std::mutex mutex_;
    std::shared_ptr<BridgePermissionCallbacks> callbacks_;
    std::unordered_map<std::string, std::function<void(BridgePermissionResponse)>> pending_;
};



class SessionIdCompat {
public:

    [[nodiscard]] static auto normalize(std::string_view id) -> std::string {

        if (id.starts_with("session_")) return std::string(id.substr(8));
        if (id.starts_with("sess_")) return std::string(id.substr(5));
        return std::string(id);
    }
    

    [[nodiscard]] static auto generate() -> std::string {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "sess_" + std::to_string(now);
    }
    

    [[nodiscard]] static auto is_valid(std::string_view id) -> bool {
        return !id.empty() && id.size() >= 8;
    }
};



class BridgeDebugger {
    bool enabled_{false};
    struct DebugEntry { std::string direction; std::string type; std::string summary; 
                        std::chrono::system_clock::time_point ts; };
    std::vector<DebugEntry> log_;
    size_t max_entries_{500};
public:
    void enable(bool on = true) { enabled_ = on; }
    [[nodiscard]] auto is_enabled() const -> bool { return enabled_; }
    
    void log_outbound(std::string_view type, std::string_view summary) {
        if (!enabled_) return;
        log_.push_back({"→", std::string(type), std::string(summary), std::chrono::system_clock::now()});
        if (log_.size() > max_entries_) log_.erase(log_.begin());
    }
    
    void log_inbound(std::string_view type, std::string_view summary) {
        if (!enabled_) return;
        log_.push_back({"←", std::string(type), std::string(summary), std::chrono::system_clock::now()});
        if (log_.size() > max_entries_) log_.erase(log_.begin());
    }
    
    [[nodiscard]] auto get_log(size_t count = 50) const -> std::vector<DebugEntry> {
        if (log_.size() <= count) return log_;
        return {log_.end() - static_cast<ptrdiff_t>(count), log_.end()};
    }
    
    void clear() { log_.clear(); }
};

} // namespace cc::bridge
