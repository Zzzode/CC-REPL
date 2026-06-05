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
#include <utility>
#include <vector>

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


class ComputerUseManager {
public:
    ComputerUseManager() = default;

    explicit ComputerUseManager(ScreenCapture screen)
        : screen_(std::move(screen)) {}

    [[nodiscard]] bool is_available() const {
        return screen_.is_available();
    }


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
                if (!action.position) return ActionResult::fail("Mouse position is required");
                return mouse_.move(action.position->x, action.position->y);
            case ActionType::MouseClick:
                if (!action.position) return ActionResult::fail("Mouse position is required");
                if (auto moved = mouse_.move(action.position->x, action.position->y); !moved.success) {
                    return moved;
                }
                return mouse_.click();
            case ActionType::MouseDoubleClick:
                if (!action.position) return ActionResult::fail("Mouse position is required");
                if (auto moved = mouse_.move(action.position->x, action.position->y); !moved.success) {
                    return moved;
                }
                return mouse_.double_click();
            case ActionType::MouseRightClick:
                if (!action.position) return ActionResult::fail("Mouse position is required");
                if (auto moved = mouse_.move(action.position->x, action.position->y); !moved.success) {
                    return moved;
                }
                return mouse_.right_click();
            case ActionType::MouseDrag:
                if (!action.position || !action.drag_end) {
                    return ActionResult::fail("Drag start and end positions are required");
                }
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


    [[nodiscard]] ScreenCapture& screen() { return screen_; }
    [[nodiscard]] MouseControl& mouse() { return mouse_; }
    [[nodiscard]] KeyboardControl& keyboard() { return keyboard_; }

private:
    ScreenCapture screen_;
    MouseControl mouse_;
    KeyboardControl keyboard_;
};

} // namespace cc::core::computer_use
