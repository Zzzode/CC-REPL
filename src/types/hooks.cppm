/// @file hooks.cppm
/// @brief Hook type definitions for UI state management.
/// Migrated from src/types/hooks.ts, textInputTypes.ts
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>

export module cc.types.hooks;

export namespace cc::types {

/// Text input cursor position
struct CursorPosition {
    int line = 0;
    int column = 0;
};

/// Text input state
struct TextInputState {
    std::string value;
    CursorPosition cursor;
    std::optional<std::pair<int, int>> selection;  // start, end offsets
    bool is_focused = false;
    bool is_multiline = false;
};

/// Completion suggestion
struct CompletionItem {
    std::string label;
    std::string insert_text;
    std::optional<std::string> detail;
    std::optional<std::string> documentation;
};

/// Text input update actions
enum class TextInputAction : std::uint8_t {
    Insert,
    Delete,
    Backspace,
    MoveCursor,
    SelectAll,
    Clear,
    SetValue,
    Accept,
};

} // namespace cc::types
