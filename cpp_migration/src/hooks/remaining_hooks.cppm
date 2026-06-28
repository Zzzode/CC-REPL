module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sstream>
#include <sys/resource.h>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.hooks.remaining_hooks;


export namespace cc::hooks {

namespace detail {
[[nodiscard]] inline auto extract_json_string(std::string_view text, std::string_view key)
    -> std::optional<std::string> {
    const auto needle = '"' + std::string(key) + '"';
    auto pos = text.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    pos = text.find('"', pos + 1);
    if (pos == std::string_view::npos) return std::nullopt;
    auto end = text.find('"', pos + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(text.substr(pos + 1, end - pos - 1));
}

[[nodiscard]] inline auto extract_json_bool(std::string_view text, std::string_view key)
    -> std::optional<bool> {
    const auto needle = '"' + std::string(key) + '"';
    auto pos = text.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    auto value = text.substr(pos + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    if (value.starts_with("true")) return true;
    if (value.starts_with("false")) return false;
    return std::nullopt;
}
}




class IdeLoggingHook {
    enum class LogLevel { debug, info, warn, error };
    struct LogEntry { LogLevel level; std::string message; std::chrono::system_clock::time_point ts; };
    std::vector<LogEntry> log_buffer_;
    size_t max_buffer_size_{1000};
    bool enabled_{false};
public:
    void enable(bool on = true) { enabled_ = on; }
    void log(LogLevel level, std::string message) {
        if (!enabled_) return;
        log_buffer_.push_back({level, std::move(message), std::chrono::system_clock::now()});
        if (log_buffer_.size() > max_buffer_size_) log_buffer_.erase(log_buffer_.begin());
    }
    [[nodiscard]] auto get_logs(size_t count = 50) const -> std::vector<LogEntry> {
        if (log_buffer_.size() <= count) return log_buffer_;
        return {log_buffer_.end() - static_cast<ptrdiff_t>(count), log_buffer_.end()};
    }
    void clear() { log_buffer_.clear(); }
};


class IdeSelectionHook {
    struct Selection { std::string file_path; int start_line; int start_col; int end_line; int end_col; std::string text; };
    std::optional<Selection> current_selection_;
    std::function<void(const Selection&)> on_selection_change_;
public:
    void update_selection(std::string path, int sl, int sc, int el, int ec, std::string text) {
        current_selection_ = Selection{std::move(path), sl, sc, el, ec, std::move(text)};
        if (on_selection_change_ && current_selection_) on_selection_change_(*current_selection_);
    }
    void clear_selection() { current_selection_.reset(); }
    [[nodiscard]] auto get_selection() const -> const std::optional<Selection>& { return current_selection_; }
    void on_change(std::function<void(const Selection&)> cb) { on_selection_change_ = std::move(cb); }
};


// IDE at-mention payload (TS IDEAtMentioned parity: filePath / lineStart / lineEnd,
// line numbers already 1-based by the time the responder fires, matching TS
// useIdeAtMentioned.ts which adds +1 to the 0-based IDE values).
struct IdeAtMentionPayload {
    std::string file_path;
    std::optional<int> line_start;
    std::optional<int> line_end;
};

class IdeAtMentionHook {
    std::vector<IdeAtMentionPayload> pending_mentions_;
    std::function<void(const IdeAtMentionPayload&)> on_at_mentioned_;
public:
    // Push a parsed at-mention (called by the MCP at_mentioned responder).
    void add_mention(IdeAtMentionPayload m) {
        pending_mentions_.push_back(std::move(m));
        if (on_at_mentioned_ && !pending_mentions_.empty()) {
            on_at_mentioned_(pending_mentions_.back());
        }
    }
    // Legacy convenience overload retained for existing call sites.
    void add_mention(std::string path, int line, std::string ctx) {
        IdeAtMentionPayload p{.file_path = std::move(path),
                              .line_start = line > 0 ? std::optional<int>(line) : std::nullopt,
                              .line_end = std::nullopt};
        (void)ctx;
        add_mention(std::move(p));
    }
    [[nodiscard]] auto get_pending() const -> const std::vector<IdeAtMentionPayload>& { return pending_mentions_; }
    void consume_all() { pending_mentions_.clear(); }
    [[nodiscard]] auto has_pending() const -> bool { return !pending_mentions_.empty(); }
    // Subscribe to each incoming at-mention (used by the UI to insert @path).
    void on_at_mentioned(std::function<void(const IdeAtMentionPayload&)> cb) {
        on_at_mentioned_ = std::move(cb);
    }
};


class IdeConnectionHook {
    enum class IdeType { vscode, jetbrains, neovim, unknown };
    struct ConnectionState { IdeType type; bool connected; std::string version; int port; };
    ConnectionState state_{IdeType::unknown, false, "", 0};
    std::function<void(bool)> on_connection_change_;
public:
    void connect(IdeType type, std::string version, int port) {
        state_ = {type, true, std::move(version), port};
        if (on_connection_change_) on_connection_change_(true);
    }
    void disconnect() {
        state_.connected = false;
        if (on_connection_change_) on_connection_change_(false);
    }
    [[nodiscard]] auto is_connected() const -> bool { return state_.connected; }
    [[nodiscard]] auto get_ide_type() const -> IdeType { return state_.type; }
    void on_change(std::function<void(bool)> cb) { on_connection_change_ = std::move(cb); }
};




class RemoteSessionHook {
    struct RemoteState { std::string session_id; bool active; std::string peer_name; };
    std::optional<RemoteState> current_;
public:
    void start(std::string id, std::string peer) { current_ = {std::move(id), true, std::move(peer)}; }
    void stop() { if (current_) current_->active = false; }
    [[nodiscard]] auto is_active() const -> bool { return current_ && current_->active; }
    [[nodiscard]] auto get_peer() const -> std::string { return current_ ? current_->peer_name : ""; }
};


class ReplBridgeHook {
    bool bridge_active_{false};
    std::string bridge_url_;
    std::function<void(std::string_view)> message_handler_;
    std::vector<std::string> outbound_;
public:
    void activate(std::string url) { bridge_active_ = true; bridge_url_ = std::move(url); }
    void deactivate() { bridge_active_ = false; }
    void on_message(std::function<void(std::string_view)> handler) { message_handler_ = std::move(handler); }
    void send(std::string_view msg) {
        if (!bridge_active_) return;
        outbound_.emplace_back(msg);
        if (message_handler_) message_handler_(msg);
    }
    [[nodiscard]] auto is_active() const -> bool { return bridge_active_; }
    [[nodiscard]] auto pending_outbound() const -> const std::vector<std::string>& { return outbound_; }
};


class SSHSessionHook {
    struct SSHInfo { std::string host; std::string user; int port{22}; bool connected; };
    std::optional<SSHInfo> session_;
public:
    void detect() {

        if (std::getenv("SSH_CONNECTION")) {
            session_ = SSHInfo{
                .host = "remote",
                .user = {},
                .port = 22,
                .connected = true
            };
        }
    }
    [[nodiscard]] auto is_ssh() const -> bool { return session_.has_value(); }
    [[nodiscard]] auto get_host() const -> std::string { return session_ ? session_->host : ""; }
};


class MailboxBridgeHook {
    struct Message { std::string from_agent; std::string to_agent; std::string content; };
    std::vector<Message> inbox_;
    std::vector<Message> outbox_;
public:
    void send(std::string from, std::string to, std::string content) {
        outbox_.push_back({std::move(from), std::move(to), std::move(content)});
    }
    void receive(std::string from, std::string to, std::string content) {
        inbox_.push_back({std::move(from), std::move(to), std::move(content)});
    }
    [[nodiscard]] auto pending_inbox() const -> size_t { return inbox_.size(); }
    auto consume_inbox() -> std::vector<Message> { return std::exchange(inbox_, {}); }
};




class PluginRecommendationHook {
    struct Recommendation { std::string plugin_id; std::string reason; double confidence; };
    std::vector<Recommendation> recommendations_;
public:
    void analyze_context(std::string_view file_type, std::string_view tool_used) {
        recommendations_.clear();
        const auto add = [this](std::string id, std::string reason, double confidence) {
            recommendations_.push_back({std::move(id), std::move(reason), confidence});
        };

        if (file_type == "py" || file_type == "python") {
            add("python-dev", "Python file detected; enables linting, formatting, and test discovery", 0.92);
        } else if (file_type == "ts" || file_type == "tsx" || file_type == "typescript") {
            add("typescript-tools", "TypeScript context detected; improves symbols and diagnostics", 0.9);
        } else if (file_type == "go") {
            add("go-dev", "Go source detected; adds gopls-backed navigation and tests", 0.88);
        } else if (file_type == "cpp" || file_type == "hpp" || file_type == "cppm") {
            add("clangd-tools", "C++ context detected; provides clangd symbol and compile diagnostics", 0.86);
        }

        if (tool_used == "web_fetch" || tool_used == "web_search") {
            add("browser-assistant", "Recent web tool use suggests browser automation may help", 0.72);
        } else if (tool_used == "bash" || tool_used == "terminal") {
            add("shell-helper", "Terminal-heavy workflow detected", 0.68);
        }
    }
    [[nodiscard]] auto get_recommendations(size_t limit = 3) const -> std::vector<Recommendation> {
        if (recommendations_.size() <= limit) return recommendations_;
        return {recommendations_.begin(), recommendations_.begin() + static_cast<ptrdiff_t>(limit)};
    }
    void dismiss(std::string_view plugin_id) {
        std::erase_if(recommendations_, [&](const auto& r) { return r.plugin_id == plugin_id; });
    }
};


class LspPluginRecommendationHook {
    std::vector<std::string> detected_languages_;
    std::unordered_map<std::string, std::string> language_to_plugin_;
public:
    LspPluginRecommendationHook() {
        language_to_plugin_ = {
            {"python", "pylsp-plugin"}, {"rust", "rust-analyzer-plugin"},
            {"go", "gopls-plugin"}, {"typescript", "tsserver-plugin"}
        };
    }
    void detect_language(std::string lang) { detected_languages_.push_back(std::move(lang)); }
    [[nodiscard]] auto get_suggested_plugins() const -> std::vector<std::string> {
        std::vector<std::string> result;
        for (const auto& lang : detected_languages_) {
            if (auto it = language_to_plugin_.find(lang); it != language_to_plugin_.end())
                result.push_back(it->second);
        }
        return result;
    }
};


class ManagePluginsHook {
    struct PluginState { std::string id; std::string version; bool enabled; bool needs_update; };
    std::vector<PluginState> installed_;
public:
    void refresh() {
        installed_.clear();
        const char* home = std::getenv("HOME");
        if (!home) return;
        namespace fs = std::filesystem;
        const fs::path plugin_dir = fs::path(home) / ".cc-repl" / "plugins";
        if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir)) return;
        for (const auto& entry : fs::directory_iterator(plugin_dir)) {
            if (!entry.is_directory()) continue;
            const auto id = entry.path().filename().string();
            std::string version = "unknown";
            bool enabled = true;
            std::ifstream manifest(entry.path() / "manifest.json");
            if (manifest) {
                std::stringstream buffer;
                buffer << manifest.rdbuf();
                const auto text = buffer.str();
                if (auto v = detail::extract_json_string(text, "version")) version = *v;
                if (auto e = detail::extract_json_bool(text, "enabled")) enabled = *e;
            }
            installed_.push_back({.id = id, .version = version, .enabled = enabled, .needs_update = false});
        }
    }
    [[nodiscard]] auto get_installed() const -> const std::vector<PluginState>& { return installed_; }
    [[nodiscard]] auto needs_updates() const -> size_t {
        size_t count = 0;
        for (const auto& p : installed_) if (p.needs_update) ++count;
        return count;
    }
    void enable(std::string_view id) {
        for (auto& p : installed_) if (p.id == id) { p.enabled = true; break; }
    }
    void disable(std::string_view id) {
        for (auto& p : installed_) if (p.id == id) { p.enabled = false; break; }
    }
};


class MarketplaceNotificationHook {
    std::vector<std::string> featured_plugins_;
    bool has_unread_{false};
public:
    void fetch_featured() {
        featured_plugins_ = {"code-review", "unit-test-gen", "diagnose", "browser-assistant"};
        has_unread_ = !featured_plugins_.empty();
    }
    [[nodiscard]] auto has_notification() const -> bool { return has_unread_; }
    [[nodiscard]] auto featured() const -> const std::vector<std::string>& { return featured_plugins_; }
    void mark_read() { has_unread_ = false; }
};




class ElapsedTimeHook {
    std::chrono::steady_clock::time_point start_;
    bool running_{false};
public:
    void start() { start_ = std::chrono::steady_clock::now(); running_ = true; }
    void stop() { running_ = false; }
    [[nodiscard]] auto elapsed() const -> std::chrono::milliseconds {
        if (!running_) return std::chrono::milliseconds{0};
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_);
    }
    [[nodiscard]] auto elapsed_seconds() const -> double {
        return elapsed().count() / 1000.0;
    }
};


class BlinkHook {
    bool visible_{true};
    std::chrono::milliseconds interval_{500};
    std::chrono::steady_clock::time_point last_toggle_;
public:
    explicit BlinkHook(std::chrono::milliseconds interval = std::chrono::milliseconds{500})
        : interval_(interval), last_toggle_(std::chrono::steady_clock::now()) {}
    void tick() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_toggle_ >= interval_) { visible_ = !visible_; last_toggle_ = now; }
    }
    [[nodiscard]] auto is_visible() const -> bool { return visible_; }
    void set_interval(std::chrono::milliseconds ms) { interval_ = ms; }
};


class DoublePressHook {
    std::chrono::steady_clock::time_point last_press_;
    std::chrono::milliseconds threshold_{300};
    std::string last_key_;
public:
    [[nodiscard]] auto check(std::string_view key) -> bool {
        auto now = std::chrono::steady_clock::now();
        bool is_double = (key == last_key_) && (now - last_press_ < threshold_);
        last_press_ = now;
        last_key_ = std::string(key);
        return is_double;
    }
    void set_threshold(std::chrono::milliseconds ms) { threshold_ = ms; }
};


class MemoryUsageHook {
    struct MemStats { size_t rss_bytes; size_t heap_bytes; size_t external_bytes; };
    MemStats last_stats_{};
public:
    void update() {
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
            last_stats_.rss_bytes = static_cast<size_t>(usage.ru_maxrss);
#else
            last_stats_.rss_bytes = static_cast<size_t>(usage.ru_maxrss) * 1024;
#endif
            last_stats_.heap_bytes = last_stats_.rss_bytes;
            last_stats_.external_bytes = 0;
        }
    }
    [[nodiscard]] auto get_rss_mb() const -> double { return last_stats_.rss_bytes / (1024.0 * 1024.0); }
    [[nodiscard]] auto get_heap_mb() const -> double { return last_stats_.heap_bytes / (1024.0 * 1024.0); }
    [[nodiscard]] auto exceeds_threshold(size_t mb) const -> bool { return get_rss_mb() > mb; }
};


class TimeoutHook {
    std::chrono::steady_clock::time_point deadline_;
    bool active_{false};
    std::function<void()> on_timeout_;
public:
    void set(std::chrono::milliseconds duration, std::function<void()> callback) {
        deadline_ = std::chrono::steady_clock::now() + duration;
        on_timeout_ = std::move(callback);
        active_ = true;
    }
    void cancel() { active_ = false; }
    void check() {
        if (active_ && std::chrono::steady_clock::now() >= deadline_) {
            active_ = false;
            if (on_timeout_) on_timeout_();
        }
    }
    [[nodiscard]] auto is_active() const -> bool { return active_; }
};


class MinDisplayTimeHook {
    std::chrono::steady_clock::time_point show_start_;
    std::chrono::milliseconds min_duration_{200};
    bool showing_{false};
public:
    explicit MinDisplayTimeHook(std::chrono::milliseconds min = std::chrono::milliseconds{200})
        : min_duration_(min) {}
    void show() { show_start_ = std::chrono::steady_clock::now(); showing_ = true; }
    [[nodiscard]] auto can_hide() const -> bool {
        if (!showing_) return true;
        return (std::chrono::steady_clock::now() - show_start_) >= min_duration_;
    }
    void hide() { showing_ = false; }
};


class LogMessagesHook {
    struct LogMsg { std::string source; std::string message; std::chrono::system_clock::time_point ts; };
    std::vector<LogMsg> messages_;
    size_t max_messages_{500};
public:
    void log(std::string source, std::string msg) {
        messages_.push_back({std::move(source), std::move(msg), std::chrono::system_clock::now()});
        if (messages_.size() > max_messages_)
            messages_.erase(messages_.begin(), messages_.begin() + static_cast<ptrdiff_t>(messages_.size() - max_messages_));
    }
    [[nodiscard]] auto recent(size_t count = 20) const -> std::vector<LogMsg> {
        if (messages_.size() <= count) return messages_;
        return {messages_.end() - static_cast<ptrdiff_t>(count), messages_.end()};
    }
    void clear() { messages_.clear(); }
};


class InboxPollerHook {
    std::chrono::milliseconds poll_interval_{5000};
    std::chrono::steady_clock::time_point last_poll_;
    size_t unread_count_{0};
    std::function<void(size_t)> on_new_messages_;
public:
    void set_interval(std::chrono::milliseconds ms) { poll_interval_ = ms; }
    [[nodiscard]] auto should_poll() const -> bool {
        return (std::chrono::steady_clock::now() - last_poll_) >= poll_interval_;
    }
    void poll() {
        last_poll_ = std::chrono::steady_clock::now();
        if (unread_count_ > 0 && on_new_messages_) on_new_messages_(unread_count_);
    }
    void on_new(std::function<void(size_t)> cb) { on_new_messages_ = std::move(cb); }
    [[nodiscard]] auto unread() const -> size_t { return unread_count_; }
};




class CopyOnSelectHook {
    bool enabled_{false};
    std::function<void(std::string_view)> copy_fn_;
public:
    void enable(bool on = true) { enabled_ = on; }
    void set_copy_impl(std::function<void(std::string_view)> fn) { copy_fn_ = std::move(fn); }
    void on_selection(std::string_view text) {
        if (enabled_ && copy_fn_ && !text.empty()) copy_fn_(text);
    }
};


class AwaySummaryHook {
    std::chrono::system_clock::time_point last_active_;
    std::chrono::minutes away_threshold_{5};
    std::string summary_;
public:
    void mark_active() { last_active_ = std::chrono::system_clock::now(); }
    [[nodiscard]] auto is_away() const -> bool {
        return (std::chrono::system_clock::now() - last_active_) > away_threshold_;
    }
    void set_summary(std::string s) { summary_ = std::move(s); }
    [[nodiscard]] auto get_summary() const -> std::string_view { return summary_; }
};


class DirectConnectHook {
    bool direct_mode_{false};
    std::string direct_url_;
public:
    void enable(std::string url) { direct_mode_ = true; direct_url_ = std::move(url); }
    void disable() { direct_mode_ = false; direct_url_.clear(); }
    [[nodiscard]] auto is_direct() const -> bool { return direct_mode_; }
    [[nodiscard]] auto get_url() const -> std::string_view { return direct_url_; }
};


class DynamicConfigHook {
    std::unordered_map<std::string, std::string> config_;
    std::chrono::steady_clock::time_point last_refresh_;
    std::chrono::seconds refresh_interval_{60};
public:
    void refresh() {
        last_refresh_ = std::chrono::steady_clock::now();
        if (const char* raw = std::getenv("CC_DYNAMIC_CONFIG")) {
            std::stringstream lines(raw);
            std::string line;
            while (std::getline(lines, line, ';')) {
                auto eq = line.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                config_[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
    }
    [[nodiscard]] auto get(std::string_view key) const -> std::optional<std::string> {
        auto it = config_.find(std::string(key));
        if (it != config_.end()) return it->second;
        return std::nullopt;
    }
    [[nodiscard]] auto needs_refresh() const -> bool {
        return (std::chrono::steady_clock::now() - last_refresh_) > refresh_interval_;
    }
    void set(std::string key, std::string value) { config_[std::move(key)] = std::move(value); }
};


class ApiKeyVerificationHook {
    enum class KeyStatus { unchecked, valid, invalid, expired };
    KeyStatus status_{KeyStatus::unchecked};
    std::string error_message_;
public:
    void verify(std::string_view key) {
        if (key.empty()) { status_ = KeyStatus::invalid; error_message_ = "API key is empty"; return; }
        if (!key.starts_with("sk-")) { status_ = KeyStatus::invalid; error_message_ = "invalid format"; return; }
        status_ = KeyStatus::valid;
    }
    [[nodiscard]] auto is_valid() const -> bool { return status_ == KeyStatus::valid; }
    [[nodiscard]] auto get_error() const -> std::string_view { return error_message_; }
};


class SessionBackgroundingHook {
    bool backgrounded_{false};
    std::chrono::system_clock::time_point backgrounded_at_;
public:
    void background() { backgrounded_ = true; backgrounded_at_ = std::chrono::system_clock::now(); }
    void foreground() { backgrounded_ = false; }
    [[nodiscard]] auto is_backgrounded() const -> bool { return backgrounded_; }
    [[nodiscard]] auto time_in_background() const -> std::chrono::seconds {
        if (!backgrounded_) return std::chrono::seconds{0};
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - backgrounded_at_);
    }
};


class ScheduledTasksHook {
    struct ScheduledTask { std::string id; std::string cron; std::chrono::system_clock::time_point next_run; bool active; };
    std::vector<ScheduledTask> tasks_;
public:
    void add(std::string id, std::string cron) {
        tasks_.push_back({std::move(id), std::move(cron), std::chrono::system_clock::now(), true});
    }
    void remove(std::string_view id) {
        std::erase_if(tasks_, [&](const auto& t) { return t.id == id; });
    }
    [[nodiscard]] auto get_due_tasks() const -> std::vector<std::string> {
        std::vector<std::string> due;
        auto now = std::chrono::system_clock::now();
        for (const auto& t : tasks_) {
            if (t.active && now >= t.next_run) due.push_back(t.id);
        }
        return due;
    }
};

} // namespace cc::hooks
