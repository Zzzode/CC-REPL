// C++23 Module: Computer use capabilities
// 计算机操控能力：屏幕截图、鼠标控制、键盘控制
module;
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.computer_use;

export namespace cc::core::computer_use {

// 图像数据结构
struct ImageData {
    std::vector<uint8_t> pixels;  // 原始像素数据 (RGBA)
    uint32_t width{0};
    uint32_t height{0};
    std::string format{"rgba"};   // 像素格式

    [[nodiscard]] bool is_valid() const {
        return width > 0 && height > 0 && !pixels.empty();
    }

    [[nodiscard]] size_t byte_size() const { return pixels.size(); }
};

// 屏幕坐标
struct Point {
    int32_t x{0};
    int32_t y{0};
};

// 矩形区域
struct Rect {
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};

    [[nodiscard]] bool contains(Point p) const {
        return p.x >= x && p.x < x + static_cast<int32_t>(width) &&
               p.y >= y && p.y < y + static_cast<int32_t>(height);
    }
};

// 鼠标按钮
enum class MouseButton : uint8_t {
    Left, Right, Middle
};

// 键盘修饰键
enum class Modifier : uint8_t {
    None    = 0,
    Shift   = 1 << 0,
    Ctrl    = 1 << 1,
    Alt     = 1 << 2,
    Meta    = 1 << 3   // Cmd on macOS, Win on Windows
};

// 操作结果
struct ActionResult {
    bool success{false};
    std::string error_message;
    std::optional<ImageData> screenshot;  // 操作后截图 (可选)

    [[nodiscard]] static ActionResult ok() { return {true, {}, {}}; }
    [[nodiscard]] static ActionResult fail(std::string msg) {
        return {false, std::move(msg), {}};
    }
};

// 计算机操控动作
enum class ActionType : uint8_t {
    Screenshot,
    MouseMove,
    MouseClick,
    MouseDoubleClick,
    MouseRightClick,
    MouseDrag,
    KeyType,
    KeyPress,
    KeyHotkey,
    Scroll
};

// 抽象动作描述
struct ComputerAction {
    ActionType type;
    std::optional<Point> position;       // 鼠标位置
    std::optional<Point> drag_end;       // 拖拽终点
    std::optional<std::string> text;     // 输入文本或按键名
    std::optional<Rect> region;          // 截屏区域
    std::vector<std::string> keys;       // 组合键列表

    [[nodiscard]] std::string describe() const {
        switch (type) {
            case ActionType::Screenshot:
                return region ? std::format("screenshot region({},{} {}x{})",
                    region->x, region->y, region->width, region->height) : "screenshot";
            case ActionType::MouseMove:
                return std::format("move({}, {})", position->x, position->y);
            case ActionType::MouseClick:
                return std::format("click({}, {})", position->x, position->y);
            case ActionType::MouseDoubleClick:
                return std::format("double_click({}, {})", position->x, position->y);
            case ActionType::MouseRightClick:
                return std::format("right_click({}, {})", position->x, position->y);
            case ActionType::MouseDrag:
                return std::format("drag({},{} -> {},{})",
                    position->x, position->y, drag_end->x, drag_end->y);
            case ActionType::KeyType:
                return std::format("type(\"{}\")", text.value_or(""));
            case ActionType::KeyPress:
                return std::format("press({})", text.value_or(""));
            case ActionType::KeyHotkey: {
                std::string combo;
                for (size_t i = 0; i < keys.size(); ++i) {
                    if (i > 0) combo += "+";
                    combo += keys[i];
                }
                return std::format("hotkey({})", combo);
            }
            case ActionType::Scroll:
                return std::format("scroll({}, {})", position->x, position->y);
        }
        return "unknown";
    }
};

// 屏幕截图控制类
class ScreenCapture {
public:
    // 截取全屏
    [[nodiscard]] std::expected<ImageData, std::string> capture_screen() const {
        if (!is_available()) {
            return std::unexpected("Screen capture not available on this platform");
        }
        // 平台特定实现 (macOS: CGDisplayCreateImage, Linux: X11/Wayland)
        return std::unexpected("Screen capture: platform implementation required");
    }

    // 截取指定区域
    [[nodiscard]] std::expected<ImageData, std::string> capture_region(
        int32_t x, int32_t y, uint32_t w, uint32_t h) const {
        if (!is_available()) {
            return std::unexpected("Screen capture not available");
        }
        if (w == 0 || h == 0) {
            return std::unexpected("Invalid capture region dimensions");
        }
        return std::unexpected("Region capture: platform implementation required");
    }

    // 检查截图能力是否可用
    [[nodiscard]] bool is_available() const {
#ifdef __APPLE__
        return true;  // macOS 始终可用 (需权限)
#elif defined(__linux__)
        return check_display_server();
#else
        return false;
#endif
    }

private:
    [[nodiscard]] bool check_display_server() const {
        return std::getenv("DISPLAY") != nullptr ||
               std::getenv("WAYLAND_DISPLAY") != nullptr;
    }
};

// 鼠标控制类
class MouseControl {
public:
    // 移动鼠标到指定位置
    [[nodiscard]] ActionResult move(int32_t x, int32_t y) {
        if (!validate_position(x, y)) {
            return ActionResult::fail("Position out of screen bounds");
        }
        current_position_ = {x, y};
        // 平台特定实现
        return ActionResult::ok();
    }

    // 点击
    [[nodiscard]] ActionResult click(MouseButton button = MouseButton::Left) {
        return ActionResult::ok();
    }

    // 双击
    [[nodiscard]] ActionResult double_click() {
        return ActionResult::ok();
    }

    // 右键点击
    [[nodiscard]] ActionResult right_click() {
        return click(MouseButton::Right);
    }

    // 拖拽
    [[nodiscard]] ActionResult drag(int32_t from_x, int32_t from_y,
                                     int32_t to_x, int32_t to_y) {
        if (!validate_position(from_x, from_y) || !validate_position(to_x, to_y)) {
            return ActionResult::fail("Drag position out of bounds");
        }
        current_position_ = {to_x, to_y};
        return ActionResult::ok();
    }

    // 滚动（x/y 表示水平/垂直滚动增量）
    [[nodiscard]] ActionResult scroll(int32_t delta_x, int32_t delta_y) {
        last_scroll_delta_ = {delta_x, delta_y};
        return ActionResult::ok();
    }

    [[nodiscard]] Point current_position() const { return current_position_; }
    [[nodiscard]] Point last_scroll_delta() const { return last_scroll_delta_; }

private:
    Point current_position_{0, 0};
    Point last_scroll_delta_{0, 0};
    Rect screen_bounds_{0, 0, 3840, 2160};  // 默认 4K 边界

    [[nodiscard]] bool validate_position(int32_t x, int32_t y) const {
        return screen_bounds_.contains({x, y});
    }
};

// 键盘控制类
class KeyboardControl {
public:
    // 输入文本
    [[nodiscard]] ActionResult type_text(std::string_view text) {
        if (text.empty()) {
            return ActionResult::fail("Empty text input");
        }
        // 平台特定实现
        return ActionResult::ok();
    }

    // 按下单个键
    [[nodiscard]] ActionResult press_key(std::string_view key) {
        if (!is_valid_key(key)) {
            return ActionResult::fail(std::format("Unknown key: {}", key));
        }
        return ActionResult::ok();
    }

    // 组合键
    [[nodiscard]] ActionResult hotkey(const std::vector<std::string>& keys) {
        if (keys.empty()) {
            return ActionResult::fail("Empty hotkey combination");
        }
        for (const auto& key : keys) {
            if (!is_valid_key(key)) {
                return ActionResult::fail(std::format("Unknown key in combo: {}", key));
            }
        }
        return ActionResult::ok();
    }

private:
    [[nodiscard]] bool is_valid_key(std::string_view key) const {
        static const std::vector<std::string_view> valid_keys = {
            "enter", "return", "tab", "space", "backspace", "delete",
            "escape", "up", "down", "left", "right", "home", "end",
            "pageup", "pagedown", "f1", "f2", "f3", "f4", "f5", "f6",
            "f7", "f8", "f9", "f10", "f11", "f12",
            "shift", "ctrl", "alt", "meta", "cmd", "super"
        };
        auto lower = to_lower(key);
        return std::ranges::any_of(valid_keys, [&](auto k) { return k == lower; }) ||
               (key.size() == 1 && std::isprint(static_cast<unsigned char>(key[0])));
    }

    [[nodiscard]] static std::string to_lower(std::string_view sv) {
        std::string result(sv);
        std::ranges::transform(result, result.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        return result;
    }
};

// 计算机操控管理器：统一入口
class ComputerUseManager {
public:
    // 检查计算机操控功能是否可用
    [[nodiscard]] bool is_available() const {
        return screen_.is_available();
    }

    // 执行动作
    [[nodiscard]] ActionResult execute_action(const ComputerAction& action) {
        if (!is_available() && action.type != ActionType::Screenshot) {
            return ActionResult::fail("Computer use not available");
        }

        switch (action.type) {
            case ActionType::Screenshot:
                if (action.region) {
                    auto r = screen_.capture_region(
                        action.region->x, action.region->y,
                        action.region->width, action.region->height);
                    if (r) return {true, {}, std::move(*r)};
                    return ActionResult::fail(r.error());
                } else {
                    auto r = screen_.capture_screen();
                    if (r) return {true, {}, std::move(*r)};
                    return ActionResult::fail(r.error());
                }
            case ActionType::MouseMove:
                return mouse_.move(action.position->x, action.position->y);
            case ActionType::MouseClick:
                mouse_.move(action.position->x, action.position->y);
                return mouse_.click();
            case ActionType::MouseDoubleClick:
                mouse_.move(action.position->x, action.position->y);
                return mouse_.double_click();
            case ActionType::MouseRightClick:
                mouse_.move(action.position->x, action.position->y);
                return mouse_.right_click();
            case ActionType::MouseDrag:
                return mouse_.drag(action.position->x, action.position->y,
                                   action.drag_end->x, action.drag_end->y);
            case ActionType::KeyType:
                return keyboard_.type_text(action.text.value_or(""));
            case ActionType::KeyPress:
                return keyboard_.press_key(action.text.value_or(""));
            case ActionType::KeyHotkey:
                return keyboard_.hotkey(action.keys);
            case ActionType::Scroll:
                if (!action.position) return ActionResult::fail("Scroll delta is required");
                return mouse_.scroll(action.position->x, action.position->y);
        }
        return ActionResult::fail("Unknown action type");
    }

    // 获取各子系统引用
    [[nodiscard]] ScreenCapture& screen() { return screen_; }
    [[nodiscard]] MouseControl& mouse() { return mouse_; }
    [[nodiscard]] KeyboardControl& keyboard() { return keyboard_; }

private:
    ScreenCapture screen_;
    MouseControl mouse_;
    KeyboardControl keyboard_;
};

} // namespace cc::core::computer_use
