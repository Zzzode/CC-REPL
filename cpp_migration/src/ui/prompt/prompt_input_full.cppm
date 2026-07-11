module;
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.prompt_input_full;

import cc.ui.layout;
import cc.ui.prompt_input;
import cc.ui.design.figures;
import cc.ui.common.types;  // unified PromptInputMode canonical enum

export namespace cc::ui::prompt {

// --- Context indicators (file attachments, IDE selections, etc.) ---

struct ImageAttachment {
    std::string path;
    int width;
    int height;
};

struct PastedTextRef {
    std::string label;
    int num_lines;
};

struct FileReference {
    std::string path;
    std::optional<int> start_line;
    std::optional<int> end_line;
};

struct IdeSelection {
    std::string file_path;
    int start_line;
    int end_line;
    std::string language;
};

using ContextAttachment = std::variant<
    ImageAttachment,
    PastedTextRef,
    FileReference,
    IdeSelection
>;

// --- Prompt mode (unified canonical enum from ui_types.cppm) ---
// Previously this file defined a local 8-value PromptInputMode:
//   {Normal, HistorySearch, SlashCommand, PlanMode, FastMode,
//    VimNormal, VimInsert, VimVisual}
// All values are now in cc::ui::common::PromptInputMode.
using cc::ui::common::PromptInputMode;

// --- Model selector state ---
struct ModelSelectorState {
    std::string current_model;
    std::string display_name;
    bool is_fast_mode;
    bool fast_mode_available;
    std::optional<std::string> fast_mode_unavailable_reason;
};

// --- Auto updater status ---
enum class UpdateStatus {
    None,
    Available,
    Downloading,
    Ready,
    Error
};

struct AutoUpdaterState {
    UpdateStatus status;
    std::optional<std::string> new_version;
    std::optional<std::string> error_message;
};

// --- Footer items ---
struct FooterItem {
    std::string label;
    std::string shortcut;
    std::optional<std::string> color;
};

// --- Notification state ---
struct NotificationState {
    std::string message;
    std::string level;  // "info", "warn", "error"
    std::chrono::steady_clock::time_point expires_at;
};

// --- Permission mode ---
enum class PermissionMode {
    Default,
    AcceptEdits,
    AcceptAll,
    Plan
};

// --- Effort level ---
enum class EffortLevel {
    Low,
    Medium,
    High,
    Auto
};

// --- Prompt suggestion state ---
struct PromptSuggestion {
    std::string text;
    std::string source;  // "history", "ai", "command"
    float confidence;
};

struct ActiveSpeculation {
    std::string partial_text;
    std::string suggested_completion;
    bool is_visible;
};

// --- Agent/teammate state ---
struct AgentInfo {
    std::string name;
    std::string color;
    std::string status;  // "running", "idle", "done"
};

// --- Full prompt input props ---
struct PromptInputFullProps {
    // Input state
    cc::ui::InputBuffer* buffer;
    cc::ui::HistoryManager* history;
    cc::ui::Typeahead* typeahead;
    // Canonical vim mode (replaces cc::ui::VimHandler* pointer).
    // TS REF: src/types/textInputTypes.ts:222 — VimMode type.
    // vim_mode = Insert is the default (matches TS useVimInput initial state).
    cc::ui::common::VimMode vim_mode = cc::ui::common::VimMode::Insert;

    // Context
    std::vector<ContextAttachment> context_items;
    std::vector<FooterItem> footer_items;

    // Model
    ModelSelectorState model_state;

    // Mode
    PromptInputMode mode;
    PermissionMode permission_mode;
    EffortLevel effort;

    // Suggestions
    std::optional<PromptSuggestion> suggestion;
    std::optional<ActiveSpeculation> speculation;

    // Agent
    std::optional<AgentInfo> active_agent;
    std::vector<AgentInfo> running_teammates;

    // Notifications
    std::optional<NotificationState> notification;
    std::optional<AutoUpdaterState> updater;

    // Terminal
    int terminal_width;
    int terminal_height;

    // Callbacks
    std::function<void(std::string_view)> on_submit;
    std::optional<std::function<void()>> on_interrupt;
    std::optional<std::function<void()>> on_escape;
    std::optional<std::function<void(PermissionMode)>> on_permission_cycle;
    std::optional<std::function<void()>> on_model_select;
};

// --- Rendering helpers ---

// Render a context attachment as a compact indicator
[[nodiscard]] inline auto render_context_indicator(const ContextAttachment& item)
    -> std::string {
    struct Visitor {
        auto operator()(const ImageAttachment& img) -> std::string {
            return "\033[35m\xF0\x9F\x96\xBC " + img.path + " (" +
                   std::to_string(img.width) + "x" + std::to_string(img.height) + ")\033[0m";
        }
        auto operator()(const PastedTextRef& ref) -> std::string {
            return "\033[33m\xF0\x9F\x93\x8B " + ref.label + " (" +
                   std::to_string(ref.num_lines) + " lines)\033[0m";
        }
        auto operator()(const FileReference& ref) -> std::string {
            std::string result = "\033[36m\xF0\x9F\x93\x84 " + ref.path;
            if (ref.start_line.has_value()) {
                result += ":" + std::to_string(*ref.start_line);
                if (ref.end_line.has_value()) {
                    result += "-" + std::to_string(*ref.end_line);
                }
            }
            result += "\033[0m";
            return result;
        }
        auto operator()(const IdeSelection& sel) -> std::string {
            return "\033[34m\xE2\x9C\x82 " + sel.file_path + ":" +
                   std::to_string(sel.start_line) + "-" +
                   std::to_string(sel.end_line) + " [" + sel.language + "]\033[0m";
        }
    };
    return std::visit(Visitor{}, item);
}

// Render the TS-faithful prompt PREFIX glyph (glyph + trailing space).
//
// TS REF: src/components/PromptInput/PromptInputModeIndicator.tsx:82 — the
// prompt line begins with exactly ONE of two glyphs: '!' in bash mode,
// otherwise `figures.pointer` ('❯').  This is the first thing the user sees
// on every render, so it MUST match TS byte-for-byte.  The glyph bytes live
// in cc::ui::design::figures (single source of truth); colour is applied by
// the caller (bashBorder for bash, teammate/theme.text otherwise), mirroring
// how TS supplies it via Ink's <Text color=…> rather than embedding ANSI.
//
// NOTE: bash mode is detected from the leading '!' via figures::get_mode_from_input.
// The unified PromptInputMode enum does include Bash, but this function intentionally
// returns the pointer glyph for ALL modes — bash prefix is rendered separately by
// the caller (repl_screen) using figures::kBashPrefix.  TS PromptInputModeIndicator
// only ever emits '❯' or '!' at the prefix position.
[[nodiscard]] inline auto render_prompt_prefix(PromptInputMode /*mode*/)
    -> std::string {
    return std::string(cc::ui::design::figures::kPointerPrefix);
}

// Render the mode indicator (plan mode, fast mode, vim mode, etc.).
//
// NOTE (glyph unification, audit round7 prefix-glyph-no-unified-impl):
//   This is a CPP-ONLY status BADGE row ("[PLAN]/[FAST]/[N]" + permission
//   lock emoji).  It is NOT the prompt PREFIX glyph — TS's
//   PromptInputModeIndicator only ever emits '❯' or '!' at the prefix
//   position (see render_prompt_prefix above / cc::ui::design::figures).
//   These badges are supplementary chrome retained for the interactive UX;
//   they must never be used as a replacement for the '❯'/'!' prefix.
[[nodiscard]] inline auto render_mode_indicator(PromptInputMode mode,
                                                 PermissionMode perm_mode)
    -> std::string {
    std::string result;
    switch (mode) {
        case PromptInputMode::Normal: break;
        case PromptInputMode::HistorySearch:
            result = "\033[33m(reverse-i-search)\033[0m ";
            break;
        case PromptInputMode::SlashCommand:
            result = "\033[36m/\033[0m";
            break;
        case PromptInputMode::PlanMode:
            result = "\033[35m[PLAN]\033[0m ";
            break;
        case PromptInputMode::FastMode:
            result = "\033[32m[FAST]\033[0m ";
            break;
        case PromptInputMode::VimNormal:
            result = "\033[34m[N]\033[0m ";
            break;
        case PromptInputMode::VimInsert:
            result = "\033[32m[I]\033[0m ";
            break;
        case PromptInputMode::VimVisual:
            result = "\033[33m[V]\033[0m ";
            break;
        // Unified enum fallthrough — modes that don't render a badge here:
        // Bash (rendered as '!' prefix glyph, not a badge),
        // FileRef / Agent / BgRun / MCP (prefix-triggered, surfaced elsewhere),
        // OrphanedPermission / TaskNotification (TS: fall through to pointer),
        // Search (handled by HistorySearch above), Normal (no badge).
        default: break;
    }

    switch (perm_mode) {
        case PermissionMode::Default: break;
        case PermissionMode::AcceptEdits:
            result += "\033[33m\xF0\x9F\x94\x93\033[0m ";
            break;
        case PermissionMode::AcceptAll:
            result += "\033[31m\xF0\x9F\x94\x93\xF0\x9F\x94\x93\033[0m ";
            break;
        case PermissionMode::Plan:
            result += "\033[35m\xF0\x9F\x93\x8B\033[0m ";
            break;
    }
    return result;
}

// Render the prompt suggestion ghost text
[[nodiscard]] inline auto render_suggestion_ghost(const PromptSuggestion& suggestion)
    -> std::string {
    return "\033[2m" + suggestion.text + "\033[0m";
}

// Render the speculation overlay
[[nodiscard]] inline auto render_speculation(const ActiveSpeculation& spec)
    -> std::string {
    if (!spec.is_visible) return "";
    return "\033[2;3m" + spec.suggested_completion + "\033[0m";
}

// Render footer bar with shortcuts
[[nodiscard]] inline auto render_footer(const std::vector<FooterItem>& items,
                                         int terminal_width) -> std::string {
    std::string result = "\033[2m";
    int used_width = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto entry = items[i].shortcut + ":" + items[i].label;
        if (used_width + static_cast<int>(entry.size()) + 2 > terminal_width) break;
        if (i > 0) { result += "  "; used_width += 2; }
        result += entry;
        used_width += static_cast<int>(entry.size());
    }
    result += "\033[0m";
    return result;
}

// Render running teammate indicators
[[nodiscard]] inline auto render_teammate_indicators(const std::vector<AgentInfo>& teammates)
    -> std::string {
    if (teammates.empty()) return "";
    std::string result = "\033[2m[";
    for (std::size_t i = 0; i < teammates.size(); ++i) {
        if (i > 0) result += ", ";
        result += teammates[i].name;
        if (teammates[i].status == "running") result += "\xE2\x9A\xA1";
    }
    result += "]\033[0m";
    return result;
}

// Render the notification banner
[[nodiscard]] inline auto render_notification(const NotificationState& notif) -> std::string {
    if (notif.level == "error") return "\033[31m\xE2\x9D\x8C " + notif.message + "\033[0m";
    if (notif.level == "warn") return "\033[33m\xE2\x9A\xA0 " + notif.message + "\033[0m";
    return "\033[2m\xE2\x84\xB9 " + notif.message + "\033[0m";
}

// --- Full render function ---

// Render the complete prompt input area with all indicators
[[nodiscard]] inline auto render_prompt_input_full(const PromptInputFullProps& props)
    -> std::expected<std::string, std::string> {
    if (!props.buffer || !props.history || !props.typeahead) {
        return std::unexpected(std::string("PromptInputFull: null pointer in props"));
    }

    std::string result;

    // Notification banner (if active)
    if (props.notification.has_value()) {
        auto now = std::chrono::steady_clock::now();
        if (now < props.notification->expires_at) {
            result += render_notification(*props.notification) + "\n";
        }
    }

    // Context indicators
    if (!props.context_items.empty()) {
        for (const auto& item : props.context_items) {
            result += "  " + render_context_indicator(item) + "\n";
        }
    }

    // Mode indicator + prompt prefix
    result += render_mode_indicator(props.mode, props.permission_mode);

    // Agent indicator
    if (props.active_agent.has_value()) {
        result += "\033[1m@" + props.active_agent->name + "\033[0m ";
    }

    // Model indicator
    result += "\033[2m(" + props.model_state.display_name + ")\033[0m ";

    // Input content
    auto content = props.buffer->content();
    result += content;

    // Suggestion ghost text
    if (props.suggestion.has_value() && !content.empty()) {
        result += render_suggestion_ghost(*props.suggestion);
    }

    // Speculation overlay
    if (props.speculation.has_value()) {
        result += render_speculation(*props.speculation);
    }

    result += "\n";

    // Running teammates
    if (!props.running_teammates.empty()) {
        result += render_teammate_indicators(props.running_teammates) + "\n";
    }

    // Footer
    if (!props.footer_items.empty()) {
        result += render_footer(props.footer_items, props.terminal_width);
    }

    return result;
}

// --- FTXUI Component factories ---

// Create the full interactive prompt input FTXUI Component
[[nodiscard]] auto make_prompt_input_full_component(
    PromptInputFullProps props) -> ftxui::Component;

// Create a model selector dropdown component
[[nodiscard]] auto make_model_selector_component(
    ModelSelectorState state,
    std::function<void(std::string_view)> on_select) -> ftxui::Component;

// Create an autocomplete overlay component
[[nodiscard]] auto make_autocomplete_overlay_component(
    cc::ui::Typeahead* typeahead,
    std::function<void(std::string_view)> on_accept) -> ftxui::Component;

} // namespace cc::ui::prompt
