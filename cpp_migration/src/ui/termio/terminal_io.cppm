// cc.ui.terminal_io - ANSI terminal escape sequence parsing and generation
// Migrated from: src/ink/termio/ (ansi.ts, csi.ts, dec.ts, esc.ts, osc.ts,
//                                  parser.ts, sgr.ts, tokenize.ts, types.ts)
//
// Provides parsing of raw terminal input (escape sequences, CSI codes, SGR
// attributes), tokenization of ANSI-colored text, and generation of escape
// sequences for custom terminal operations. Rendering is handled by FTXUI.

module;

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.ui.terminal_io;

export namespace cc::ui::termio {

// ============================================================================
// Enums
// ============================================================================

/// Standard 16-color palette
enum class Color16 : std::uint8_t {
    Black = 0,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
    BrightBlack,
    BrightRed,
    BrightGreen,
    BrightYellow,
    BrightBlue,
    BrightMagenta,
    BrightCyan,
    BrightWhite,
};

/// 256-color palette index
struct Color256 {
    std::uint8_t index{0};
};

/// 24-bit true color
struct TrueColor {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
};

/// A color value: either 16-color, 256-color, true color, or default
using ColorValue = std::variant<std::monostate, Color16, Color256, TrueColor>;

/// Keyboard modifier flags (bitfield)
enum class KeyModifier : std::uint8_t {
    None  = 0,
    Shift = 1 << 0,
    Alt   = 1 << 1,
    Ctrl  = 1 << 2,
    Meta  = 1 << 3,
};

[[nodiscard]] inline constexpr KeyModifier operator|(KeyModifier a, KeyModifier b) noexcept {
    return static_cast<KeyModifier>(
        static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] inline constexpr KeyModifier operator&(KeyModifier a, KeyModifier b) noexcept {
    return static_cast<KeyModifier>(
        static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

inline constexpr KeyModifier& operator|=(KeyModifier& a, KeyModifier b) noexcept {
    a = a | b;
    return a;
}

/// Special (non-printable) keys
enum class SpecialKey : std::uint8_t {
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
    Delete,
    Tab,
    Enter,
    Escape,
    Backspace,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
};

// ============================================================================
// Types
// ============================================================================

/// SGR (Select Graphic Rendition) text attributes
struct SgrAttr {
    ColorValue fg{};
    ColorValue bg{};
    bool bold{false};
    bool italic{false};
    bool underline{false};
    bool dim{false};
    bool strikethrough{false};
    bool inverse{false};
};

/// CSI (Control Sequence Introducer) sequence
struct CsiSequence {
    std::vector<int> params;
    std::string intermediate;
    char final_byte{'\0'};
};

/// OSC (Operating System Command) sequence
struct OscSequence {
    int command{0};
    std::string data;
};

/// ANSI token types for tokenization
struct TextToken {
    std::string_view content;
};

struct EscapeToken {
    std::string_view sequence;
};

struct CsiToken {
    CsiSequence csi;
};

struct OscToken {
    OscSequence osc;
};

struct SgrToken {
    SgrAttr attr;
};

/// Variant representing any ANSI token
using AnsiToken = std::variant<TextToken, EscapeToken, CsiToken, OscToken, SgrToken>;

/// Parsed input event types
struct KeypressEvent {
    std::variant<char32_t, SpecialKey> key;
    KeyModifier modifiers{KeyModifier::None};
};

struct MouseEvent {
    int x{0};
    int y{0};
    int button{0};
    KeyModifier modifiers{KeyModifier::None};
    bool pressed{false};
    bool released{false};
    bool motion{false};
};

struct PasteEvent {
    std::string content;
    bool start{false};
    bool end{false};
};

struct ResizeEvent {
    int cols{0};
    int rows{0};
};

struct FocusEvent {
    bool focused{false};
};

/// Variant representing parsed terminal input
using ParsedInput = std::variant<KeypressEvent, MouseEvent, PasteEvent, ResizeEvent, FocusEvent>;

// ============================================================================
// Parsing Functions
// ============================================================================

/// Parse SGR parameters from an escape sequence body (e.g. "1;31" from "\033[1;31m")
[[nodiscard]] inline auto parse_sgr(std::string_view params)
    -> std::expected<SgrAttr, std::string> {
    SgrAttr attr{};
    if (params.empty()) return attr; // Reset

    // Split on ';' and process each parameter
    std::size_t i = 0;
    auto next_int = [&]() -> int {
        int val = 0;
        while (i < params.size() && params[i] >= '0' && params[i] <= '9') {
            val = val * 10 + (params[i] - '0');
            i++;
        }
        if (i < params.size() && params[i] == ';') i++;
        return val;
    };

    while (i < params.size()) {
        int code = next_int();
        switch (code) {
            case 0: attr = SgrAttr{}; break;
            case 1: attr.bold = true; break;
            case 2: attr.dim = true; break;
            case 3: attr.italic = true; break;
            case 4: attr.underline = true; break;
            case 7: attr.inverse = true; break;
            case 9: attr.strikethrough = true; break;
            case 22: attr.bold = false; attr.dim = false; break;
            case 23: attr.italic = false; break;
            case 24: attr.underline = false; break;
            case 27: attr.inverse = false; break;
            case 29: attr.strikethrough = false; break;
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                attr.fg = static_cast<Color16>(code - 30); break;
            case 38: { // Extended foreground
                int mode = next_int();
                if (mode == 5) { attr.fg = Color256{static_cast<std::uint8_t>(next_int())}; }
                else if (mode == 2) {
                    auto r = static_cast<std::uint8_t>(next_int());
                    auto g = static_cast<std::uint8_t>(next_int());
                    auto b = static_cast<std::uint8_t>(next_int());
                    attr.fg = TrueColor{r, g, b};
                }
                break;
            }
            case 39: attr.fg = std::monostate{}; break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                attr.bg = static_cast<Color16>(code - 40); break;
            case 48: { // Extended background
                int mode = next_int();
                if (mode == 5) { attr.bg = Color256{static_cast<std::uint8_t>(next_int())}; }
                else if (mode == 2) {
                    auto r = static_cast<std::uint8_t>(next_int());
                    auto g = static_cast<std::uint8_t>(next_int());
                    auto b = static_cast<std::uint8_t>(next_int());
                    attr.bg = TrueColor{r, g, b};
                }
                break;
            }
            case 49: attr.bg = std::monostate{}; break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                attr.fg = static_cast<Color16>(code - 90 + 8); break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                attr.bg = static_cast<Color16>(code - 100 + 8); break;
            default: break;
        }
    }
    return attr;
}

/// Tokenize a string containing ANSI escape sequences into structured tokens
[[nodiscard]] inline auto tokenize_ansi(std::string_view input)
    -> std::vector<AnsiToken> {
    std::vector<AnsiToken> tokens;
    std::size_t i = 0;
    std::size_t text_start = 0;

    while (i < input.size()) {
        if (input[i] == '\033' && i + 1 < input.size()) {
            // Flush preceding text
            if (i > text_start) {
                tokens.push_back(TextToken{input.substr(text_start, i - text_start)});
            }

            if (input[i + 1] == '[') {
                // CSI sequence
                std::size_t seq_start = i;
                i += 2; // skip ESC[
                // Collect params and intermediate bytes
                std::string params_str;
                while (i < input.size() && input[i] >= 0x20 && input[i] <= 0x3F) {
                    params_str += input[i];
                    i++;
                }
                char final_byte = (i < input.size()) ? input[i] : '\0';
                if (i < input.size()) i++;

                if (final_byte == 'm') {
                    // SGR sequence
                    auto sgr_result = parse_sgr(params_str);
                    if (sgr_result) {
                        tokens.push_back(SgrToken{*sgr_result});
                    } else {
                        tokens.push_back(EscapeToken{input.substr(seq_start, i - seq_start)});
                    }
                } else {
                    CsiSequence csi;
                    csi.final_byte = final_byte;
                    // Parse params as semicolon-separated ints
                    std::size_t p = 0;
                    while (p < params_str.size()) {
                        int val = 0;
                        while (p < params_str.size() && params_str[p] >= '0' && params_str[p] <= '9') {
                            val = val * 10 + (params_str[p] - '0');
                            p++;
                        }
                        csi.params.push_back(val);
                        if (p < params_str.size() && params_str[p] == ';') p++;
                    }
                    tokens.push_back(CsiToken{std::move(csi)});
                }
            } else if (input[i + 1] == ']') {
                // OSC sequence
                i += 2;
                std::string osc_data;
                while (i < input.size() && input[i] != '\a' &&
                       !(i + 1 < input.size() && input[i] == '\033' && input[i + 1] == '\\')) {
                    osc_data += input[i];
                    i++;
                }
                if (i < input.size() && input[i] == '\a') i++;
                else if (i + 1 < input.size()) i += 2; // ESC backslash

                OscSequence osc;
                auto semi = osc_data.find(';');
                if (semi != std::string::npos) {
                    osc.command = std::atoi(osc_data.substr(0, semi).c_str());
                    osc.data = osc_data.substr(semi + 1);
                } else {
                    osc.data = std::move(osc_data);
                }
                tokens.push_back(OscToken{std::move(osc)});
            } else {
                // Other escape sequence
                std::size_t seq_start = i;
                i += 2;
                tokens.push_back(EscapeToken{input.substr(seq_start, i - seq_start)});
            }
            text_start = i;
        } else {
            i++;
        }
    }

    // Flush trailing text
    if (text_start < input.size()) {
        tokens.push_back(TextToken{input.substr(text_start)});
    }

    return tokens;
}

/// Strip all ANSI escape sequences, returning plain text
[[nodiscard]] inline auto strip_ansi(std::string_view input)
    -> std::string {
    std::string result;
    result.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '\033' && i + 1 < input.size()) {
            if (input[i + 1] == '[') {
                i += 2;
                // Skip until final byte (0x40-0x7E)
                while (i < input.size() && (input[i] < 0x40 || input[i] > 0x7E)) i++;
                if (i < input.size()) i++; // skip final byte
            } else if (input[i + 1] == ']') {
                i += 2;
                // Skip until BEL or ST
                while (i < input.size() && input[i] != '\a' &&
                       !(i + 1 < input.size() && input[i] == '\033' && input[i + 1] == '\\')) i++;
                if (i < input.size() && input[i] == '\a') i++;
                else if (i + 1 < input.size()) i += 2;
            } else {
                i += 2; // skip other ESC sequences
            }
        } else {
            result += input[i];
            i++;
        }
    }
    return result;
}

/// Parse a CSI sequence from raw bytes (excluding the ESC[ prefix)
[[nodiscard]] inline auto parse_csi(std::string_view input)
    -> std::expected<CsiSequence, std::string> {
    if (input.empty()) return std::unexpected(std::string{"Empty CSI sequence"});

    CsiSequence csi;
    std::size_t i = 0;

    // Parse parameters (digits and semicolons)
    std::string param_buf;
    while (i < input.size() && ((input[i] >= '0' && input[i] <= '9') || input[i] == ';')) {
        param_buf += input[i];
        i++;
    }

    // Parse intermediate bytes (0x20-0x2F)
    while (i < input.size() && input[i] >= 0x20 && input[i] <= 0x2F) {
        csi.intermediate += input[i];
        i++;
    }

    // Final byte (0x40-0x7E)
    if (i < input.size() && input[i] >= 0x40 && input[i] <= 0x7E) {
        csi.final_byte = input[i];
    } else {
        return std::unexpected(std::string{"Missing or invalid CSI final byte"});
    }

    // Parse param_buf into integer params
    if (!param_buf.empty()) {
        std::size_t p = 0;
        while (p <= param_buf.size()) {
            int val = 0;
            bool has_digit = false;
            while (p < param_buf.size() && param_buf[p] >= '0' && param_buf[p] <= '9') {
                val = val * 10 + (param_buf[p] - '0');
                has_digit = true;
                p++;
            }
            if (has_digit) csi.params.push_back(val);
            else csi.params.push_back(0); // default param
            if (p < param_buf.size() && param_buf[p] == ';') p++;
            else break;
        }
    }

    return csi;
}

/// Parse a raw terminal input byte sequence into a structured event
[[nodiscard]] inline auto parse_input_sequence(std::string_view input)
    -> std::expected<ParsedInput, std::string> {
    if (input.empty()) return std::unexpected(std::string{"Empty input"});

    // Bracketed paste mode
    if (input.starts_with("\033[200~")) {
        auto end_pos = input.find("\033[201~");
        std::string content;
        if (end_pos != std::string_view::npos) {
            content = std::string(input.substr(6, end_pos - 6));
        } else {
            content = std::string(input.substr(6));
        }
        return PasteEvent{.content = std::move(content), .start = true, .end = (end_pos != std::string_view::npos)};
    }

    // Focus events
    if (input == "\033[I") return FocusEvent{.focused = true};
    if (input == "\033[O") return FocusEvent{.focused = false};

    // CSI sequences (ESC[...)
    if (input.size() >= 3 && input[0] == '\033' && input[1] == '[') {
        auto body = input.substr(2);
        char final_char = body.back();

        // Arrow keys and navigation
        if (final_char == 'A') return KeypressEvent{.key = SpecialKey::Up};
        if (final_char == 'B') return KeypressEvent{.key = SpecialKey::Down};
        if (final_char == 'C') return KeypressEvent{.key = SpecialKey::Right};
        if (final_char == 'D') return KeypressEvent{.key = SpecialKey::Left};
        if (final_char == 'H') return KeypressEvent{.key = SpecialKey::Home};
        if (final_char == 'F') return KeypressEvent{.key = SpecialKey::End};

        // Tilde sequences: ESC[N~
        if (final_char == '~') {
            int num = 0;
            for (std::size_t i = 0; i < body.size() - 1 && body[i] >= '0' && body[i] <= '9'; i++) {
                num = num * 10 + (body[i] - '0');
            }
            switch (num) {
                case 1: return KeypressEvent{.key = SpecialKey::Home};
                case 2: return KeypressEvent{.key = SpecialKey::Insert};
                case 3: return KeypressEvent{.key = SpecialKey::Delete};
                case 4: return KeypressEvent{.key = SpecialKey::End};
                case 5: return KeypressEvent{.key = SpecialKey::PageUp};
                case 6: return KeypressEvent{.key = SpecialKey::PageDown};
                case 15: return KeypressEvent{.key = SpecialKey::F5};
                case 17: return KeypressEvent{.key = SpecialKey::F6};
                case 18: return KeypressEvent{.key = SpecialKey::F7};
                case 19: return KeypressEvent{.key = SpecialKey::F8};
                case 20: return KeypressEvent{.key = SpecialKey::F9};
                case 21: return KeypressEvent{.key = SpecialKey::F10};
                case 23: return KeypressEvent{.key = SpecialKey::F11};
                case 24: return KeypressEvent{.key = SpecialKey::F12};
                default: break;
            }
        }

        // F1-F4 (ESC[OP through ESC[OS via SS3)
        if (body.size() == 1 && body[0] >= 'P' && body[0] <= 'S') {
            return KeypressEvent{.key = static_cast<SpecialKey>(
                static_cast<int>(SpecialKey::F1) + (body[0] - 'P'))};
        }

        // Mouse events (SGR mode: ESC[<btn;x;yM or ESC[<btn;x;ym)
        if (body.starts_with("<")) {
            // Parse SGR mouse: <btn;x;y[Mm]
            int btn = 0, x = 0, y = 0;
            std::size_t p = 1;
            while (p < body.size() && body[p] >= '0' && body[p] <= '9') { btn = btn * 10 + (body[p] - '0'); p++; }
            if (p < body.size() && body[p] == ';') p++;
            while (p < body.size() && body[p] >= '0' && body[p] <= '9') { x = x * 10 + (body[p] - '0'); p++; }
            if (p < body.size() && body[p] == ';') p++;
            while (p < body.size() && body[p] >= '0' && body[p] <= '9') { y = y * 10 + (body[p] - '0'); p++; }
            bool pressed = (p < body.size() && body[p] == 'M');

            MouseEvent me;
            me.x = x;
            me.y = y;
            me.button = btn & 0x03;
            me.pressed = pressed;
            me.released = !pressed;
            me.motion = (btn & 32) != 0;
            if (btn & 4) me.modifiers |= KeyModifier::Shift;
            if (btn & 8) me.modifiers |= KeyModifier::Alt;
            if (btn & 16) me.modifiers |= KeyModifier::Ctrl;
            return me;
        }

        // Resize (not typically via CSI but handled for completeness)
        if (final_char == 't' && body.size() > 2) {
            // ESC[8;rows;cols t
            if (body[0] == '8' && body[1] == ';') {
                int rows = 0, cols = 0;
                std::size_t p = 2;
                while (p < body.size() && body[p] >= '0' && body[p] <= '9') { rows = rows * 10 + (body[p] - '0'); p++; }
                if (p < body.size() && body[p] == ';') p++;
                while (p < body.size() && body[p] >= '0' && body[p] <= '9') { cols = cols * 10 + (body[p] - '0'); p++; }
                return ResizeEvent{.cols = cols, .rows = rows};
            }
        }
    }

    // SS3 sequences (ESC O followed by letter) - F1-F4
    if (input.size() == 3 && input[0] == '\033' && input[1] == 'O') {
        switch (input[2]) {
            case 'P': return KeypressEvent{.key = SpecialKey::F1};
            case 'Q': return KeypressEvent{.key = SpecialKey::F2};
            case 'R': return KeypressEvent{.key = SpecialKey::F3};
            case 'S': return KeypressEvent{.key = SpecialKey::F4};
            default: break;
        }
    }

    // Single escape
    if (input.size() == 1 && input[0] == '\033') {
        return KeypressEvent{.key = SpecialKey::Escape};
    }

    // Alt+key (ESC followed by a printable char)
    if (input.size() == 2 && input[0] == '\033' && input[1] >= 0x20) {
        return KeypressEvent{.key = static_cast<char32_t>(input[1]), .modifiers = KeyModifier::Alt};
    }

    // Control characters
    if (input.size() == 1) {
        unsigned char ch = static_cast<unsigned char>(input[0]);
        if (ch == 0x09) return KeypressEvent{.key = SpecialKey::Tab};
        if (ch == 0x0A || ch == 0x0D) return KeypressEvent{.key = SpecialKey::Enter};
        if (ch == 0x1B) return KeypressEvent{.key = SpecialKey::Escape};
        if (ch == 0x7F) return KeypressEvent{.key = SpecialKey::Backspace};
        if (ch < 0x20) {
            // Ctrl+letter (Ctrl+A = 0x01, etc.)
            return KeypressEvent{.key = static_cast<char32_t>(ch + 0x60), .modifiers = KeyModifier::Ctrl};
        }
        // Regular printable character
        return KeypressEvent{.key = static_cast<char32_t>(ch)};
    }

    // Multi-byte UTF-8 character
    if (!input.empty() && static_cast<unsigned char>(input[0]) >= 0x80) {
        // Decode UTF-8 to char32_t
        char32_t cp = 0;
        unsigned char first = static_cast<unsigned char>(input[0]);
        if ((first & 0xE0) == 0xC0 && input.size() >= 2) {
            cp = (first & 0x1F) << 6 | (static_cast<unsigned char>(input[1]) & 0x3F);
        } else if ((first & 0xF0) == 0xE0 && input.size() >= 3) {
            cp = (first & 0x0F) << 12 | (static_cast<unsigned char>(input[1]) & 0x3F) << 6 |
                 (static_cast<unsigned char>(input[2]) & 0x3F);
        } else if ((first & 0xF8) == 0xF0 && input.size() >= 4) {
            cp = (first & 0x07) << 18 | (static_cast<unsigned char>(input[1]) & 0x3F) << 12 |
                 (static_cast<unsigned char>(input[2]) & 0x3F) << 6 |
                 (static_cast<unsigned char>(input[3]) & 0x3F);
        }
        return KeypressEvent{.key = cp};
    }

    // Fallback: treat first byte as a character
    return KeypressEvent{.key = static_cast<char32_t>(input[0])};
}

// ============================================================================
// Generation Functions
// ============================================================================

/// Generate a CSI escape sequence from components
[[nodiscard]] inline auto generate_csi(
    std::span<const int> params,
    std::string_view intermediate,
    char final_byte) -> std::string {
    std::string result = "\033[";
    for (std::size_t i = 0; i < params.size(); i++) {
        if (i > 0) result += ';';
        result += std::to_string(params[i]);
    }
    result += intermediate;
    result += final_byte;
    return result;
}

/// Generate an SGR escape sequence from attributes
[[nodiscard]] inline auto generate_sgr(const SgrAttr& attr) -> std::string {
    std::string params;
    auto append = [&](int code) {
        if (!params.empty()) params += ';';
        params += std::to_string(code);
    };

    if (attr.bold) append(1);
    if (attr.dim) append(2);
    if (attr.italic) append(3);
    if (attr.underline) append(4);
    if (attr.inverse) append(7);
    if (attr.strikethrough) append(9);

    // Foreground color
    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Color16>) {
            int val = static_cast<int>(c);
            append(val < 8 ? 30 + val : 90 + (val - 8));
        } else if constexpr (std::is_same_v<T, Color256>) {
            append(38); append(5); append(c.index);
        } else if constexpr (std::is_same_v<T, TrueColor>) {
            append(38); append(2); append(c.r); append(c.g); append(c.b);
        }
    }, attr.fg);

    // Background color
    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Color16>) {
            int val = static_cast<int>(c);
            append(val < 8 ? 40 + val : 100 + (val - 8));
        } else if constexpr (std::is_same_v<T, Color256>) {
            append(48); append(5); append(c.index);
        } else if constexpr (std::is_same_v<T, TrueColor>) {
            append(48); append(2); append(c.r); append(c.g); append(c.b);
        }
    }, attr.bg);

    if (params.empty()) params = "0";
    return "\033[" + params + "m";
}

// ============================================================================
// Cursor Movement
// ============================================================================

/// Move cursor up by n lines
[[nodiscard]] inline auto cursor_up(int n = 1) -> std::string {
    return "\033[" + std::to_string(n) + "A";
}

/// Move cursor down by n lines
[[nodiscard]] inline auto cursor_down(int n = 1) -> std::string {
    return "\033[" + std::to_string(n) + "B";
}

/// Move cursor right by n columns
[[nodiscard]] inline auto cursor_right(int n = 1) -> std::string {
    return "\033[" + std::to_string(n) + "C";
}

/// Move cursor left by n columns
[[nodiscard]] inline auto cursor_left(int n = 1) -> std::string {
    return "\033[" + std::to_string(n) + "D";
}

/// Move cursor to beginning of line
[[nodiscard]] inline auto cursor_home() -> std::string {
    return "\033[H";
}

/// Save cursor position (DEC private)
[[nodiscard]] inline auto cursor_save() -> std::string {
    return "\033[s";
}

/// Restore cursor position (DEC private)
[[nodiscard]] inline auto cursor_restore() -> std::string {
    return "\033[u";
}

// ============================================================================
// Screen/Line Clearing
// ============================================================================

/// Clear entire screen
[[nodiscard]] inline auto clear_screen() -> std::string {
    return "\033[2J";
}

/// Clear entire current line
[[nodiscard]] inline auto clear_line() -> std::string {
    return "\033[2K";
}

/// Clear from cursor to end of line
[[nodiscard]] inline auto clear_to_end() -> std::string {
    return "\033[0K";
}

// ============================================================================
// OSC (Operating System Commands)
// ============================================================================

/// Set terminal window title via OSC 2
[[nodiscard]] inline auto set_title(std::string_view title) -> std::string {
    return "\033]2;" + std::string{title} + "\033\\";
}

// ============================================================================
// Terminal Modes
// ============================================================================

/// Enable mouse tracking (SGR pixel mode, DEC 1003 + 1006)
[[nodiscard]] inline auto enable_mouse_tracking() -> std::string {
    return "\033[?1003h\033[?1006h";
}

/// Disable mouse tracking
[[nodiscard]] inline auto disable_mouse_tracking() -> std::string {
    return "\033[?1006l\033[?1003l";
}

/// Enter alternate screen buffer (DEC 1049)
[[nodiscard]] inline auto enter_alternate_screen() -> std::string {
    return "\033[?1049h";
}

/// Leave alternate screen buffer (DEC 1049)
[[nodiscard]] inline auto leave_alternate_screen() -> std::string {
    return "\033[?1049l";
}

} // namespace cc::ui::termio
