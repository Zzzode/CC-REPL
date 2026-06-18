/// @file runtime_computer_use.cppm
/// @brief Computer-use JSON serialization helpers, extracted from the (former)
/// monolithic runtime_registry.cppm as part of audit §13 #1.
///
/// These are pure functions that build the JSON payload describing a
/// computer-use action (mouse/keyboard). They depend only on the
/// cc::core::computer_use action types — no runtime_registry internals.
module;

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

export module cc.tools.runtime_computer_use;

import cc.tools.computer_use;

export namespace cc::tools::runtime_computer_use {

using cc::core::computer_use::ActionType;

/// Map a computer-use ActionType to its wire string.
[[nodiscard]] inline std::string_view action_name(ActionType action) {
    switch (action) {
        case ActionType::Screenshot: return "screenshot";
        case ActionType::MouseMove: return "move";
        case ActionType::MouseClick: return "click";
        case ActionType::MouseDoubleClick: return "double_click";
        case ActionType::MouseRightClick: return "right_click";
        case ActionType::MouseDrag: return "drag";
        case ActionType::KeyType: return "type";
        case ActionType::KeyPress: return "press";
        case ActionType::KeyHotkey: return "hotkey";
        case ActionType::Scroll: return "scroll";
    }
    return "unknown";
}

/// Escape a string for embedding inside a JSON string literal.
[[nodiscard]] inline std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += R"(\\)"; break;
            case '"': escaped += R"(\")"; break;
            case '\b': escaped += R"(\b)"; break;
            case '\f': escaped += R"(\f)"; break;
            case '\n': escaped += R"(\n)"; break;
            case '\r': escaped += R"(\r)"; break;
            case '\t': escaped += R"(\t)"; break;
            default:
                if (ch < 0x20) escaped += std::format(R"(\u{:04x})", static_cast<unsigned>(ch));
                else escaped.push_back(static_cast<char>(ch));
                break;
        }
    }
    return escaped;
}

namespace detail {

inline void append_separator(std::string& out, bool& first) {
    if (!first) out += ',';
    first = false;
}

inline void append_string(std::string& out, std::string_view key, std::string_view value, bool& first) {
    append_separator(out, first);
    out += std::format(R"("{}":"{}")", json_escape(key), json_escape(value));
}

inline void append_int(std::string& out, std::string_view key, std::int64_t value, bool& first) {
    append_separator(out, first);
    out += std::format(R"("{}":{})", json_escape(key), value);
}

} // namespace detail

/// Build the JSON payload describing a computer-use action.
[[nodiscard]] inline std::string command_request_json(const cc::core::computer_use::ComputerAction& action) {
    std::string out = "{";
    bool first = true;
    detail::append_string(out, "action", action_name(action.type), first);
    if (action.position && !action.region) {
        detail::append_int(out, "x", action.position->x, first);
        detail::append_int(out, "y", action.position->y, first);
    }
    if (action.drag_end) {
        detail::append_int(out, "to_x", action.drag_end->x, first);
        detail::append_int(out, "to_y", action.drag_end->y, first);
    }
    if (action.text) {
        detail::append_string(out, "text", *action.text, first);
    }
    if (!action.keys.empty()) {
        detail::append_separator(out, first);
        out += R"("keys":[)";
        for (std::size_t i = 0; i < action.keys.size(); ++i) {
            if (i != 0) out += ',';
            out += '"';
            out += json_escape(action.keys[i]);
            out += '"';
        }
        out += ']';
    }
    if (action.region) {
        detail::append_int(out, "x", action.region->x, first);
        detail::append_int(out, "y", action.region->y, first);
        detail::append_int(out, "width", action.region->width, first);
        detail::append_int(out, "height", action.region->height, first);
        detail::append_separator(out, first);
        out += std::format(
            R"("region":{{"x":{},"y":{},"width":{},"height":{}}})",
            action.region->x,
            action.region->y,
            action.region->width,
            action.region->height);
    }
    out += '}';
    return out;
}

/// Replace all occurrences of `needle` in `text` with `replacement`.
inline void replace_all(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

} // namespace cc::tools::runtime_computer_use
