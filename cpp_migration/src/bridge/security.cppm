module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
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

// ─── JWT 工具 (对应 jwtUtils.ts) ────────────────────────────

struct JwtPayload {
    std::string sub;      // subject (session ID)
    std::string iss;      // issuer
    int64_t iat{0};       // issued at (unix timestamp)
    int64_t exp{0};       // expiry (unix timestamp)
    std::vector<std::string> scopes;  // 权限范围
};

class JwtUtils {
public:
    // 解码 JWT (不验证签名，仅解析 payload)
    [[nodiscard]] static auto decode_payload(std::string_view token) -> std::expected<JwtPayload, std::string> {
        // JWT 格式: header.payload.signature
        auto first_dot = token.find('.');
        if (first_dot == std::string_view::npos) return std::unexpected("无效 JWT 格式");
        auto second_dot = token.find('.', first_dot + 1);
        if (second_dot == std::string_view::npos) return std::unexpected("无效 JWT 格式");
        
        auto payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
        auto payload_json = detail::base64url_decode(payload_b64);
        if (payload_json.empty()) return std::unexpected("JWT payload 解码失败");
        return JwtPayload{
            .sub = detail::extract_json_string(payload_json, "sub"),
            .iss = detail::extract_json_string(payload_json, "iss"),
            .iat = detail::extract_json_int(payload_json, "iat"),
            .exp = detail::extract_json_int(payload_json, "exp"),
            .scopes = {},
        };
    }
    
    // 检查 JWT 是否过期
    [[nodiscard]] static auto is_expired(const JwtPayload& payload) -> bool {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        return payload.exp > 0 && now_secs > payload.exp;
    }
    
    // 生成简单的会话 token (非加密，仅用于内部标识)
    [[nodiscard]] static auto generate_session_token() -> std::string {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "ccs_" + std::to_string(now);
    }
};

// ─── 可信设备 (对应 trustedDevice.ts) ──────────────────────

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
    // 获取当前设备 ID (机器指纹)
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
    
    // 注册为可信设备
    void register_device(std::string name) {
        auto id = get_current_device_id();
        auto now = std::chrono::system_clock::now();
        devices_.push_back({id, std::move(name), now, now, true});
    }
    
    // 验证是否为可信设备
    [[nodiscard]] auto is_trusted(std::string_view device_id) const -> bool {
        for (const auto& d : devices_) if (d.device_id == device_id) return true;
        return false;
    }
    
    // 获取所有可信设备
    [[nodiscard]] auto list_devices() const -> const std::vector<TrustedDevice>& { return devices_; }
    
    // 撤销设备信任
    void revoke(std::string_view device_id) {
        std::erase_if(devices_, [&](const auto& d) { return d.device_id == device_id; });
    }
};

// ─── 工作密钥 (对应 workSecret.ts) ──────────────────────────

class WorkSecret {
    std::string secret_;
    std::chrono::system_clock::time_point created_at_;
    std::chrono::hours max_age_{24};
public:
    // 生成新密钥
    void generate() {
        std::random_device rd;
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        secret_ = "ws_" + std::to_string(rd()) + "_" + std::to_string(rd()) + "_" + std::to_string(now);
        created_at_ = std::chrono::system_clock::now();
    }
    
    // 验证密钥
    [[nodiscard]] auto verify(std::string_view candidate) const -> bool {
        if (secret_.empty()) return false;
        if (is_expired()) return false;
        return candidate == secret_;
    }
    
    // 密钥是否过期
    [[nodiscard]] auto is_expired() const -> bool {
        return (std::chrono::system_clock::now() - created_at_) > max_age_;
    }
    
    // 获取密钥 (仅内部使用)
    [[nodiscard]] auto get() const -> std::string_view { return secret_; }
};

// ─── 权限回调 (对应 bridgePermissionCallbacks.ts) ───────────

// 权限请求类型
enum class BridgePermissionType { 
    tool_execution, file_read, file_write, shell_command, network_access 
};

struct BridgePermissionRequest {
    BridgePermissionType type;
    std::string resource;      // 被请求的资源 (文件路径/命令/URL)
    std::string requester;     // 请求方标识
    std::string context;       // 额外上下文
};

enum class BridgePermissionResult { allow, deny, ask_user };

using PermissionCallback = std::function<BridgePermissionResult(const BridgePermissionRequest&)>;

class BridgePermissionManager {
    std::vector<PermissionCallback> callbacks_;
    std::vector<std::pair<std::string, BridgePermissionResult>> remembered_;
public:
    // 注册权限回调
    void register_callback(PermissionCallback cb) { callbacks_.push_back(std::move(cb)); }
    
    // 检查权限
    [[nodiscard]] auto check(const BridgePermissionRequest& req) -> BridgePermissionResult {
        // 先检查记忆
        for (const auto& [resource, result] : remembered_) {
            if (resource == req.resource) return result;
        }
        // 调用回调链
        for (const auto& cb : callbacks_) {
            auto result = cb(req);
            if (result != BridgePermissionResult::ask_user) return result;
        }
        return BridgePermissionResult::ask_user;
    }
    
    // 记住决定
    void remember(std::string resource, BridgePermissionResult result) {
        remembered_.emplace_back(std::move(resource), result);
    }
    
    // 清除记忆
    void clear_remembered() { remembered_.clear(); }
};

// ─── 会话 ID 兼容 (对应 sessionIdCompat.ts) ─────────────────

class SessionIdCompat {
public:
    // 标准化会话 ID 格式 (兼容旧版/新版)
    [[nodiscard]] static auto normalize(std::string_view id) -> std::string {
        // 移除前缀 "session_" 如果存在
        if (id.starts_with("session_")) return std::string(id.substr(8));
        if (id.starts_with("sess_")) return std::string(id.substr(5));
        return std::string(id);
    }
    
    // 生成带前缀的标准 ID
    [[nodiscard]] static auto generate() -> std::string {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "sess_" + std::to_string(now);
    }
    
    // 验证 ID 格式
    [[nodiscard]] static auto is_valid(std::string_view id) -> bool {
        return !id.empty() && id.size() >= 8;
    }
};

// ─── 桥接调试 (对应 bridgeDebug.ts + debugUtils.ts) ─────────

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
