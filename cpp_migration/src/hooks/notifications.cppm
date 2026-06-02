module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.notifications;


export namespace cc::hooks {

// 通知优先级
enum class NotifPriority { low, normal, high, critical };

// 通知类型 (对应原项目 hooks/notifs/ 下的16个通知hook)
enum class NotifType {
    auto_mode_warning,         // useAutoModeNotification — 自动模式警告
    deprecation_warning,       // useDeprecationWarning — 功能弃用通知
    fast_mode_upgrade,         // useFastModeNotification — 快速模式升级提示
    ide_status,                // useIDEStatusNotification — IDE 连接状态
    install_message,           // useInstallMessageNotification — 安装消息
    lsp_init,                  // useLSPInitNotification — LSP 初始化状态
    mcp_connection,            // useMcpConnectionNotification — MCP 服务器连接
    model_migration,           // useModelMigrationNotification — 模型迁移提示
    npm_deprecation,           // useNpmDeprecationNotification — npm 包弃用
    plugin_update,             // usePluginUpdateNotification — 插件更新可用
    rate_limit,                // useRateLimitNotification — 限流警告
    settings_error,            // useSettingsErrorNotification — 设置错误
    startup,                   // useStartupNotification — 启动通知
    update_available,          // useUpdateNotification — 版本更新可用
    permission_change,         // 权限变更通知
    session_restored,          // 会话恢复通知
};

// 通知消息
struct Notification {
    std::string id;
    NotifType type;
    NotifPriority priority{NotifPriority::normal};
    std::string title;
    std::string message;
    std::optional<std::string> action_label;  // 可选操作按钮文本
    std::optional<std::string> action_url;    // 可选操作链接
    std::chrono::system_clock::time_point created_at;
    bool dismissed{false};
    bool persistent{false};  // 是否在下次启动时仍显示
};

// 通知过滤条件
struct NotifFilter {
    std::optional<NotifType> type;
    std::optional<NotifPriority> min_priority;
    bool include_dismissed{false};
};

// 通知处理回调
using NotifHandler = std::function<void(const Notification&)>;
using DismissHandler = std::function<void(std::string_view notif_id)>;
using UnsubscribeFn = std::function<void()>;

// ─── 各类通知检查器 ──────────────────────────────────────────

// 自动模式通知 — 检测自动模式下的危险操作
class AutoModeNotifier {
    bool auto_mode_active_{false};
    int auto_approved_count_{0};
public:
    void set_auto_mode(bool active) { auto_mode_active_ = active; auto_approved_count_ = 0; }
    void record_auto_approval() { ++auto_approved_count_; }
    [[nodiscard]] auto should_warn() const -> bool { return auto_mode_active_ && auto_approved_count_ > 20; }
    [[nodiscard]] auto get_notification() const -> std::optional<Notification> {
        if (!should_warn()) return std::nullopt;
        return Notification{
            .id = "auto_mode_warn", .type = NotifType::auto_mode_warning,
            .priority = NotifPriority::high,
            .title = "自动模式警告",
            .message = "已自动批准 " + std::to_string(auto_approved_count_) + " 个操作，建议检查执行结果",
            .created_at = std::chrono::system_clock::now()
        };
    }
};

// 弃用警告检查器
class DeprecationChecker {
    struct DeprecatedFeature { std::string name; std::string replacement; std::string removal_version; };
    std::vector<DeprecatedFeature> deprecated_features_;
public:
    void register_deprecation(std::string name, std::string replacement, std::string version) {
        deprecated_features_.push_back({std::move(name), std::move(replacement), std::move(version)});
    }
    [[nodiscard]] auto check(std::string_view feature_used) const -> std::optional<Notification> {
        for (const auto& f : deprecated_features_) {
            if (f.name == feature_used) {
                return Notification{
                    .id = "deprecation_" + f.name, .type = NotifType::deprecation_warning,
                    .title = "功能已弃用: " + f.name,
                    .message = "请使用 " + f.replacement + "（将在 " + f.removal_version + " 中移除）",
                    .created_at = std::chrono::system_clock::now()
                };
            }
        }
        return std::nullopt;
    }
};

// 限流通知器
class RateLimitNotifier {
    int consecutive_limits_{0};
    std::chrono::system_clock::time_point last_limit_at_{};
public:
    void record_limit() {
        ++consecutive_limits_;
        last_limit_at_ = std::chrono::system_clock::now();
    }
    void reset() { consecutive_limits_ = 0; }
    [[nodiscard]] auto get_notification() const -> std::optional<Notification> {
        if (consecutive_limits_ == 0) return std::nullopt;
        return Notification{
            .id = "rate_limit", .type = NotifType::rate_limit,
            .priority = consecutive_limits_ > 3 ? NotifPriority::critical : NotifPriority::high,
            .title = "API 限流",
            .message = "已触发 " + std::to_string(consecutive_limits_) + " 次限流，正在等待恢复",
            .action_label = "查看用量", .action_url = "/usage",
            .created_at = last_limit_at_
        };
    }
};

// 更新通知器
class UpdateNotifier {
    std::string current_version_;
    std::optional<std::string> latest_version_;
public:
    explicit UpdateNotifier(std::string version) : current_version_(std::move(version)) {}
    void set_latest(std::string version) { latest_version_ = std::move(version); }
    [[nodiscard]] auto get_notification() const -> std::optional<Notification> {
        if (!latest_version_ || *latest_version_ == current_version_) return std::nullopt;
        return Notification{
            .id = "update_available", .type = NotifType::update_available,
            .title = "新版本可用: " + *latest_version_,
            .message = "当前版本 " + current_version_ + "，可用 /upgrade 升级",
            .action_label = "升级", .action_url = "/upgrade",
            .created_at = std::chrono::system_clock::now(), .persistent = true
        };
    }
};

// MCP 连接状态通知器
class McpConnectionNotifier {
    struct ServerStatus { std::string name; bool connected; std::string error; };
    std::vector<ServerStatus> servers_;
public:
    void update_status(std::string name, bool connected, std::string error = "") {
        for (auto& s : servers_) {
            if (s.name == name) { s.connected = connected; s.error = std::move(error); return; }
        }
        servers_.push_back({std::move(name), connected, std::move(error)});
    }
    [[nodiscard]] auto get_disconnected_notifications() const -> std::vector<Notification> {
        std::vector<Notification> notifs;
        for (const auto& s : servers_) {
            if (!s.connected) {
                notifs.push_back({
                    .id = "mcp_" + s.name, .type = NotifType::mcp_connection,
                    .priority = NotifPriority::high,
                    .title = "MCP 服务器断开: " + s.name,
                    .message = s.error.empty() ? "连接已断开" : s.error,
                    .created_at = std::chrono::system_clock::now()
                });
            }
        }
        return notifs;
    }
};

// IDE 状态通知器
class IdeStatusNotifier {
    bool connected_{false};
    std::string ide_name_;
    std::string ide_version_;
public:
    void update(bool connected, std::string name = "", std::string version = "") {
        connected_ = connected; ide_name_ = std::move(name); ide_version_ = std::move(version);
    }
    [[nodiscard]] auto get_notification() const -> std::optional<Notification> {
        if (connected_) return std::nullopt;
        return Notification{
            .id = "ide_disconnected", .type = NotifType::ide_status,
            .priority = NotifPriority::normal,
            .title = "IDE 未连接",
            .message = ide_name_.empty() ? "未检测到 IDE 连接" : ide_name_ + " 连接已断开",
            .created_at = std::chrono::system_clock::now()
        };
    }
};

// 插件更新通知器
class PluginUpdateNotifier {
    struct PluginUpdate { std::string name; std::string current_ver; std::string latest_ver; };
    std::vector<PluginUpdate> pending_updates_;
public:
    void add_update(std::string name, std::string current, std::string latest) {
        pending_updates_.push_back({std::move(name), std::move(current), std::move(latest)});
    }
    void clear() { pending_updates_.clear(); }
    [[nodiscard]] auto get_notification() const -> std::optional<Notification> {
        if (pending_updates_.empty()) return std::nullopt;
        std::string msg = std::to_string(pending_updates_.size()) + " 个插件有更新可用";
        return Notification{
            .id = "plugin_updates", .type = NotifType::plugin_update,
            .title = "插件更新", .message = msg,
            .action_label = "更新", .action_url = "/plugin update",
            .created_at = std::chrono::system_clock::now()
        };
    }
};

// 设置错误通知器
class SettingsErrorNotifier {
    std::vector<std::string> errors_;
public:
    void add_error(std::string err) { errors_.push_back(std::move(err)); }
    void clear() { errors_.clear(); }
    [[nodiscard]] auto get_notification() const -> std::optional<Notification> {
        if (errors_.empty()) return std::nullopt;
        return Notification{
            .id = "settings_error", .type = NotifType::settings_error,
            .priority = NotifPriority::high,
            .title = "设置配置错误",
            .message = errors_.front() + (errors_.size() > 1 ? " (+" + std::to_string(errors_.size()-1) + " 个其他错误)" : ""),
            .created_at = std::chrono::system_clock::now()
        };
    }
};

// ─── 通知中心 (聚合所有通知器) ──────────────────────────────

class NotificationCenter {
    std::vector<Notification> active_notifications_;
    std::vector<NotifHandler> handlers_;
    
    // 各子通知器
    AutoModeNotifier auto_mode_;
    DeprecationChecker deprecation_;
    RateLimitNotifier rate_limit_;
    UpdateNotifier update_{"2.0.0"};
    McpConnectionNotifier mcp_;
    IdeStatusNotifier ide_;
    PluginUpdateNotifier plugin_;
    SettingsErrorNotifier settings_;

public:
    // 推送通知
    void push(Notification notif) {
        for (const auto& handler : handlers_) handler(notif);
        active_notifications_.push_back(std::move(notif));
    }

    // 批量收集所有待显示通知
    [[nodiscard]] auto collect_all() -> std::vector<Notification> {
        std::vector<Notification> result;
        if (auto n = auto_mode_.get_notification()) result.push_back(*n);
        if (auto n = rate_limit_.get_notification()) result.push_back(*n);
        if (auto n = update_.get_notification()) result.push_back(*n);
        if (auto n = ide_.get_notification()) result.push_back(*n);
        if (auto n = plugin_.get_notification()) result.push_back(*n);
        if (auto n = settings_.get_notification()) result.push_back(*n);
        for (auto& n : mcp_.get_disconnected_notifications()) result.push_back(std::move(n));
        return result;
    }

    // 关闭通知
    void dismiss(std::string_view notif_id) {
        for (auto& n : active_notifications_) {
            if (n.id == notif_id) { n.dismissed = true; break; }
        }
    }

    // 获取未关闭通知
    [[nodiscard]] auto get_active(NotifFilter filter = {}) const -> std::vector<Notification> {
        std::vector<Notification> result;
        for (const auto& n : active_notifications_) {
            if (n.dismissed && !filter.include_dismissed) continue;
            if (filter.type && n.type != *filter.type) continue;
            if (filter.min_priority && n.priority < *filter.min_priority) continue;
            result.push_back(n);
        }
        return result;
    }

    // 订阅通知
    [[nodiscard]] auto subscribe(NotifHandler handler) -> UnsubscribeFn {
        handlers_.push_back(std::move(handler));
        auto idx = handlers_.size() - 1;
        return [this, idx]() { handlers_[idx] = nullptr; };
    }

    // 访问子通知器
    auto auto_mode() -> AutoModeNotifier& { return auto_mode_; }
    auto deprecation() -> DeprecationChecker& { return deprecation_; }
    auto rate_limit_notifier() -> RateLimitNotifier& { return rate_limit_; }
    auto updater() -> UpdateNotifier& { return update_; }
    auto mcp_notifier() -> McpConnectionNotifier& { return mcp_; }
    auto ide_notifier() -> IdeStatusNotifier& { return ide_; }
    auto plugin_notifier() -> PluginUpdateNotifier& { return plugin_; }
    auto settings_notifier() -> SettingsErrorNotifier& { return settings_; }

    // 清除所有
    void clear_all() { active_notifications_.clear(); }
    [[nodiscard]] auto count() const -> size_t { return active_notifications_.size(); }
};

} // namespace cc::hooks
