// C++23 Module: Computer use capabilities

module;
#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#endif

export module cc.tools.computer_use;

export namespace cc::core::computer_use {


struct ImageData {
    std::vector<uint8_t> pixels;
    uint32_t width{0};
    uint32_t height{0};
    std::string format{"rgba"};

    [[nodiscard]] bool is_valid() const {
        return width > 0 && height > 0 && !pixels.empty();
    }

    [[nodiscard]] size_t byte_size() const { return pixels.size(); }
};


struct Point {
    int32_t x{0};
    int32_t y{0};
};


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


enum class MouseButton : uint8_t {
    Left, Right, Middle
};


enum class Modifier : uint8_t {
    None    = 0,
    Shift   = 1 << 0,
    Ctrl    = 1 << 1,
    Alt     = 1 << 2,
    Meta    = 1 << 3   // Cmd on macOS, Win on Windows
};


struct ActionResult {
    bool success{false};
    std::string error_message;
    std::optional<ImageData> screenshot;

    [[nodiscard]] static ActionResult ok() { return {true, {}, {}}; }
    [[nodiscard]] static ActionResult fail(std::string msg) {
        return {false, std::move(msg), {}};
    }
};


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


struct ComputerAction {
    ActionType type;
    std::optional<Point> position;
    std::optional<Point> drag_end;
    std::optional<std::string> text;
    std::optional<Rect> region;
    std::vector<std::string> keys;

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

using CaptureProvider = std::function<std::expected<ImageData, std::string>(std::optional<Rect>)>;
using InputProvider = std::function<std::expected<void, std::string>(const ComputerAction&)>;

class ScreenCapture {
public:
    explicit ScreenCapture(CaptureProvider provider = {})
        : provider_(std::move(provider)) {}

    [[nodiscard]] std::expected<ImageData, std::string> capture_screen() const {
        return capture(std::nullopt);
    }


    [[nodiscard]] std::expected<ImageData, std::string> capture_region(
        int32_t x, int32_t y, uint32_t w, uint32_t h) const {
        if (!is_available()) {
            return std::unexpected("Screen capture not available");
        }
        if (w == 0 || h == 0) {
            return std::unexpected("Invalid capture region dimensions");
        }
        return capture(Rect{.x = x, .y = y, .width = w, .height = h});
    }

    void set_provider(CaptureProvider provider) {
        provider_ = std::move(provider);
    }

    [[nodiscard]] bool is_available() const {
        if (provider_) return true;
#ifdef __APPLE__
        return command_exists("screencapture");
#elif defined(__linux__)
        return check_display_server();
#else
        return false;
#endif
    }

private:
    CaptureProvider provider_;

    [[nodiscard]] std::expected<ImageData, std::string> capture(std::optional<Rect> region) const {
        if (provider_) {
            return provider_(region);
        }
        if (!is_available()) {
            return std::unexpected("Screen capture not available on this platform");
        }
        return platform_capture(region);
    }

    [[nodiscard]] static std::string shell_quote(std::string_view value) {
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    [[nodiscard]] static bool command_exists(std::string_view command) {
        auto cmd = std::format("command -v {} >/dev/null 2>&1", shell_quote(command));
        return std::system(cmd.c_str()) == 0;
    }

    [[nodiscard]] static std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    [[nodiscard]] static std::optional<std::pair<uint32_t, uint32_t>> parse_png_size(
        const std::vector<uint8_t>& bytes) {
        static constexpr std::array<uint8_t, 8> signature{
            0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (bytes.size() < 24 ||
            !std::equal(signature.begin(), signature.end(), bytes.begin())) {
            return std::nullopt;
        }
        auto read_be = [&](std::size_t offset) -> uint32_t {
            return (static_cast<uint32_t>(bytes[offset]) << 24) |
                   (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
                   static_cast<uint32_t>(bytes[offset + 3]);
        };
        return std::pair{read_be(16), read_be(20)};
    }

    [[nodiscard]] static std::expected<ImageData, std::string> platform_capture(std::optional<Rect> region) {
#ifdef __APPLE__
        auto path = std::filesystem::temp_directory_path() /
            std::format("cc-repl-computer-use-{}.png",
                std::chrono::steady_clock::now().time_since_epoch().count());
        std::string cmd = "screencapture -x -t png ";
        if (region) {
            cmd += std::format("-R {},{},{},{} ",
                region->x, region->y, region->width, region->height);
        }
        cmd += shell_quote(path.string());
        cmd += " >/dev/null 2>&1";
        auto status = std::system(cmd.c_str());
        if (status != 0 || !std::filesystem::exists(path)) {
            return std::unexpected("screencapture failed");
        }
        auto bytes = read_binary_file(path);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto size = parse_png_size(bytes);
        if (!size) {
            return std::unexpected("screencapture returned an invalid PNG image");
        }
        return ImageData{
            .pixels = std::move(bytes),
            .width = size->first,
            .height = size->second,
            .format = "png",
        };
#else
        (void)region;
        return std::unexpected("Screen capture is unavailable on this platform");
#endif
    }

    [[nodiscard]] bool check_display_server() const {
        return std::getenv("DISPLAY") != nullptr ||
               std::getenv("WAYLAND_DISPLAY") != nullptr;
    }
};


class MouseControl {
public:

    [[nodiscard]] ActionResult move(int32_t x, int32_t y) {
        if (!validate_position(x, y)) {
            return ActionResult::fail("Position out of screen bounds");
        }
        current_position_ = {x, y};

        return ActionResult::ok();
    }


    [[nodiscard]] ActionResult click(MouseButton button = MouseButton::Left) {
        (void)button;
        return ActionResult::ok();
    }


    [[nodiscard]] ActionResult double_click() {
        return ActionResult::ok();
    }


    [[nodiscard]] ActionResult right_click() {
        return click(MouseButton::Right);
    }


    [[nodiscard]] ActionResult drag(int32_t from_x, int32_t from_y,
                                     int32_t to_x, int32_t to_y) {
        if (!validate_position(from_x, from_y) || !validate_position(to_x, to_y)) {
            return ActionResult::fail("Drag position out of bounds");
        }
        current_position_ = {to_x, to_y};
        return ActionResult::ok();
    }


    [[nodiscard]] ActionResult scroll(int32_t delta_x, int32_t delta_y) {
        last_scroll_delta_ = {delta_x, delta_y};
        return ActionResult::ok();
    }

    [[nodiscard]] Point current_position() const { return current_position_; }
    [[nodiscard]] Point last_scroll_delta() const { return last_scroll_delta_; }

private:
    Point current_position_{0, 0};
    Point last_scroll_delta_{0, 0};
    Rect screen_bounds_{0, 0, 3840, 2160};

    [[nodiscard]] bool validate_position(int32_t x, int32_t y) const {
        return screen_bounds_.contains({x, y});
    }
};


class KeyboardControl {
public:

    [[nodiscard]] ActionResult type_text(std::string_view text) {
        if (text.empty()) {
            return ActionResult::fail("Empty text input");
        }

        return ActionResult::ok();
    }


    [[nodiscard]] ActionResult press_key(std::string_view key) {
        if (!is_valid_key(key)) {
            return ActionResult::fail(std::format("Unknown key: {}", key));
        }
        return ActionResult::ok();
    }


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
            "shift", "ctrl", "control", "alt", "option", "meta", "cmd", "command", "super"
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

namespace native_input_detail {

[[nodiscard]] inline std::string lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

[[nodiscard]] inline bool native_input_disabled() {
    const char* value = std::getenv("CC_REPL_DISABLE_NATIVE_COMPUTER_INPUT");
    if (!value) return false;
    std::string_view text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "on";
}

#ifdef __APPLE__
struct KeySpec {
    CGKeyCode code;
    CGEventFlags shift{0};
};

[[nodiscard]] inline std::optional<KeySpec> key_spec(std::string_view key) {
    const auto value = lower(key);
    if (value.size() == 1) {
        switch (value[0]) {
            case 'a': return KeySpec{0x00};
            case 's': return KeySpec{0x01};
            case 'd': return KeySpec{0x02};
            case 'f': return KeySpec{0x03};
            case 'h': return KeySpec{0x04};
            case 'g': return KeySpec{0x05};
            case 'z': return KeySpec{0x06};
            case 'x': return KeySpec{0x07};
            case 'c': return KeySpec{0x08};
            case 'v': return KeySpec{0x09};
            case 'b': return KeySpec{0x0B};
            case 'q': return KeySpec{0x0C};
            case 'w': return KeySpec{0x0D};
            case 'e': return KeySpec{0x0E};
            case 'r': return KeySpec{0x0F};
            case 'y': return KeySpec{0x10};
            case 't': return KeySpec{0x11};
            case '1': return KeySpec{0x12};
            case '2': return KeySpec{0x13};
            case '3': return KeySpec{0x14};
            case '4': return KeySpec{0x15};
            case '6': return KeySpec{0x16};
            case '5': return KeySpec{0x17};
            case '=': return KeySpec{0x18};
            case '9': return KeySpec{0x19};
            case '7': return KeySpec{0x1A};
            case '-': return KeySpec{0x1B};
            case '8': return KeySpec{0x1C};
            case '0': return KeySpec{0x1D};
            case ']': return KeySpec{0x1E};
            case 'o': return KeySpec{0x1F};
            case 'u': return KeySpec{0x20};
            case '[': return KeySpec{0x21};
            case 'i': return KeySpec{0x22};
            case 'p': return KeySpec{0x23};
            case 'l': return KeySpec{0x25};
            case 'j': return KeySpec{0x26};
            case '\'': return KeySpec{0x27};
            case 'k': return KeySpec{0x28};
            case ';': return KeySpec{0x29};
            case '\\': return KeySpec{0x2A};
            case ',': return KeySpec{0x2B};
            case '/': return KeySpec{0x2C};
            case 'n': return KeySpec{0x2D};
            case 'm': return KeySpec{0x2E};
            case '.': return KeySpec{0x2F};
            case ' ': return KeySpec{0x31};
            case '`': return KeySpec{0x32};
            default: return std::nullopt;
        }
    }
    if (value == "return" || value == "enter") return KeySpec{0x24};
    if (value == "tab") return KeySpec{0x30};
    if (value == "space") return KeySpec{0x31};
    if (value == "backspace") return KeySpec{0x33};
    if (value == "escape") return KeySpec{0x35};
    if (value == "delete") return KeySpec{0x75};
    if (value == "home") return KeySpec{0x73};
    if (value == "pageup") return KeySpec{0x74};
    if (value == "end") return KeySpec{0x77};
    if (value == "pagedown") return KeySpec{0x79};
    if (value == "left") return KeySpec{0x7B};
    if (value == "right") return KeySpec{0x7C};
    if (value == "down") return KeySpec{0x7D};
    if (value == "up") return KeySpec{0x7E};
    if (value == "f1") return KeySpec{0x7A};
    if (value == "f2") return KeySpec{0x78};
    if (value == "f3") return KeySpec{0x63};
    if (value == "f4") return KeySpec{0x76};
    if (value == "f5") return KeySpec{0x60};
    if (value == "f6") return KeySpec{0x61};
    if (value == "f7") return KeySpec{0x62};
    if (value == "f8") return KeySpec{0x64};
    if (value == "f9") return KeySpec{0x65};
    if (value == "f10") return KeySpec{0x6D};
    if (value == "f11") return KeySpec{0x67};
    if (value == "f12") return KeySpec{0x6F};
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::pair<CGKeyCode, CGEventFlags>> modifier_spec(std::string_view key) {
    const auto value = lower(key);
    if (value == "shift") return std::pair{static_cast<CGKeyCode>(0x38), static_cast<CGEventFlags>(kCGEventFlagMaskShift)};
    if (value == "ctrl" || value == "control") return std::pair{static_cast<CGKeyCode>(0x3B), static_cast<CGEventFlags>(kCGEventFlagMaskControl)};
    if (value == "alt" || value == "option") return std::pair{static_cast<CGKeyCode>(0x3A), static_cast<CGEventFlags>(kCGEventFlagMaskAlternate)};
    if (value == "meta" || value == "cmd" || value == "command" || value == "super") {
        return std::pair{static_cast<CGKeyCode>(0x37), static_cast<CGEventFlags>(kCGEventFlagMaskCommand)};
    }
    return std::nullopt;
}

inline void post_key(CGKeyCode code, bool down, CGEventFlags flags = 0) {
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, code, down);
    if (!event) return;
    CGEventSetFlags(event, flags);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

inline std::expected<void, std::string> post_key_press(std::string_view key, CGEventFlags flags = 0) {
    auto spec = key_spec(key);
    if (!spec) return std::unexpected(std::format("Unsupported key for native input: {}", key));
    const auto combined_flags = flags | spec->shift;
    post_key(spec->code, true, combined_flags);
    post_key(spec->code, false, combined_flags);
    return {};
}

[[nodiscard]] inline std::expected<std::vector<UniChar>, std::string> utf8_to_utf16(std::string_view text) {
    std::vector<UniChar> out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if ((c & 0x80) == 0) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            return std::unexpected("Invalid UTF-8 input");
        }
        if (i + len > text.size()) return std::unexpected("Invalid UTF-8 input");
        for (std::size_t j = 1; j < len; ++j) {
            const auto cc = static_cast<unsigned char>(text[i + j]);
            if ((cc & 0xC0) != 0x80) return std::unexpected("Invalid UTF-8 input");
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<UniChar>(cp));
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<UniChar>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<UniChar>(0xDC00 + (cp & 0x3FF)));
        } else {
            return std::unexpected("Invalid Unicode code point");
        }
        i += len;
    }
    return out;
}

inline std::expected<void, std::string> type_text(std::string_view text) {
    auto utf16 = utf8_to_utf16(text);
    if (!utf16) return std::unexpected(utf16.error());
    if (utf16->empty()) return std::unexpected("Empty text input");
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return std::unexpected("Failed to create native keyboard events");
    }
    CGEventKeyboardSetUnicodeString(down, utf16->size(), utf16->data());
    CGEventKeyboardSetUnicodeString(up, utf16->size(), utf16->data());
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
    return {};
}

inline void post_mouse(CGEventType type, CGPoint point, CGMouseButton button, std::int64_t click_state = 1) {
    CGEventRef event = CGEventCreateMouseEvent(nullptr, type, point, button);
    if (!event) return;
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_state);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

[[nodiscard]] inline CGPoint point_to_cg(Point point) {
    CGPoint out{};
    out.x = static_cast<CGFloat>(point.x);
    out.y = static_cast<CGFloat>(point.y);
    return out;
}

inline std::expected<void, std::string> move_mouse(Point point) {
    post_mouse(kCGEventMouseMoved, point_to_cg(point), kCGMouseButtonLeft);
    return {};
}

inline std::expected<void, std::string> click_mouse(Point point, CGMouseButton button, std::int64_t count = 1) {
    const auto down = button == kCGMouseButtonRight ? kCGEventRightMouseDown
                    : button == kCGMouseButtonCenter ? kCGEventOtherMouseDown
                    : kCGEventLeftMouseDown;
    const auto up = button == kCGMouseButtonRight ? kCGEventRightMouseUp
                  : button == kCGMouseButtonCenter ? kCGEventOtherMouseUp
                  : kCGEventLeftMouseUp;
    for (std::int64_t i = 1; i <= count; ++i) {
        post_mouse(down, point_to_cg(point), button, i);
        post_mouse(up, point_to_cg(point), button, i);
    }
    return {};
}

inline std::expected<void, std::string> drag_mouse(Point from, Point to) {
    auto start = point_to_cg(from);
    auto end = point_to_cg(to);
    post_mouse(kCGEventMouseMoved, start, kCGMouseButtonLeft);
    post_mouse(kCGEventLeftMouseDown, start, kCGMouseButtonLeft);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    post_mouse(kCGEventLeftMouseDragged, end, kCGMouseButtonLeft);
    post_mouse(kCGEventLeftMouseUp, end, kCGMouseButtonLeft);
    return {};
}

inline std::expected<void, std::string> scroll_mouse(Point delta) {
    CGEventRef event = CGEventCreateScrollWheelEvent(
        nullptr,
        kCGScrollEventUnitPixel,
        2,
        delta.y,
        delta.x);
    if (!event) return std::unexpected("Failed to create native scroll event");
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return {};
}

inline std::expected<void, std::string> post_hotkey(const std::vector<std::string>& keys) {
    if (keys.empty()) return std::unexpected("Empty hotkey combination");
    std::vector<std::pair<CGKeyCode, CGEventFlags>> modifiers;
    std::optional<std::string> final_key;
    CGEventFlags flags = 0;
    for (const auto& key : keys) {
        if (auto mod = modifier_spec(key)) {
            modifiers.push_back(*mod);
            flags |= mod->second;
        } else {
            final_key = key;
        }
    }
    if (!final_key) return std::unexpected("Hotkey requires a non-modifier key");
    for (const auto& [code, flag] : modifiers) {
        post_key(code, true, flags | flag);
    }
    auto result = post_key_press(*final_key, flags);
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
        post_key(it->first, false, flags & ~it->second);
    }
    return result;
}

inline std::expected<void, std::string> require_accessibility_permission() {
    if (AXIsProcessTrusted()) return {};
    return std::unexpected(
        "macOS Accessibility permission is required for native computer input");
}

inline std::expected<void, std::string> dispatch_native(const ComputerAction& action) {
    if (auto permission = require_accessibility_permission(); !permission) {
        return std::unexpected(permission.error());
    }
    switch (action.type) {
        case ActionType::MouseMove:
            if (!action.position) return std::unexpected("Mouse position is required");
            return move_mouse(*action.position);
        case ActionType::MouseClick:
            if (!action.position) return std::unexpected("Mouse position is required");
            return click_mouse(*action.position, kCGMouseButtonLeft);
        case ActionType::MouseDoubleClick:
            if (!action.position) return std::unexpected("Mouse position is required");
            return click_mouse(*action.position, kCGMouseButtonLeft, 2);
        case ActionType::MouseRightClick:
            if (!action.position) return std::unexpected("Mouse position is required");
            return click_mouse(*action.position, kCGMouseButtonRight);
        case ActionType::MouseDrag:
            if (!action.position || !action.drag_end) return std::unexpected("Drag start and end positions are required");
            return drag_mouse(*action.position, *action.drag_end);
        case ActionType::KeyType:
            return type_text(action.text.value_or(""));
        case ActionType::KeyPress:
            return post_key_press(action.text.value_or(""));
        case ActionType::KeyHotkey:
            return post_hotkey(action.keys);
        case ActionType::Scroll:
            if (!action.position) return std::unexpected("Scroll delta is required");
            return scroll_mouse(*action.position);
        case ActionType::Screenshot:
            return {};
    }
    return std::unexpected("Unsupported native input action");
}
#endif

} // namespace native_input_detail

[[nodiscard]] inline InputProvider make_native_input_provider() {
    if (native_input_detail::native_input_disabled()) return {};
#ifdef __APPLE__
    return [](const ComputerAction& action) -> std::expected<void, std::string> {
        return native_input_detail::dispatch_native(action);
    };
#else
    return {};
#endif
}


class ComputerUseManager {
public:
    ComputerUseManager() = default;

    explicit ComputerUseManager(ScreenCapture screen)
        : screen_(std::move(screen)) {}

    ComputerUseManager(ScreenCapture screen, InputProvider input)
        : screen_(std::move(screen)), input_(std::move(input)) {}

    [[nodiscard]] bool is_available() const {
        return screen_.is_available() || static_cast<bool>(input_);
    }


    [[nodiscard]] ActionResult execute_action(const ComputerAction& action) {
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
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (!action.position) return ActionResult::fail("Mouse position is required");
                if (auto validated = mouse_.move(action.position->x, action.position->y); !validated.success) {
                    return validated;
                }
                return dispatch_input(action);
            case ActionType::MouseClick:
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (!action.position) return ActionResult::fail("Mouse position is required");
                if (auto moved = mouse_.move(action.position->x, action.position->y); !moved.success) {
                    return moved;
                }
                if (auto clicked = mouse_.click(); !clicked.success) return clicked;
                return dispatch_input(action);
            case ActionType::MouseDoubleClick:
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (!action.position) return ActionResult::fail("Mouse position is required");
                if (auto moved = mouse_.move(action.position->x, action.position->y); !moved.success) {
                    return moved;
                }
                if (auto clicked = mouse_.double_click(); !clicked.success) return clicked;
                return dispatch_input(action);
            case ActionType::MouseRightClick:
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (!action.position) return ActionResult::fail("Mouse position is required");
                if (auto moved = mouse_.move(action.position->x, action.position->y); !moved.success) {
                    return moved;
                }
                if (auto clicked = mouse_.right_click(); !clicked.success) return clicked;
                return dispatch_input(action);
            case ActionType::MouseDrag:
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (!action.position || !action.drag_end) {
                    return ActionResult::fail("Drag start and end positions are required");
                }
                if (auto dragged = mouse_.drag(action.position->x, action.position->y,
                                               action.drag_end->x, action.drag_end->y); !dragged.success) {
                    return dragged;
                }
                return dispatch_input(action);
            case ActionType::KeyType: {
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (auto typed = keyboard_.type_text(action.text.value_or("")); !typed.success) {
                    return typed;
                }
                return dispatch_input(action);
            }
            case ActionType::KeyPress: {
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (auto pressed = keyboard_.press_key(action.text.value_or("")); !pressed.success) {
                    return pressed;
                }
                return dispatch_input(action);
            }
            case ActionType::KeyHotkey: {
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (auto combo = keyboard_.hotkey(action.keys); !combo.success) {
                    return combo;
                }
                return dispatch_input(action);
            }
            case ActionType::Scroll:
                if (!input_) return ActionResult::fail("Computer input control not available");
                if (!action.position) return ActionResult::fail("Scroll delta is required");
                if (auto scrolled = mouse_.scroll(action.position->x, action.position->y); !scrolled.success) {
                    return scrolled;
                }
                return dispatch_input(action);
        }
        return ActionResult::fail("Unknown action type");
    }

    void set_input_provider(InputProvider input) {
        input_ = std::move(input);
    }

    [[nodiscard]] ScreenCapture& screen() { return screen_; }
    [[nodiscard]] MouseControl& mouse() { return mouse_; }
    [[nodiscard]] KeyboardControl& keyboard() { return keyboard_; }

private:
    [[nodiscard]] ActionResult dispatch_input(const ComputerAction& action) {
        if (!input_) return ActionResult::fail("Computer input control not available");
        auto result = input_(action);
        if (!result) return ActionResult::fail(result.error());
        return ActionResult::ok();
    }

    ScreenCapture screen_;
    MouseControl mouse_;
    KeyboardControl keyboard_;
    InputProvider input_;
};

} // namespace cc::core::computer_use
