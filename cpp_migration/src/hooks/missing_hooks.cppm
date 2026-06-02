// C++23 Module: Missing hooks implementation
module;

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.hooks.missing_hooks;


export namespace cc::hooks {

// ============================================================================
// useBlink: Blinking animation for cursor/indicators
// ============================================================================

class BlinkHook {
public:
    explicit BlinkHook(std::chrono::milliseconds interval = std::chrono::milliseconds{600})
        : interval_(interval), last_toggle_(std::chrono::steady_clock::now()) {}

    // 更新闪烁状态，应在动画帧中调用
    auto tick() -> void {
        auto now = std::chrono::steady_clock::now();
        if (now - last_toggle_ >= interval_) {
            visible_ = !visible_;
            last_toggle_ = now;
        }
    }

    // 获取当前可见状态
    [[nodiscard]] auto is_visible() const -> bool { return visible_; }

    // 设置闪烁间隔
    auto set_interval(std::chrono::milliseconds ms) -> void { interval_ = ms; }

    // 重置闪烁状态
    auto reset() -> void {
        visible_ = true;
        last_toggle_ = std::chrono::steady_clock::now();
    }

private:
    bool visible_{true};
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point last_toggle_;
};

// ============================================================================
// useCancelRequest: Request cancellation handler
// ============================================================================

enum class CancelSource { escape, ctrl_c, user_action };

struct CancelEvent {
    CancelSource source;
    std::string details;
    std::chrono::steady_clock::time_point timestamp;
};

using CancelCallback = std::function<void(const CancelEvent&)>;
using InterruptCallback = std::function<bool()>;

class CancelRequestHook {
public:
    CancelRequestHook() = default;

    // 设置取消回调
    auto on_cancel(CancelCallback callback) -> void {
        cancel_callback_ = std::move(callback);
    }

    // 设置中断回调（用于硬中断）
    auto on_interrupt(InterruptCallback callback) -> void {
        interrupt_callback_ = std::move(callback);
    }

    // 触发取消请求
    auto trigger_cancel(CancelSource source, std::string_view details = "") -> void {
        CancelEvent event{
            .source = source,
            .details = std::string(details),
            .timestamp = std::chrono::steady_clock::now()
        };
        if (cancel_callback_) cancel_callback_(event);
    }

    // 触发中断（返回true表示已处理）
    auto trigger_interrupt() -> bool {
        if (interrupt_callback_) return interrupt_callback_();
        return false;
    }

    // 检查是否有活动的请求可以取消
    [[nodiscard]] auto can_cancel() const -> bool { return has_active_request_; }

    // 设置是否有活动请求
    auto set_has_active_request(bool has) -> void { has_active_request_ = has; }

private:
    CancelCallback cancel_callback_;
    InterruptCallback interrupt_callback_;
    bool has_active_request_{false};
};

// ============================================================================
// useCommandKeybindings: Command keybinding management
// ============================================================================

enum class KeybindingContext { global, chat, editor, search };

struct Keybinding {
    std::string id;
    std::string key_sequence; // e.g., "Ctrl+C", "Esc"
    std::string description;
    KeybindingContext context{KeybindingContext::global};
    bool enabled{true};
};

struct KeyPressEvent {
    std::string key;
    std::set<std::string> modifiers; // Ctrl, Shift, Alt
    KeybindingContext context;
};

using KeybindingAction = std::function<void()>;

class CommandKeybindingsHook {
public:
    CommandKeybindingsHook() = default;

    // 注册快捷键
    auto register_keybinding(Keybinding kb, KeybindingAction action) -> void {
        keybindings_[kb.id] = std::move(kb);
        actions_[kb.id] = std::move(action);
    }

    // 注销快捷键
    auto unregister_keybinding(std::string_view id) -> void {
        keybindings_.erase(std::string(id));
        actions_.erase(std::string(id));
    }

    // 处理按键事件
    auto handle_key_press(const KeyPressEvent& event) -> bool {
        // 查找匹配的快捷键
        for (const auto& [id, kb] : keybindings_) {
            if (!kb.enabled) continue;
            if (kb.context != event.context && kb.context != KeybindingContext::global) continue;
            
            if (matches_key(kb, event)) {
                if (auto it = actions_.find(id); it != actions_.end()) {
                    it->second();
                    return true;
                }
            }
        }
        return false;
    }

    // 启用/禁用快捷键
    auto set_keybinding_enabled(std::string_view id, bool enabled) -> void {
        if (auto it = keybindings_.find(std::string(id)); it != keybindings_.end()) {
            it->second.enabled = enabled;
        }
    }

    // 获取所有快捷键
    [[nodiscard]] auto get_keybindings() const -> std::vector<Keybinding> {
        std::vector<Keybinding> result;
        result.reserve(keybindings_.size());
        for (const auto& [id, kb] : keybindings_) {
            result.push_back(kb);
        }
        return result;
    }

    // 设置当前上下文
    auto set_context(KeybindingContext context) -> void { current_context_ = context; }
    [[nodiscard]] auto get_context() const -> KeybindingContext { return current_context_; }

private:
    [[nodiscard]] static auto matches_key(const Keybinding& kb, const KeyPressEvent& event) -> bool {
        // 简化的匹配逻辑，实际需要更复杂的解析
        return kb.key_sequence == event.key;
    }

    std::unordered_map<std::string, Keybinding> keybindings_;
    std::unordered_map<std::string, KeybindingAction> actions_;
    KeybindingContext current_context_{KeybindingContext::global};
};

// ============================================================================
// useCopyOnSelect: Auto-copy on text selection
// ============================================================================

struct Selection {
    std::string text;
    std::size_t start_line{0};
    std::size_t start_col{0};
    std::size_t end_line{0};
    std::size_t end_col{0};
    bool is_dragging{false};
};

using CopyCallback = std::function<void(std::string_view)>;

class CopyOnSelectHook {
public:
    CopyOnSelectHook() = default;

    // 启用/禁用自动复制
    auto enable(bool on = true) -> void { enabled_ = on; }
    [[nodiscard]] auto is_enabled() const -> bool { return enabled_; }

    // 设置复制实现
    auto set_copy_fn(CopyCallback fn) -> void { copy_fn_ = std::move(fn); }

    // 处理选择变化
    auto on_selection_change(const Selection& selection) -> void {
        if (!enabled_ || !copy_fn_) return;

        // 只在拖动结束且有非空选择时复制
        if (!selection.is_dragging && !selection.text.empty() && !copied_) {
            // 跳过纯空白
            if (std::all_of(selection.text.begin(), selection.text.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
                copied_ = true;
                return;
            }
            copy_fn_(selection.text);
            copied_ = true;
        } else if (selection.is_dragging || selection.text.empty()) {
            copied_ = false;
        }
    }

private:
    bool enabled_{true};
    CopyCallback copy_fn_;
    bool copied_{false};
};

// ============================================================================
// useDiffInIDE: Diff view integration with IDE
// ============================================================================

enum class DiffResult { accepted, rejected, modified };

struct FileDiff {
    std::string file_path;
    std::string old_content;
    std::string new_content;
    std::vector<std::pair<int, std::string>> hunks; // line, content
};

using DiffCallback = std::function<void(DiffResult, const FileDiff&)>;

class DiffInIDEHook {
public:
    DiffInIDEHook() = default;

    // 设置IDE连接
    auto set_ide_connected(bool connected, std::string_view ide_name = "") -> void {
        ide_connected_ = connected;
        ide_name_ = std::string(ide_name);
    }

    [[nodiscard]] auto is_ide_connected() const -> bool { return ide_connected_; }
    [[nodiscard]] auto get_ide_name() const -> std::string_view { return ide_name_; }

    // 在IDE中显示diff
    auto show_diff(const FileDiff& diff) -> bool {
        if (!ide_connected_) return false;
        current_diff_ = diff;
        showing_diff_ = true;
        return true;
    }

    // 关闭diff
    auto close_diff() -> void {
        showing_diff_ = false;
        current_diff_ = std::nullopt;
    }

    // 检查是否正在显示diff
    [[nodiscard]] auto is_showing_diff() const -> bool { return showing_diff_; }

    // 设置diff结果回调
    auto on_diff_result(DiffCallback callback) -> void { diff_callback_ = std::move(callback); }

    // 触发diff结果（由IDE事件调用）
    auto trigger_result(DiffResult result, const FileDiff& diff) -> void {
        if (diff_callback_) diff_callback_(result, diff);
        if (result != DiffResult::modified) showing_diff_ = false;
    }

private:
    bool ide_connected_{false};
    std::string ide_name_;
    bool showing_diff_{false};
    std::optional<FileDiff> current_diff_;
    DiffCallback diff_callback_;
};

// ============================================================================
// useDirectConnect: Direct connection mode
// ============================================================================

struct DirectConnectConfig {
    std::string url;
    std::string auth_token;
    bool secure{true};
};

enum class ConnectionState { disconnected, connecting, connected, error };

using MessageCallback = std::function<void(std::string_view)>;
using ConnectionStateCallback = std::function<void(ConnectionState)>;

class DirectConnectHook {
public:
    DirectConnectHook() = default;

    // 启用直接连接
    auto enable(DirectConnectConfig config) -> void {
        config_ = std::move(config);
        enabled_ = true;
        state_ = ConnectionState::disconnected;
    }

    // 禁用直接连接
    auto disable() -> void {
        enabled_ = false;
        if (state_ == ConnectionState::connected) {
            state_ = ConnectionState::disconnected;
            if (state_callback_) state_callback_(state_);
        }
    }

    [[nodiscard]] auto is_enabled() const -> bool { return enabled_; }
    [[nodiscard]] auto get_url() const -> std::string_view { return config_.url; }
    [[nodiscard]] auto get_state() const -> ConnectionState { return state_; }

    // 设置连接状态回调
    auto on_state_change(ConnectionStateCallback callback) -> void {
        state_callback_ = std::move(callback);
    }

    // 设置消息回调
    auto on_message(MessageCallback callback) -> void {
        message_callback_ = std::move(callback);
    }

    // 连接
    auto connect() -> bool {
        if (!enabled_) return false;
        state_ = ConnectionState::connecting;
        if (state_callback_) state_callback_(state_);
        
        // 模拟连接过程
        state_ = ConnectionState::connected;
        if (state_callback_) state_callback_(state_);
        return true;
    }

    // 断开连接
    auto disconnect() -> void {
        if (state_ == ConnectionState::connected) {
            state_ = ConnectionState::disconnected;
            if (state_callback_) state_callback_(state_);
        }
    }

    // 发送消息
    auto send_message(std::string_view message) -> bool {
        if (state_ != ConnectionState::connected) return false;
        // 实际实现会通过WebSocket发送
        return true;
    }

private:
    bool enabled_{false};
    DirectConnectConfig config_;
    ConnectionState state_{ConnectionState::disconnected};
    ConnectionStateCallback state_callback_;
    MessageCallback message_callback_;
};

// ============================================================================
// useDoublePress: Double key press detection
// ============================================================================

class DoublePressHook {
public:
    explicit DoublePressHook(std::chrono::milliseconds threshold = std::chrono::milliseconds{800})
        : threshold_(threshold) {}

    // 检查是否为双击，返回true表示检测到双击
    auto check(std::string_view key) -> bool {
        auto now = std::chrono::steady_clock::now();
        bool is_double = (key == last_key_) && (now - last_press_ < threshold_);
        
        last_press_ = now;
        last_key_ = std::string(key);
        
        return is_double;
    }

    // 设置双击阈值
    auto set_threshold(std::chrono::milliseconds ms) -> void { threshold_ = ms; }

    // 重置状态
    auto reset() -> void {
        last_key_.clear();
        last_press_ = std::chrono::steady_clock::time_point{};
    }

private:
    std::chrono::milliseconds threshold_;
    std::chrono::steady_clock::time_point last_press_;
    std::string last_key_;
};

// ============================================================================
// useDynamicConfig: Dynamic config management
// ============================================================================

class DynamicConfigHook {
public:
    DynamicConfigHook() = default;

    // 获取配置值
    template<typename T>
    [[nodiscard]] auto get(std::string_view key, T default_value) const -> T {
        std::lock_guard lock(mu_);
        if (auto it = config_.find(std::string(key)); it != config_.end()) {
            // 简化实现，实际需要类型转换
            if constexpr (std::is_same_v<T, std::string>) {
                return it->second;
            } else if constexpr (std::is_same_v<T, bool>) {
                return it->second == "true";
            } else if constexpr (std::is_same_v<T, int>) {
                return std::stoi(it->second);
            }
        }
        return default_value;
    }

    // 设置配置值
    template<typename T>
    auto set(std::string key, T value) -> void {
        std::lock_guard lock(mu_);
        if constexpr (std::is_same_v<T, std::string>) {
            config_[std::move(key)] = value;
        } else if constexpr (std::is_same_v<T, bool>) {
            config_[std::move(key)] = value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int>) {
            config_[std::move(key)] = std::to_string(value);
        }
    }

    // 刷新配置
    auto refresh() -> void {
        std::lock_guard lock(mu_);
        last_refresh_ = std::chrono::steady_clock::now();
    }

    // 检查是否需要刷新
    [[nodiscard]] auto needs_refresh(std::chrono::seconds interval = std::chrono::seconds{60}) const -> bool {
        std::lock_guard lock(mu_);
        return (std::chrono::steady_clock::now() - last_refresh_) > interval;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> config_;
    std::chrono::steady_clock::time_point last_refresh_;
};

// ============================================================================
// useElapsedTime: Elapsed time tracking
// ============================================================================

class ElapsedTimeHook {
public:
    ElapsedTimeHook() = default;

    // 开始计时
    auto start() -> void {
        start_time_ = std::chrono::steady_clock::now();
        running_ = true;
        paused_ = false;
        paused_time_ = std::chrono::milliseconds{0};
    }

    // 暂停计时
    auto pause() -> void {
        if (!running_ || paused_) return;
        pause_time_ = std::chrono::steady_clock::now();
        paused_ = true;
    }

    // 恢复计时
    auto resume() -> void {
        if (!running_ || !paused_) return;
        paused_time_ += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pause_time_
        );
        paused_ = false;
    }

    // 停止计时
    auto stop() -> void {
        if (running_) {
            end_time_ = std::chrono::steady_clock::now();
            running_ = false;
        }
    }

    // 获取已流逝时间
    [[nodiscard]] auto elapsed() const -> std::chrono::milliseconds {
        if (!running_) {
            if (!start_time_.time_since_epoch().count()) return std::chrono::milliseconds{0};
            return std::chrono::duration_cast<std::chrono::milliseconds>(end_time_ - start_time_) - paused_time_;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = paused_ ? pause_time_ - start_time_ : now - start_time_;
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed) - paused_time_;
    }

    // 获取格式化的时间字符串
    [[nodiscard]] auto formatted() const -> std::string {
        auto ms = elapsed().count();
        auto seconds = ms / 1000;
        auto minutes = seconds / 60;
        auto hours = minutes / 60;
        
        if (hours > 0) {
            return std::format("{}h {}m {}s", hours, minutes % 60, seconds % 60);
        } else if (minutes > 0) {
            return std::format("{}m {}s", minutes, seconds % 60);
        } else {
            return std::format("{}.{}s", seconds, (ms % 1000) / 100);
        }
    }

    [[nodiscard]] auto is_running() const -> bool { return running_; }
    [[nodiscard]] auto is_paused() const -> bool { return paused_; }

private:
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point end_time_;
    std::chrono::steady_clock::time_point pause_time_;
    std::chrono::milliseconds paused_time_{0};
    bool running_{false};
    bool paused_{false};
};

// ============================================================================
// useFileSuggestions: File path suggestions
// ============================================================================

struct FileSuggestion {
    std::string path;
    std::string display_text;
    double score{0.0};
    bool is_directory{false};
};

class FileSuggestionsHook {
public:
    FileSuggestionsHook() = default;

    // 生成文件建议
    [[nodiscard]] auto suggest(std::string_view partial_path, std::size_t max_results = 15) const -> std::vector<FileSuggestion> {
        std::vector<FileSuggestion> results;
        
        // 简化实现，实际需要扫描文件系统
        try {
            std::filesystem::path base_path = std::filesystem::current_path();
            std::filesystem::path search_dir = base_path;
            std::string partial_name = std::string(partial_path);
            
            // 提取目录部分
            auto last_slash = partial_name.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                search_dir = base_path / partial_name.substr(0, last_slash);
                partial_name = partial_name.substr(last_slash + 1);
            }
            
            if (std::filesystem::exists(search_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(search_dir)) {
                    auto filename = entry.path().filename().string();
                    if (partial_name.empty() || filename.find(partial_name) != std::string::npos) {
                        FileSuggestion suggestion{
                            .path = entry.path().lexically_relative(base_path).string(),
                            .display_text = filename,
                            .score = 1.0,
                            .is_directory = entry.is_directory()
                        };
                        if (suggestion.is_directory) {
                            suggestion.path += "/";
                        }
                        results.push_back(std::move(suggestion));
                        if (results.size() >= max_results) break;
                    }
                }
            }
        } catch (...) {
            // 忽略文件系统错误
        }
        
        // 按分数排序
        std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
            return a.score > b.score;
        });
        
        return results;
    }

    // 刷新文件索引（后台）
    auto refresh_index() -> void {
        // Re-scan the current working directory to update cached suggestions
        try {
            auto base = std::filesystem::current_path();
            indexed_paths_.clear();
            for (const auto& entry : std::filesystem::directory_iterator(base)) {
                indexed_paths_.push_back(entry.path().filename().string());
            }
        } catch (...) {}
    }

private:
    std::vector<std::string> indexed_paths_;
};

// ============================================================================
// useHistorySearch: Command history search
// ============================================================================

struct HistoryEntry {
    std::string id;
    std::string display_text;
    std::string value;
    std::chrono::system_clock::time_point timestamp;
    std::unordered_map<std::string, std::string> metadata;
};

using HistorySelectCallback = std::function<void(const HistoryEntry&)>;

class HistorySearchHook {
public:
    HistorySearchHook() = default;

    // 设置历史记录
    auto set_history(std::vector<HistoryEntry> history) -> void {
        history_ = std::move(history);
    }

    // 开始搜索
    auto start_search(std::string_view initial_query = "") -> void {
        searching_ = true;
        query_ = std::string(initial_query);
        update_matches();
    }

    // 结束搜索
    auto end_search() -> void {
        searching_ = false;
        query_.clear();
        matches_.clear();
        current_match_index_ = 0;
    }

    // 设置搜索查询
    auto set_query(std::string_view query) -> void {
        query_ = std::string(query);
        update_matches();
    }

    // 获取当前查询
    [[nodiscard]] auto get_query() const -> std::string_view { return query_; }

    // 获取当前匹配项
    [[nodiscard]] auto get_current_match() const -> std::optional<HistoryEntry> {
        if (matches_.empty() || current_match_index_ >= matches_.size()) return std::nullopt;
        return history_[matches_[current_match_index_]];
    }

    // 下一个匹配项
    auto next_match() -> void {
        if (!matches_.empty()) {
            current_match_index_ = (current_match_index_ + 1) % matches_.size();
        }
    }

    // 上一个匹配项
    auto prev_match() -> void {
        if (!matches_.empty()) {
            current_match_index_ = (current_match_index_ - 1 + matches_.size()) % matches_.size();
        }
    }

    // 接受当前匹配
    auto accept_match(HistorySelectCallback callback) -> void {
        if (auto match = get_current_match()) {
            callback(*match);
        }
        end_search();
    }

    [[nodiscard]] auto is_searching() const -> bool { return searching_; }
    [[nodiscard]] auto has_matches() const -> bool { return !matches_.empty(); }
    [[nodiscard]] auto match_count() const -> std::size_t { return matches_.size(); }

private:
    auto update_matches() -> void {
        matches_.clear();
        for (std::size_t i = 0; i < history_.size(); ++i) {
            if (query_.empty() || 
                history_[i].display_text.find(query_) != std::string::npos) {
                matches_.push_back(i);
            }
        }
        current_match_index_ = 0;
    }

    std::vector<HistoryEntry> history_;
    std::vector<std::size_t> matches_;
    std::size_t current_match_index_{0};
    std::string query_;
    bool searching_{false};
};

// ============================================================================
// useIdeSelection: IDE selection tracking
// ============================================================================

struct IdeSelectionPoint {
    int line{0};
    int character{0};
};

struct IdeSelectionData {
    std::optional<std::pair<IdeSelectionPoint, IdeSelectionPoint>> selection;
    std::optional<std::string> text;
    std::optional<std::string> file_path;
};

using SelectionChangeCallback = std::function<void(const IdeSelectionData&)>;

class IdeSelectionHook {
public:
    IdeSelectionHook() = default;

    // 更新选区
    auto update_selection(IdeSelectionData data) -> void {
        current_selection_ = std::move(data);
        if (change_callback_) change_callback_(current_selection_);
    }

    // 获取当前选区
    [[nodiscard]] auto get_selection() const -> const IdeSelectionData& { return current_selection_; }

    // 注册选区变化回调
    auto on_selection_change(SelectionChangeCallback callback) -> void {
        change_callback_ = std::move(callback);
    }

    // 获取选中文本（如果有）
    [[nodiscard]] auto get_selected_text() const -> std::optional<std::string> {
        return current_selection_.text;
    }

    // 获取选中文件路径
    [[nodiscard]] auto get_file_path() const -> std::optional<std::string> {
        return current_selection_.file_path;
    }

    // 检查是否有选区
    [[nodiscard]] auto has_selection() const -> bool {
        return current_selection_.selection.has_value();
    }

private:
    IdeSelectionData current_selection_;
    SelectionChangeCallback change_callback_;
};

// ============================================================================
// useLogMessages: Message logging
// ============================================================================

enum class LogLevel { debug, info, warning, error };

struct LogMessage {
    std::string source;
    std::string message;
    LogLevel level{LogLevel::info};
    std::chrono::system_clock::time_point timestamp;
};

class LogMessagesHook {
public:
    explicit LogMessagesHook(std::size_t max_messages = 500) : max_messages_(max_messages) {}

    // 记录消息
    auto log(std::string_view source, std::string_view message, LogLevel level = LogLevel::info) -> void {
        LogMessage msg{
            .source = std::string(source),
            .message = std::string(message),
            .level = level,
            .timestamp = std::chrono::system_clock::now()
        };
        messages_.push_back(std::move(msg));
        
        while (messages_.size() > max_messages_) {
            messages_.erase(messages_.begin());
        }
    }

    // 获取最近的消息
    [[nodiscard]] auto recent(std::size_t count = 20) const -> std::vector<LogMessage> {
        if (messages_.size() <= count) return messages_;
        return std::vector<LogMessage>(messages_.end() - static_cast<std::ptrdiff_t>(count), messages_.end());
    }

    // 清除所有消息
    auto clear() -> void { messages_.clear(); }

    // 获取消息总数
    [[nodiscard]] auto size() const -> std::size_t { return messages_.size(); }

    // 设置最大消息数
    auto set_max_messages(std::size_t max) -> void { max_messages_ = max; }

private:
    std::vector<LogMessage> messages_;
    std::size_t max_messages_;
};

// ============================================================================
// useMainLoopModel: Main loop model selection
// ============================================================================

enum class ModelType { claude_3_5_sonnet, claude_3_opus, claude_3_haiku, custom };

struct ModelConfig {
    std::string name;
    ModelType type{ModelType::claude_3_5_sonnet};
    std::string api_endpoint;
    std::size_t context_window{200000};
};

class MainLoopModelHook {
public:
    MainLoopModelHook() = default;

    // 设置当前模型
    auto set_model(ModelConfig config) -> void {
        current_model_ = std::move(config);
    }

    // 获取当前模型
    [[nodiscard]] auto get_model() const -> const ModelConfig& { return current_model_; }

    // 切换到默认模型
    auto use_default_model() -> void {
        current_model_ = ModelConfig{
            .name = "claude-3-5-sonnet",
            .type = ModelType::claude_3_5_sonnet,
            .context_window = 200000
        };
    }

    // 设置会话模型
    auto set_session_model(std::optional<ModelConfig> model) -> void {
        session_model_ = std::move(model);
    }

    // 获取有效模型（会话模型优先）
    [[nodiscard]] auto get_effective_model() const -> const ModelConfig& {
        if (session_model_) return *session_model_;
        return current_model_;
    }

private:
    ModelConfig current_model_{
        .name = "claude-3-5-sonnet",
        .type = ModelType::claude_3_5_sonnet,
        .context_window = 200000
    };
    std::optional<ModelConfig> session_model_;
};

// ============================================================================
// useManagePlugins: Plugin management
// ============================================================================

enum class PluginState { disabled, enabled, error };

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    PluginState state{PluginState::disabled};
    std::string error_message;
};

using PluginChangeCallback = std::function<void(std::string_view, PluginState)>;

class ManagePluginsHook {
public:
    ManagePluginsHook() = default;

    // 加载所有插件
    auto load_plugins() -> void {
        // Scan the plugins directory and register discovered plugins
        try {
            auto plugins_dir = std::filesystem::current_path() / ".cc-repl" / "plugins";
            if (std::filesystem::exists(plugins_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(plugins_dir)) {
                    if (entry.is_directory()) {
                        auto name = entry.path().filename().string();
                        if (plugins_.find(name) == plugins_.end()) {
                            plugins_[name] = PluginInfo{
                                .id = name,
                                .name = name,
                                .version = "0.0.0",
                                .description = "",
                                .state = PluginState::disabled,
                                .error_message = {},
                            };
                        }
                    }
                }
            }
        } catch (...) {}
        needs_refresh_ = false;
    }

    // 启用插件
    auto enable_plugin(std::string_view id) -> bool {
        if (auto it = plugins_.find(std::string(id)); it != plugins_.end()) {
            it->second.state = PluginState::enabled;
            if (change_callback_) change_callback_(id, PluginState::enabled);
            return true;
        }
        return false;
    }

    // 禁用插件
    auto disable_plugin(std::string_view id) -> bool {
        if (auto it = plugins_.find(std::string(id)); it != plugins_.end()) {
            it->second.state = PluginState::disabled;
            if (change_callback_) change_callback_(id, PluginState::disabled);
            return true;
        }
        return false;
    }

    // 获取所有插件
    [[nodiscard]] auto get_plugins() const -> std::vector<PluginInfo> {
        std::vector<PluginInfo> result;
        result.reserve(plugins_.size());
        for (const auto& [id, info] : plugins_) {
            result.push_back(info);
        }
        return result;
    }

    // 获取已启用的插件
    [[nodiscard]] auto get_enabled_plugins() const -> std::vector<PluginInfo> {
        std::vector<PluginInfo> result;
        for (const auto& [id, info] : plugins_) {
            if (info.state == PluginState::enabled) {
                result.push_back(info);
            }
        }
        return result;
    }

    // 检查是否需要刷新
    [[nodiscard]] auto needs_refresh() const -> bool { return needs_refresh_; }

    // 设置需要刷新
    auto set_needs_refresh(bool needs) -> void { needs_refresh_ = needs; }

    // 注册插件变化回调
    auto on_plugin_change(PluginChangeCallback callback) -> void {
        change_callback_ = std::move(callback);
    }

private:
    std::unordered_map<std::string, PluginInfo> plugins_;
    bool needs_refresh_{false};
    PluginChangeCallback change_callback_;
};

// ============================================================================
// usePrStatus: Pull Request status monitoring
// ============================================================================

enum class PrReviewState { approved, changes_requested, commented, dismissed, pending };

struct PrStatus {
    std::optional<int> number;
    std::optional<std::string> url;
    std::optional<PrReviewState> review_state;
    std::chrono::system_clock::time_point last_updated;
};

using PrStatusChangeCallback = std::function<void(const PrStatus&)>;

class PrStatusHook {
public:
    PrStatusHook() = default;

    // 启用/禁用PR状态轮询
    auto enable(bool enabled = true) -> void { enabled_ = enabled; }
    [[nodiscard]] auto is_enabled() const -> bool { return enabled_; }

    // 设置轮询间隔
    auto set_poll_interval(std::chrono::milliseconds interval) -> void { poll_interval_ = interval; }

    // 获取当前PR状态
    [[nodiscard]] auto get_status() const -> const PrStatus& { return status_; }

    // 手动刷新状态
    auto refresh() -> void {
        // 实际实现会调用GitHub API
        last_refresh_ = std::chrono::steady_clock::now();
    }

    // 注册状态变化回调
    auto on_status_change(PrStatusChangeCallback callback) -> void {
        status_callback_ = std::move(callback);
    }

    // 更新PR状态（内部使用）
    auto update_status(PrStatus new_status) -> void {
        status_ = std::move(new_status);
        if (status_callback_) status_callback_(status_);
    }

private:
    bool enabled_{true};
    PrStatus status_;
    std::chrono::milliseconds poll_interval_{60000};
    std::chrono::steady_clock::time_point last_refresh_;
    PrStatusChangeCallback status_callback_;
};

// ============================================================================
// useRemoteSession: Remote session management
// ============================================================================

enum class RemoteState { disconnected, connecting, connected, error };

struct RemoteMessage {
    std::string content;
    std::string type;
    std::optional<std::string> id;
};

using RemoteMessageCallback = std::function<void(const RemoteMessage&)>;
using RemoteStateCallback = std::function<void(RemoteState)>;

class RemoteSessionHook {
public:
    RemoteSessionHook() = default;

    // 连接到远程会话
    auto connect(std::string_view session_url) -> bool {
        session_url_ = std::string(session_url);
        state_ = RemoteState::connecting;
        if (state_callback_) state_callback_(state_);
        
        // 模拟连接过程
        state_ = RemoteState::connected;
        if (state_callback_) state_callback_(state_);
        return true;
    }

    // 断开连接
    auto disconnect() -> void {
        if (state_ == RemoteState::connected) {
            state_ = RemoteState::disconnected;
            if (state_callback_) state_callback_(state_);
        }
    }

    // 发送消息
    auto send_message(std::string_view content) -> bool {
        if (state_ != RemoteState::connected) return false;
        // 实际实现会发送到远程
        return true;
    }

    // 取消当前请求
    auto cancel_request() -> void {
        // Signal cancellation by transitioning to disconnected if connected
        if (state_ == RemoteState::connected) {
            state_ = RemoteState::disconnected;
            if (state_callback_) state_callback_(state_);
        }
    }

    // 获取当前状态
    [[nodiscard]] auto get_state() const -> RemoteState { return state_; }

    // 检查是否是远程模式
    [[nodiscard]] auto is_remote_mode() const -> bool { return state_ == RemoteState::connected; }

    // 注册消息回调
    auto on_message(RemoteMessageCallback callback) -> void {
        message_callback_ = std::move(callback);
    }

    // 注册状态变化回调
    auto on_state_change(RemoteStateCallback callback) -> void {
        state_callback_ = std::move(callback);
    }

private:
    std::string session_url_;
    RemoteState state_{RemoteState::disconnected};
    RemoteMessageCallback message_callback_;
    RemoteStateCallback state_callback_;
};

// ============================================================================
// useSSHSession: SSH session management
// ============================================================================

struct SSHConfig {
    std::string host;
    std::string user;
    int port{22};
    std::optional<std::string> identity_file;
};

using SSHMessageCallback = std::function<void(std::string_view)>;
using SSHStateCallback = std::function<void(RemoteState)>;

class SSHSessionHook {
public:
    SSHSessionHook() = default;

    // 设置SSH配置并连接
    auto connect(SSHConfig config) -> bool {
        config_ = std::move(config);
        state_ = RemoteState::connecting;
        if (state_callback_) state_callback_(state_);
        
        // 模拟连接
        state_ = RemoteState::connected;
        if (state_callback_) state_callback_(RemoteState::connected);
        return true;
    }

    // 断开
    auto disconnect() -> void {
        state_ = RemoteState::disconnected;
        if (state_callback_) state_callback_(state_);
    }

    // 发送命令
    auto send_command(std::string_view cmd) -> bool {
        if (state_ != RemoteState::connected) return false;
        // 实际会通过SSH通道发送
        return true;
    }

    [[nodiscard]] auto get_state() const -> RemoteState { return state_; }
    [[nodiscard]] auto is_connected() const -> bool { return state_ == RemoteState::connected; }

    // 注册回调
    auto on_message(SSHMessageCallback callback) -> void { message_callback_ = std::move(callback); }
    auto on_state_change(SSHStateCallback callback) -> void { state_callback_ = std::move(callback); }

private:
    SSHConfig config_;
    RemoteState state_{RemoteState::disconnected};
    SSHMessageCallback message_callback_;
    SSHStateCallback state_callback_;
};

// ============================================================================
// useTasksV2: Task management V2
// ============================================================================

enum class TaskStatus { pending, in_progress, completed, cancelled, failed };

struct TaskV2 {
    std::string id;
    std::string title;
    std::string description;
    TaskStatus status{TaskStatus::pending};
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
    std::unordered_map<std::string, std::string> metadata;
};

using TaskChangeCallback = std::function<void(const TaskV2&)>;
using TaskListChangeCallback = std::function<void()>;

class TasksV2Hook {
public:
    TasksV2Hook() = default;

    // 添加任务
    auto add_task(TaskV2 task) -> std::string {
        task.id = generate_task_id();
        task.created_at = std::chrono::system_clock::now();
        task.updated_at = task.created_at;
        tasks_[task.id] = std::move(task);
        if (list_callback_) list_callback_();
        return task.id;
    }

    // 更新任务
    auto update_task(std::string_view id, TaskStatus status) -> bool {
        if (auto it = tasks_.find(std::string(id)); it != tasks_.end()) {
            it->second.status = status;
            it->second.updated_at = std::chrono::system_clock::now();
            if (status == TaskStatus::completed) {
                it->second.completed_at = std::chrono::system_clock::now();
            }
            if (change_callback_) change_callback_(it->second);
            if (list_callback_) list_callback_();
            return true;
        }
        return false;
    }

    // 获取任务
    [[nodiscard]] auto get_task(std::string_view id) const -> std::optional<TaskV2> {
        if (auto it = tasks_.find(std::string(id)); it != tasks_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // 获取所有任务
    [[nodiscard]] auto get_tasks() const -> std::vector<TaskV2> {
        std::vector<TaskV2> result;
        result.reserve(tasks_.size());
        for (const auto& [id, task] : tasks_) {
            result.push_back(task);
        }
        // 按更新时间排序
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.updated_at > b.updated_at;
        });
        return result;
    }

    // 获取未完成任务
    [[nodiscard]] auto get_pending_tasks() const -> std::vector<TaskV2> {
        std::vector<TaskV2> result;
        for (const auto& [id, task] : tasks_) {
            if (task.status != TaskStatus::completed && task.status != TaskStatus::cancelled) {
                result.push_back(task);
            }
        }
        return result;
    }

    // 删除任务
    auto delete_task(std::string_view id) -> bool {
        if (tasks_.erase(std::string(id)) > 0) {
            if (list_callback_) list_callback_();
            return true;
        }
        return false;
    }

    // 注册回调
    auto on_task_change(TaskChangeCallback callback) -> void { change_callback_ = std::move(callback); }
    auto on_list_change(TaskListChangeCallback callback) -> void { list_callback_ = std::move(callback); }

    // 设置隐藏延迟
    auto set_hide_delay(std::chrono::milliseconds delay) -> void { hide_delay_ = delay; }

private:
    [[nodiscard]] auto generate_task_id() -> std::string {
        static std::uint64_t counter = 0;
        return "task_" + std::to_string(++counter);
    }

    std::unordered_map<std::string, TaskV2> tasks_;
    TaskChangeCallback change_callback_;
    TaskListChangeCallback list_callback_;
    std::chrono::milliseconds hide_delay_{5000};
};

// ============================================================================
// useTimeout: Generic timeout
// ============================================================================

using TimeoutCallback = std::function<void()>;

class TimeoutHook {
public:
    TimeoutHook() = default;

    // 设置超时
    auto set(std::chrono::milliseconds duration, TimeoutCallback callback) -> void {
        duration_ = duration;
        callback_ = std::move(callback);
        start_time_ = std::chrono::steady_clock::now();
        active_ = true;
        elapsed_ = false;
    }

    // 取消超时
    auto cancel() -> void {
        active_ = false;
        elapsed_ = false;
    }

    // 检查是否超时（需要定期调用）
    auto check() -> void {
        if (!active_ || elapsed_) return;
        auto elapsed = std::chrono::steady_clock::now() - start_time_;
        if (elapsed >= duration_) {
            elapsed_ = true;
            active_ = false;
            if (callback_) callback_();
        }
    }

    // 重置超时
    auto reset() -> void {
        if (active_) {
            start_time_ = std::chrono::steady_clock::now();
            elapsed_ = false;
        }
    }

    [[nodiscard]] auto is_active() const -> bool { return active_; }
    [[nodiscard]] auto has_elapsed() const -> bool { return elapsed_; }

    // 获取剩余时间
    [[nodiscard]] auto remaining() const -> std::chrono::milliseconds {
        if (!active_) return std::chrono::milliseconds{0};
        auto elapsed = std::chrono::steady_clock::now() - start_time_;
        auto rem = duration_ - elapsed;
        return rem.count() > 0 ? std::chrono::duration_cast<std::chrono::milliseconds>(rem) : std::chrono::milliseconds{0};
    }

private:
    std::chrono::milliseconds duration_;
    TimeoutCallback callback_;
    std::chrono::steady_clock::time_point start_time_;
    bool active_{false};
    bool elapsed_{false};
};

// ============================================================================
// useVoiceEnabled: Voice feature flag
// ============================================================================

class VoiceEnabledHook {
public:
    VoiceEnabledHook() = default;

    // 设置用户偏好
    auto set_user_enabled(bool enabled) -> void { user_enabled_ = enabled; }

    // 设置是否有权限
    auto set_has_auth(bool has_auth) -> void { has_auth_ = has_auth; }

    // 设置功能开关
    auto set_feature_enabled(bool enabled) -> void { feature_enabled_ = enabled; }

    // 检查语音功能是否完全启用
    [[nodiscard]] auto is_enabled() const -> bool {
        return user_enabled_ && has_auth_ && feature_enabled_;
    }

    // 单独检查各项
    [[nodiscard]] auto user_enabled() const -> bool { return user_enabled_; }
    [[nodiscard]] auto has_auth() const -> bool { return has_auth_; }
    [[nodiscard]] auto feature_enabled() const -> bool { return feature_enabled_; }

private:
    bool user_enabled_{false};
    bool has_auth_{false};
    bool feature_enabled_{true};
};

} // namespace cc::hooks
