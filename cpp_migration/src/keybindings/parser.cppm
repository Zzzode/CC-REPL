/// @file parser.cppm
/// @brief Keybinding expression parser.
/// Migrated from src/keybindings/parser.ts - parses keybinding expressions
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <variant>

export module cc.keybindings.parser;

export namespace cc::keybindings::parser {

// ============================================================
// Types
// ============================================================

/// Token types produced by the keybinding tokenizer
enum class KeyToken : std::uint8_t {
    Ctrl,
    Alt,
    Shift,
    Meta,
    Key,
    Plus,
    Invalid,
};

/// A fully parsed key combination
struct ParsedKey {
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
    bool meta{false};
    std::string key;
};

/// A sequence of parsed keys (e.g., for multi-chord bindings)
using KeySequence = std::vector<ParsedKey>;

// ============================================================
// Functions
// ============================================================

/// Normalize a key name to its canonical form
[[nodiscard]] inline std::string normalize_key(std::string_view key) {
    std::string result(key);
    for (auto& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // Normalize common aliases
    if (result == "control") return "ctrl";
    if (result == "option") return "alt";
    if (result == "cmd" || result == "command" || result == "super") return "meta";
    if (result == "esc") return "escape";
    if (result == "del") return "delete";
    if (result == "ins") return "insert";
    if (result == "bs" || result == "backspace") return "backspace";
    if (result == "cr" || result == "return") return "enter";
    if (result == "space" || result == " ") return "space";
    return result;
}

/// Tokenize a key string into individual tokens
[[nodiscard]] inline std::vector<std::pair<KeyToken, std::string>> tokenize_key_string(
    std::string_view input)
{
    std::vector<std::pair<KeyToken, std::string>> tokens;
    std::string current;

    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '+') {
            if (!current.empty()) {
                auto normalized = normalize_key(current);
                KeyToken type = KeyToken::Key;
                if (normalized == "ctrl") type = KeyToken::Ctrl;
                else if (normalized == "alt") type = KeyToken::Alt;
                else if (normalized == "shift") type = KeyToken::Shift;
                else if (normalized == "meta") type = KeyToken::Meta;
                tokens.emplace_back(type, std::move(normalized));
                current.clear();
            }
            tokens.emplace_back(KeyToken::Plus, "+");
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        auto normalized = normalize_key(current);
        KeyToken type = KeyToken::Key;
        if (normalized == "ctrl") type = KeyToken::Ctrl;
        else if (normalized == "alt") type = KeyToken::Alt;
        else if (normalized == "shift") type = KeyToken::Shift;
        else if (normalized == "meta") type = KeyToken::Meta;
        tokens.emplace_back(type, std::move(normalized));
    }

    return tokens;
}

/// Parse a single key expression (e.g., "Ctrl+Shift+K") into a ParsedKey
[[nodiscard]] inline std::expected<ParsedKey, std::string> parse_key_expression(
    std::string_view expr)
{
    if (expr.empty()) {
        return std::unexpected(std::string("Empty key expression"));
    }

    auto tokens = tokenize_key_string(expr);
    ParsedKey result;
    bool found_key = false;

    for (const auto& [type, value] : tokens) {
        switch (type) {
            case KeyToken::Ctrl:  result.ctrl = true; break;
            case KeyToken::Alt:   result.alt = true; break;
            case KeyToken::Shift: result.shift = true; break;
            case KeyToken::Meta:  result.meta = true; break;
            case KeyToken::Key:
                if (found_key) {
                    return std::unexpected(
                        std::string("Multiple keys in expression: ") + std::string(expr));
                }
                result.key = value;
                found_key = true;
                break;
            case KeyToken::Plus:
                break;  // separator, skip
            case KeyToken::Invalid:
                return std::unexpected(
                    std::string("Invalid token in expression: ") + value);
        }
    }

    if (!found_key) {
        return std::unexpected(
            std::string("No key found in expression: ") + std::string(expr));
    }
    return result;
}

/// Parse a key sequence (space-separated chords, e.g., "Ctrl+K Ctrl+C")
[[nodiscard]] inline std::expected<KeySequence, std::string> parse_key_sequence(
    std::string_view expr)
{
    KeySequence sequence;
    std::size_t start = 0;

    while (start < expr.size()) {
        // Skip whitespace
        while (start < expr.size() && expr[start] == ' ') ++start;
        if (start >= expr.size()) break;

        // Find end of this chord
        auto end = expr.find(' ', start);
        if (end == std::string_view::npos) end = expr.size();

        auto chord_expr = expr.substr(start, end - start);
        auto parsed = parse_key_expression(chord_expr);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        sequence.push_back(std::move(*parsed));
        start = end;
    }

    if (sequence.empty()) {
        return std::unexpected(std::string("Empty key sequence"));
    }
    return sequence;
}

/// Convert a ParsedKey back to its string representation
[[nodiscard]] inline std::string key_to_string(const ParsedKey& key) {
    std::string result;
    if (key.ctrl) result += "Ctrl+";
    if (key.alt) result += "Alt+";
    if (key.shift) result += "Shift+";
    if (key.meta) result += "Meta+";
    result += key.key;
    return result;
}

/// Convert a KeySequence to its string representation
[[nodiscard]] inline std::string sequence_to_string(const KeySequence& sequence) {
    std::string result;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        if (i > 0) result += ' ';
        result += key_to_string(sequence[i]);
    }
    return result;
}

} // namespace cc::keybindings::parser
