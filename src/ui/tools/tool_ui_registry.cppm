/// @file tool_ui_registry.cppm
/// @brief Registry of per-tool UI rendering functions.
///
/// Maps tool_name -> { user_facing_name, message, tag, progress, queued, ... }
///
/// The TS reference puts these as methods on each tool class
/// (userFacingName, renderToolUseMessage, renderToolUseTag, etc.).
/// In C++ we keep UI concerns out of the tools layer (cc_tools) and
/// instead register UI renderers in a separate registry here.
///
/// MODULE:   cc.ui.tools.registry
/// LICENCE:  Exported.  Imported by message projection code and the
///           faithful tool-use message renderer.
///
/// TS REFERENCE:
///   Per-tool methods:
///     tool.userFacingName(input)            -> string (bold header)
///     tool.renderToolUseMessage(input)  -> ReactNode (summary in parens)
///     tool.renderToolUseTag?.(input)    -> ReactNode (optional badge)
///     tool.renderToolUseProgressMessage?.(input, partialResult?)
///     tool.renderToolUseQueuedMessage?.(input)
///     tool.isTransparentWrapper        -> bool
///     tool.extractSearchText?(output)  -> string (rich searchable text)
module;

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

export module cc.ui.tools.registry;

export namespace cc::ui::tools {

// ============================================================
// ToolUIFunctions — per-tool UI render function bundle
// ============================================================

/// Bundle of UI rendering functions for a single tool.
/// Mirrors the TS tool-class UI methods.
///
/// All functions take the tool's raw input JSON string.
struct ToolUIFunctions {
    /// userFacingName(input) — bold header text.
    ///
    /// TS: tool.userFacingName(input)
    std::function<std::string(std::string_view input_json)> user_facing_name;

    /// renderToolUseMessage(input) — summary text shown in parens after the name.
    ///
    /// TS: tool.renderToolUseMessage(input)
    std::function<std::string(std::string_view input_json)> message;

    /// renderToolUseTag?(input) — optional tag/badge shown after the message.
    ///
    /// TS: tool.renderToolUseTag?.(input) — returns nullopt for no tag.
    std::function<std::optional<std::string>(std::string_view input_json)> tag;

    /// renderToolUseProgressMessage?(input, partialResult?) — progress line.
    ///
    /// TS: tool.renderToolUseProgressMessage?.(input, partialResult)
    std::function<std::string(std::string_view input_json,
                              std::string_view partial_result)> progress;

    /// renderToolUseQueuedMessage?(input) — text shown when queued.
    ///
    /// TS: tool.renderToolUseQueuedMessage?.(input)
    std::function<std::string(std::string_view input_json)> queued;

    /// If true, only the progress line is shown (no header row).
    ///
    /// TS: tool.isTransparentWrapper
    bool is_transparent_wrapper = false;

    /// extractSearchText(output_text, content_items_text) — rich searchable text
    /// from tool result.  Returns nullopt when not implemented (caller falls
    /// back to field-name heuristic).  Returns "" when tool explicitly says
    /// "nothing to index" (e.g. FileRead — content not shown on screen).
    ///
    /// TS REF: src/Tool.ts L599  extractSearchText?(out: Output): string
    ///   Per-tool implementations:
    ///     BashTool.tsx L549    → stdout + "\n" + stderr
    ///     GrepTool.ts L250     → content (mode=content) or filenames.join
    ///     GlobTool.ts L151     → filenames.join
    ///     FileReadTool.ts L414 → "" (UI shows metadata, not file content)
    ///     FileWriteTool.ts L146→ "" (update mode hides content → phantom)
    ///     WebSearchTool.ts L229→ "" (results not shown on screen)
    ///
    /// Arguments:
    ///   output_text   — flattened output string (ToolResultOptions.output)
    ///   error_text    — error message if any (ToolResultOptions.error_message)
    std::function<std::optional<std::string>(
        std::string_view output_text,
        std::string_view error_text)> extract_search_text;
};

// ============================================================
// ToolUIRegistry
// ============================================================

/// Registry mapping tool_name -> ToolUIFunctions.
///
/// Use `register_tool_ui()` to add per-tool UI functions.
/// Use `get_tool_ui()` to look up by name; falls back to generic if not found.
///
/// The registry is not thread-safe; register all tools at startup.
class ToolUIRegistry {
  public:
    /// Register UI functions for a tool.
    /// Replaces any existing entry for the same name.
    void register_tool_ui(std::string name, ToolUIFunctions fns) {
        entries_[std::move(name)] = std::move(fns);
    }

    /// Look up UI functions for a tool by name.
    /// Returns pointer to the registered functions, or nullptr if not found.
    ///
    /// Callers should handle the "not found" case by falling back to a
    /// generic renderer (see tool_ui_generic.cppm).
    [[nodiscard]] const ToolUIFunctions* find(std::string_view name) const {
        // string_view → string lookup (unordered_map doesn't support
        // heterogeneous lookup with default hash).  Accept the allocation
        // — tool names are short and this is called once per tool render.
        auto it = entries_.find(std::string{name});
        if (it == entries_.end()) return nullptr;
        return &it->second;
    }

    /// Get number of registered tools.
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    /// Check if a tool is registered.
    [[nodiscard]] bool contains(std::string_view name) const {
        return entries_.contains(std::string{name});
    }

  private:
    std::unordered_map<std::string, ToolUIFunctions> entries_;
};

// ============================================================
// Global registry accessor (singleton pattern)
// ============================================================

/// Get the global tool UI registry.
/// All built-in tool UIs are registered here at startup.
[[nodiscard]] inline ToolUIRegistry& global_tool_ui_registry() {
    static ToolUIRegistry registry;
    return registry;
}

}  // namespace cc::ui::tools
