// C++23 Module: Raw input buffering with escape sequence detection and UTF-8 handling
module;

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module cc.hooks.input_buffer;


export namespace cc::hooks {

// 键盘修饰符
struct KeyModifiers {
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
    bool meta{false};

    [[nodiscard]] auto any() const -> bool { return ctrl || alt || shift || meta; }
    auto operator==(const KeyModifiers&) const -> bool = default;
};

// 解析后的键盘事件
struct KeyEvent {
    std::string key;             // 键名："a", "Enter", "Escape", "ArrowUp" 等
    KeyModifiers modifiers;
    bool is_paste{false};        // 是否属于粘贴流的一部分
    std::vector<std::uint8_t> raw_bytes;  // 原始字节序列

    // 生成规范化表示字符串
    [[nodiscard]] auto canonical() const -> std::string {
        std::string result;
        if (modifiers.ctrl)  result += "Ctrl+";
        if (modifiers.alt)   result += "Alt+";
        if (modifiers.shift) result += "Shift+";
        if (modifiers.meta)  result += "Meta+";
        result += key;
        return result;
    }
};

// 输入缓冲区状态信息
struct InputBufferState {
    std::size_t pending_bytes{0};   // 缓冲区中待处理的字节数
    std::chrono::steady_clock::time_point last_input_time;
    bool in_escape_sequence{false}; // 当前是否正在解析转义序列
    bool in_bracketed_paste{false}; // 是否在 bracketed paste 模式中
};

// 取消订阅类型
using UnsubscribeFn = std::function<void()>;
using KeyCallback = std::function<void(const KeyEvent&)>;

// InputBuffer: 管理原始输入字节的缓冲、解析和事件分发
class InputBuffer {
    using Clock = std::chrono::steady_clock;

    // 转义序列类型
    enum class EscapeType { None, CSI, OSC, SS3, Unknown };

public:
    InputBuffer() = default;

    // 向缓冲区送入原始字节
    auto feed(const std::uint8_t* bytes, std::size_t len) -> void {
        for (std::size_t i = 0; i < len; ++i) {
            buffer_.push_back(bytes[i]);
        }
        last_input_time_ = Clock::now();
        try_parse();
    }

    // 带容器的 feed 重载
    auto feed(std::span<const std::uint8_t> bytes) -> void {
        feed(bytes.data(), bytes.size());
    }

    // 从已解析的事件队列中取出一个事件
    [[nodiscard]] auto poll() -> std::optional<KeyEvent> {
        // 检查转义超时：如果只有单独的 ESC 且超时了，当作 Escape 键处理
        check_escape_timeout();

        if (parsed_events_.empty()) return std::nullopt;
        auto event = std::move(parsed_events_.front());
        parsed_events_.pop_front();
        return event;
    }

    // 是否有待处理的数据
    [[nodiscard]] auto has_pending() const -> bool {
        return !buffer_.empty() || !parsed_events_.empty();
    }

    // 设置转义序列歧义超时（区分 Alt+key 和单独 Escape）
    auto set_escape_timeout(std::chrono::milliseconds ms) -> void {
        escape_timeout_ = ms;
    }

    // 启用/禁用 bracketed paste 模式
    auto enable_bracketed_paste(bool enable) -> void {
        bracketed_paste_enabled_ = enable;
    }

    // 清空缓冲区和事件队列
    auto flush() -> void {
        buffer_.clear();
        parsed_events_.clear();
        in_escape_ = false;
        in_bracketed_paste_ = false;
    }

    // 注册键盘事件回调，返回取消注册函数
    [[nodiscard]] auto on_key(KeyCallback callback) -> UnsubscribeFn {
        auto id = next_sub_id_++;
        subscribers_.push_back({.id = id, .callback = std::move(callback)});
        return [this, id]() {
            std::erase_if(subscribers_, [id](const auto& s) { return s.id == id; });
        };
    }

    // 获取缓冲区状态
    [[nodiscard]] auto state() const -> InputBufferState {
        return InputBufferState{
            .pending_bytes = buffer_.size(),
            .last_input_time = last_input_time_,
            .in_escape_sequence = in_escape_,
            .in_bracketed_paste = in_bracketed_paste_
        };
    }

private:
    struct Subscriber {
        std::uint64_t id;
        KeyCallback callback;
    };

    std::vector<std::uint8_t> buffer_;
    std::deque<KeyEvent> parsed_events_;
    std::vector<Subscriber> subscribers_;
    std::uint64_t next_sub_id_{1};
    Clock::time_point last_input_time_;
    std::chrono::milliseconds escape_timeout_{50};
    bool bracketed_paste_enabled_{true};
    bool in_escape_{false};
    bool in_bracketed_paste_{false};
    std::vector<std::uint8_t> paste_buffer_;

    // 尝试从缓冲区解析完整的按键事件
    auto try_parse() -> void {
        while (!buffer_.empty()) {
            // Bracketed paste 模式处理
            if (in_bracketed_paste_) {
                if (!consume_paste_data()) break;
                continue;
            }

            auto byte = buffer_.front();

            if (byte == 0x1B) { // ESC
                if (!try_parse_escape()) break; // 序列不完整，等待更多数据
            } else if (byte < 0x20) {
                // 控制字符
                emit_control_char(byte);
                buffer_.erase(buffer_.begin());
            } else {
                // 普通字符或 UTF-8 多字节
                if (!try_parse_utf8()) break;
            }
        }
    }

    // 解析 ESC 开头的转义序列
    [[nodiscard]] auto try_parse_escape() -> bool {
        if (buffer_.size() < 2) {
            in_escape_ = true;
            return false; // 等待更多数据或超时
        }

        auto second = buffer_[1];
        if (second == '[') {
            return try_parse_csi(); // CSI 序列: ESC [ ...
        } else if (second == ']') {
            return try_parse_osc(); // OSC 序列: ESC ] ...
        } else if (second == 'O') {
            return try_parse_ss3(); // SS3 序列: ESC O ...
        } else {
            // Alt+key 组合
            KeyEvent event{
                .key = std::string(1, static_cast<char>(second)),
                .modifiers = {.alt = true},
                .raw_bytes = {buffer_[0], buffer_[1]}
            };
            buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
            emit_event(std::move(event));
            return true;
        }
    }

    // 解析 CSI 序列 (ESC [ ...)
    [[nodiscard]] auto try_parse_csi() -> bool {
        // 查找终止字符 (0x40-0x7E)
        for (std::size_t i = 2; i < buffer_.size(); ++i) {
            auto b = buffer_[i];
            if (b >= 0x40 && b <= 0x7E) {
                // 检查是否是 bracketed paste 开始
                if (check_bracketed_paste_start(i)) return true;

                auto raw = std::vector<std::uint8_t>(buffer_.begin(), buffer_.begin() + i + 1);
                auto key_name = decode_csi_key(raw);
                auto mods = decode_csi_modifiers(raw);

                buffer_.erase(buffer_.begin(), buffer_.begin() + i + 1);
                emit_event(KeyEvent{
                    .key = std::move(key_name),
                    .modifiers = mods,
                    .raw_bytes = std::move(raw)
                });
                in_escape_ = false;
                return true;
            }
        }
        in_escape_ = true;
        return false; // 序列尚未完成
    }

    // 解析 OSC 序列 (ESC ] ...)
    [[nodiscard]] auto try_parse_osc() -> bool {
        // OSC 以 ST (ESC \) 或 BEL (0x07) 结束
        for (std::size_t i = 2; i < buffer_.size(); ++i) {
            if (buffer_[i] == 0x07 ||
                (buffer_[i] == '\\' && i > 0 && buffer_[i-1] == 0x1B)) {
                // 消耗整个 OSC 序列（通常不产生按键事件）
                buffer_.erase(buffer_.begin(), buffer_.begin() + i + 1);
                in_escape_ = false;
                return true;
            }
        }
        return false;
    }

    // 解析 SS3 序列 (ESC O ...)
    [[nodiscard]] auto try_parse_ss3() -> bool {
        if (buffer_.size() < 3) return false;
        auto code = buffer_[2];
        auto raw = std::vector<std::uint8_t>(buffer_.begin(), buffer_.begin() + 3);
        std::string key_name;

        switch (code) {
            case 'P': key_name = "F1"; break;
            case 'Q': key_name = "F2"; break;
            case 'R': key_name = "F3"; break;
            case 'S': key_name = "F4"; break;
            case 'H': key_name = "Home"; break;
            case 'F': key_name = "End"; break;
            default:  key_name = std::string("SS3_") + static_cast<char>(code); break;
        }

        buffer_.erase(buffer_.begin(), buffer_.begin() + 3);
        emit_event(KeyEvent{.key = std::move(key_name), .raw_bytes = std::move(raw)});
        in_escape_ = false;
        return true;
    }

    // 解析 UTF-8 多字节字符
    [[nodiscard]] auto try_parse_utf8() -> bool {
        auto byte = buffer_.front();
        std::size_t expected_len = 1;

        if ((byte & 0x80) == 0)         expected_len = 1;
        else if ((byte & 0xE0) == 0xC0) expected_len = 2;
        else if ((byte & 0xF0) == 0xE0) expected_len = 3;
        else if ((byte & 0xF8) == 0xF0) expected_len = 4;

        if (buffer_.size() < expected_len) return false;

        auto raw = std::vector<std::uint8_t>(buffer_.begin(), buffer_.begin() + expected_len);
        std::string key_str(raw.begin(), raw.end());

        buffer_.erase(buffer_.begin(), buffer_.begin() + expected_len);
        emit_event(KeyEvent{
            .key = std::move(key_str),
            .is_paste = in_bracketed_paste_,
            .raw_bytes = std::move(raw)
        });
        return true;
    }

    // 处理控制字符
    auto emit_control_char(std::uint8_t byte) -> void {
        KeyModifiers mods{.ctrl = true};
        std::string key_name;

        switch (byte) {
            case 0x0D: key_name = "Enter"; mods.ctrl = false; break;
            case 0x09: key_name = "Tab"; mods.ctrl = false; break;
            case 0x7F: key_name = "Backspace"; mods.ctrl = false; break;
            case 0x01: key_name = "a"; break; // Ctrl+A
            case 0x03: key_name = "c"; break; // Ctrl+C
            case 0x04: key_name = "d"; break; // Ctrl+D
            default:   key_name = std::string(1, static_cast<char>('a' + byte - 1)); break;
        }

        emit_event(KeyEvent{
            .key = std::move(key_name),
            .modifiers = mods,
            .raw_bytes = {byte}
        });
    }

    // 检查并处理 bracketed paste 开始标记
    [[nodiscard]] auto check_bracketed_paste_start(std::size_t end_idx) -> bool {
        // ESC[200~ 标记粘贴开始
        if (end_idx >= 5 && buffer_[2] == '2' && buffer_[3] == '0' &&
            buffer_[4] == '0' && buffer_[end_idx] == '~') {
            buffer_.erase(buffer_.begin(), buffer_.begin() + end_idx + 1);
            in_bracketed_paste_ = true;
            in_escape_ = false;
            return true;
        }
        return false;
    }

    // 消费 bracketed paste 数据直到结束标记
    [[nodiscard]] auto consume_paste_data() -> bool {
        // 搜索结束标记 ESC[201~
        static constexpr std::array<std::uint8_t, 6> end_marker = {0x1B, '[', '2', '0', '1', '~'};

        for (std::size_t i = 0; i + end_marker.size() <= buffer_.size(); ++i) {
            bool match = true;
            for (std::size_t j = 0; j < end_marker.size(); ++j) {
                if (buffer_[i + j] != end_marker[j]) { match = false; break; }
            }
            if (match) {
                // 将粘贴内容作为一个事件发出
                auto paste_data = std::vector<std::uint8_t>(buffer_.begin(), buffer_.begin() + i);
                buffer_.erase(buffer_.begin(), buffer_.begin() + i + end_marker.size());
                in_bracketed_paste_ = false;

                std::string paste_text(paste_data.begin(), paste_data.end());
                emit_event(KeyEvent{
                    .key = std::move(paste_text),
                    .is_paste = true,
                    .raw_bytes = std::move(paste_data)
                });
                return true;
            }
        }
        return false; // 结束标记尚未到达
    }

    // 检查 escape 超时
    auto check_escape_timeout() -> void {
        if (!in_escape_ || buffer_.empty()) return;
        auto elapsed = Clock::now() - last_input_time_;
        if (elapsed >= escape_timeout_ && buffer_.size() == 1 && buffer_[0] == 0x1B) {
            buffer_.clear();
            in_escape_ = false;
            emit_event(KeyEvent{.key = "Escape", .raw_bytes = {0x1B}});
        }
    }

    // CSI 序列终止字符到键名的映射
    [[nodiscard]] static auto decode_csi_key(std::span<const std::uint8_t> raw) -> std::string {
        auto terminator = raw.back();
        switch (terminator) {
            case 'A': return "ArrowUp";
            case 'B': return "ArrowDown";
            case 'C': return "ArrowRight";
            case 'D': return "ArrowLeft";
            case 'H': return "Home";
            case 'F': return "End";
            case '~': return decode_tilde_key(raw);
            case 'Z': return "ShiftTab";
            default:  return std::string("CSI_") + static_cast<char>(terminator);
        }
    }

    // 解析 CSI n ~ 类型的键
    [[nodiscard]] static auto decode_tilde_key(std::span<const std::uint8_t> raw) -> std::string {
        // 提取参数数字
        std::string params;
        for (std::size_t i = 2; i < raw.size() - 1; ++i) {
            if (raw[i] >= '0' && raw[i] <= '9') params += static_cast<char>(raw[i]);
            else break;
        }
        if (params == "2") return "Insert";
        if (params == "3") return "Delete";
        if (params == "5") return "PageUp";
        if (params == "6") return "PageDown";
        if (params == "15") return "F5";
        if (params == "17") return "F6";
        if (params == "18") return "F7";
        if (params == "19") return "F8";
        if (params == "20") return "F9";
        if (params == "21") return "F10";
        if (params == "23") return "F11";
        if (params == "24") return "F12";
        return "Unknown~" + params;
    }

    // 解析 CSI 修饰符参数
    [[nodiscard]] static auto decode_csi_modifiers(std::span<const std::uint8_t> raw) -> KeyModifiers {
        // 修饰符在分号后的数字中编码: CSI 1;modifier char
        KeyModifiers mods;
        for (std::size_t i = 2; i < raw.size(); ++i) {
            if (raw[i] == ';' && i + 1 < raw.size() - 1) {
                int mod_val = raw[i + 1] - '0';
                // modifier = 1 + (shift?1:0) + (alt?2:0) + (ctrl?4:0) + (meta?8:0)
                mod_val -= 1;
                mods.shift = (mod_val & 1) != 0;
                mods.alt   = (mod_val & 2) != 0;
                mods.ctrl  = (mod_val & 4) != 0;
                mods.meta  = (mod_val & 8) != 0;
                break;
            }
        }
        return mods;
    }

    // 发出解析完成的事件
    auto emit_event(KeyEvent event) -> void {
        for (const auto& sub : subscribers_) {
            sub.callback(event);
        }
        parsed_events_.push_back(std::move(event));
    }
};

} // namespace cc::hooks
