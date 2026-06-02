// C++23 Module: Multi-line paste detection and processing with bracketed paste mode support
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.paste_handler;


export namespace cc::hooks {

// 粘贴事件来源
enum class PasteSource {
    bracketed,   // 终端 bracketed paste mode (ESC[200~ ... ESC[201~)
    detected,    // 通过快速输入检测到的粘贴
    manual,      // 手动触发（如用户操作）
};

// 粘贴事件
struct PasteEvent {
    std::string content;             // 粘贴的文本内容
    PasteSource source{PasteSource::bracketed};
    bool is_bracketed{false};        // 是否通过 bracketed paste 模式检测
    std::size_t char_count{0};       // 字符数（可能与 content.size() 不同因为 UTF-8）
    std::size_t line_count{0};       // 行数

    // 计算统计信息
    auto compute_stats() -> void {
        char_count = content.size();
        line_count = 1;
        for (char c : content) {
            if (c == '\n') ++line_count;
        }
    }
};

// 粘贴处理器配置
struct PasteConfig {
    std::size_t max_size{1048576};            // 最大粘贴大小（1MB）
    bool strip_trailing_newline{true};        // 是否去除尾部换行
    bool auto_indent{false};                  // 是否自动缩进
    std::size_t confirm_threshold{1000};      // 超过此字符数需要确认
    std::string indent_string{"    "};        // 自动缩进使用的字符串
};

// 确认回调类型：返回 true 表示用户确认粘贴
using PasteConfirmFn = std::function<bool(const PasteEvent&)>;
// 粘贴事件回调
using PasteCallback = std::function<void(const PasteEvent&)>;

// PasteHandler: 管理粘贴检测、缓冲和处理
class PasteHandler {
public:
    explicit PasteHandler(PasteConfig config = {})
        : config_(std::move(config)) {}

    // 粘贴开始（收到 ESC[200~ 标记时调用）
    auto on_paste_start() -> void {
        pasting_ = true;
        paste_buffer_.clear();
        paste_start_time_ = std::chrono::steady_clock::now();
    }

    // 接收粘贴数据
    auto on_paste_data(const std::uint8_t* bytes, std::size_t len) -> void {
        if (!pasting_) return;

        // 限制最大粘贴大小
        auto remaining = config_.max_size - paste_buffer_.size();
        auto to_append = std::min(len, remaining);
        paste_buffer_.append(reinterpret_cast<const char*>(bytes), to_append);

        if (paste_buffer_.size() >= config_.max_size) {
            truncated_ = true;
        }
    }

    // 字符串重载
    auto on_paste_data(std::string_view data) -> void {
        on_paste_data(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }

    // 粘贴结束（收到 ESC[201~ 标记时调用），返回完整的粘贴事件
    [[nodiscard]] auto on_paste_end() -> PasteEvent {
        pasting_ = false;

        PasteEvent event{
            .content = std::move(paste_buffer_),
            .source = PasteSource::bracketed,
            .is_bracketed = true,
        };
        event.compute_stats();

        paste_buffer_.clear();
        truncated_ = false;

        // 通知监听器
        for (const auto& cb : callbacks_) {
            cb(event);
        }

        return event;
    }

    // 判断是否需要用户确认（大粘贴）
    [[nodiscard]] auto should_confirm(const PasteEvent& event) const -> bool {
        return event.char_count > config_.confirm_threshold;
    }

    /**
     * 处理粘贴内容：应用配置中的转换规则。
     * - 去除尾部换行
     * - 自动缩进
     * - 大小限制
     */
    [[nodiscard]] auto process_paste(const PasteEvent& event,
                                      std::string_view current_indent = "") const -> std::string {
        std::string result = event.content;

        // 去除尾部换行
        if (config_.strip_trailing_newline) {
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
                result.pop_back();
            }
        }

        // 自动缩进：对多行粘贴的每一行添加当前缩进
        if (config_.auto_indent && !current_indent.empty() && event.line_count > 1) {
            result = apply_indent(result, current_indent);
        }

        return result;
    }

    // 设置配置
    auto set_config(PasteConfig config) -> void { config_ = std::move(config); }

    // 获取配置
    [[nodiscard]] auto config() const -> const PasteConfig& { return config_; }

    // 是否正在接收粘贴数据
    [[nodiscard]] auto is_pasting() const -> bool { return pasting_; }

    // 是否因为超出大小限制而截断
    [[nodiscard]] auto is_truncated() const -> bool { return truncated_; }

    // 获取当前缓冲区大小
    [[nodiscard]] auto buffer_size() const -> std::size_t { return paste_buffer_.size(); }

    // 注册粘贴事件回调
    auto on_paste(PasteCallback callback) -> void {
        callbacks_.push_back(std::move(callback));
    }

    // 设置确认函数
    auto set_confirm_fn(PasteConfirmFn fn) -> void {
        confirm_fn_ = std::move(fn);
    }

    // 使用确认函数请求用户确认
    [[nodiscard]] auto request_confirm(const PasteEvent& event) -> bool {
        if (confirm_fn_) return confirm_fn_(event);
        return true; // 无确认函数时默认允许
    }

    // 从非 bracketed 来源创建粘贴事件
    [[nodiscard]] static auto create_detected_paste(std::string_view content) -> PasteEvent {
        PasteEvent event{
            .content = std::string(content),
            .source = PasteSource::detected,
            .is_bracketed = false,
        };
        event.compute_stats();
        return event;
    }

private:
    PasteConfig config_;
    std::string paste_buffer_;
    bool pasting_{false};
    bool truncated_{false};
    std::chrono::steady_clock::time_point paste_start_time_;
    std::vector<PasteCallback> callbacks_;
    PasteConfirmFn confirm_fn_;

    // 对多行文本应用缩进
    [[nodiscard]] static auto apply_indent(std::string_view text,
                                            std::string_view indent) -> std::string {
        std::string result;
        result.reserve(text.size() + text.size() / 40 * indent.size());

        bool at_line_start = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (at_line_start && text[i] != '\n') {
                result.append(indent);
                at_line_start = false;
            }
            result.push_back(text[i]);
            if (text[i] == '\n') {
                at_line_start = true;
            }
        }
        return result;
    }
};

} // namespace cc::hooks
