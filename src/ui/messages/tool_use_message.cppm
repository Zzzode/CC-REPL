/// @file tool_use_message.cppm
/// @brief Tool call message rendering with FTXUI - displays tool invocations,
/// their parameters (JSON/YAML highlighted), status pill with spinner,
/// duration, retry count, collapsible args, grouped-tools stack, and footer
/// action buttons (Retry / Copy / Open In Editor).
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <variant>
#include <sstream>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

export module cc.ui.messages.tool_use_message;

import cc.types.types;
import cc.ui.code_highlight;
// Import the shared spinner via its module-interface name.  The file exports
// `ui::components` so we alias at the bottom of this file for convenience.
import ui.components.spinner;
// For unescape_literal_newlines() + ansi_to_ftxui_elements() used by the
// Output: section to decode JSON-escaped newlines and render ANSI SGR codes.
import cc.ui.messages.message_tool_result;

export namespace cc::ui::messages::tool_use_message {
using namespace ftxui;

// Bring spinner types into a convenient local alias.
namespace spinner_ns = ::ui::components;

// ============================================================
// Types
// ============================================================

/// Status of a tool invocation
enum class ToolStatus : std::uint8_t {
    Pending,    // Waiting for permission
    Running,    // Currently executing
    Success,    // Completed successfully
    Error,      // Completed with error
    Cancelled,  // Cancelled by user
};

/// Retry metadata
struct RetryInfo {
    int current_attempt = 1;
    int max_attempts = 1;
};

/// A single tool-use call stack entry (for grouped-tools rendering)
struct ToolUseCallData {
    std::string tool_use_id;
    std::string tool_name;
    std::string server_name;        // MCP server or "built-in"
    ToolStatus status{ToolStatus::Pending};
    std::string raw_parameters;     // JSON / YAML payload
    std::string parameters_language;// "json" | "yaml" | "text"
    std::optional<std::string> result_preview;
    std::optional<std::string> error_message;
    std::chrono::milliseconds duration{0};
    std::optional<std::string> file_path;
    RetryInfo retry;
    bool args_collapsed = true;
    std::optional<int> param_key_count;
    std::optional<std::size_t> param_byte_count;
};

/// Options for rendering a single tool-use block
struct ToolUseRenderOptions {
    ToolUseCallData call;
    bool show_result = false;
    int max_result_lines = 10;
    std::size_t args_collapse_threshold = 1024;  // >1KB => default collapsed
    bool is_grouped_member = false;
    std::function<void()> on_toggle_args;
    std::function<void()> on_retry;
    std::function<void()> on_copy_params;
    std::function<void()> on_copy_result;
    std::function<void()> on_open_in_editor;  // only shown for file_edit-like tools
    std::function<void()> on_toggle_collapse; // full-card collapse
};

/// Options for a grouped-tools stack (multiple tool_use sharing header)
struct GroupedToolsOptions {
    std::vector<ToolUseCallData> calls;
    std::string group_title;            // optional summary override
    bool group_collapsed = true;
    std::size_t args_collapse_threshold = 1024;
    int max_visible_preview = 3;         // when group collapsed, preview N entries
    std::function<void()> on_toggle_group;
    std::function<void(std::size_t idx)> on_call_retry;
    std::function<void(std::size_t idx)> on_call_copy_params;
    std::function<void(std::size_t idx)> on_call_open_in_editor;
};

// ============================================================
// Helpers
// ============================================================

constexpr std::size_t KParamKeyFallbackGuess = 5;
constexpr std::size_t kLargeContentWarnBytes = 5 * 1024 * 1024;  // 5 MB

/// Count JSON/YAML top-level keys (best-effort, used for summary pill).
[[nodiscard]] inline std::size_t estimate_key_count(std::string_view payload,
                                                    std::string_view lang) {
    // Heuristic: JSON => count top-level commas + 1 inside first { ... }.
    // YAML => count top-level "key:" lines at indent==0.
    if (payload.empty()) return 0;
    if (lang == "json" || lang == "text") {
        auto first = payload.find('{');
        if (first == std::string_view::npos) return 1;
        auto last = payload.rfind('}');
        if (last == std::string_view::npos || last <= first) return 1;
        auto inner = payload.substr(first + 1, last - first - 1);
        int depth = 0;
        std::size_t count = 1;
        bool in_string = false;
        char prev = 0;
        for (char c : inner) {
            if (in_string) {
                if (c == '"' && prev != '\\') in_string = false;
            } else {
                if (c == '"') in_string = true;
                else if (c == '{' || c == '[' || c == '(') ++depth;
                else if (c == '}' || c == ']' || c == ')') --depth;
                else if (c == ',' && depth == 0) ++count;
            }
            prev = c;
        }
        return count;
    }
    // YAML fallback
    std::size_t count = 0;
    std::string_view::size_type pos = 0;
    while (pos < payload.size()) {
        auto nl = payload.find('\n', pos);
        auto line = payload.substr(pos, nl - pos);
        if (!line.empty() && line[0] != ' ' && line[0] != '#' && line.find(':') != std::string_view::npos) {
            ++count;
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return count == 0 ? KParamKeyFallbackGuess : count;
}

[[nodiscard]] inline std::pair<std::string, Color> status_display(ToolStatus status) {
    switch (status) {
        case ToolStatus::Pending:   return {"◯ Pending",   Color::Yellow};
        case ToolStatus::Running:   return {"⟳ Running",   Color::Cyan};
        case ToolStatus::Success:   return {"✓ Success",   Color::Green};
        case ToolStatus::Error:     return {"✗ Failed",    Color::Red};
        case ToolStatus::Cancelled: return {"⊘ Cancelled", Color::GrayDark};
    }
    return {"?", Color::White};
}

/// Parse a status string from the shared projection contract
/// ("pending"|"running"|"success"|"error", with "cancelled" tolerated) into
/// the renderer's ToolStatus enum.  Unknown / empty => Pending (default).
/// G1 reads entry.tool_status (populated by G2 / app.cppm) via this helper.
[[nodiscard]] inline ToolStatus parse_tool_status(std::string_view s) {
    if (s == "running")          return ToolStatus::Running;
    if (s == "success")          return ToolStatus::Success;
    if (s == "error" || s == "failed")
                                return ToolStatus::Error;
    if (s == "cancelled" || s == "canceled")
                                return ToolStatus::Cancelled;
    return ToolStatus::Pending;  // "pending" / "" / unknown
}

[[nodiscard]] inline std::string tool_icon(std::string_view tool_name) {
    using namespace std::string_view_literals;
    auto has = [&](std::string_view s) {
        return std::search(tool_name.begin(), tool_name.end(),
                           s.begin(), s.end(),
                           [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b));
                           }) != tool_name.end();
    };
    if (has("read")) return "📖";
    if (has("write") || has("edit")) return "✏️";
    if (has("bash") || has("shell")) return "⚡";
    if (has("glob")) return "🔍";
    if (has("grep") || has("search")) return "🔎";
    if (has("web")) return "🌐";
    if (has("agent") || has("team")) return "🤝";
    if (has("mcp")) return "🔌";
    if (has("skill")) return "🧩";
    return "🔧";
}

[[nodiscard]] inline bool is_file_edit_tool(std::string_view tool_name) {
    using namespace std::string_view_literals;
    auto has = [&](std::string_view s) {
        return std::search(tool_name.begin(), tool_name.end(),
                           s.begin(), s.end(),
                           [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b));
                           }) != tool_name.end();
    };
    return has("edit") || has("write") || has("fileedit") || has("file_edit");
}

[[nodiscard]] inline std::string format_bytes(std::size_t bytes) {
    if (bytes < 1024) return std::format("{}B", bytes);
    if (bytes < 1024 * 1024) return std::format("{:.1f}KB", bytes / 1024.0);
    return std::format("{:.2f}MB", bytes / (1024.0 * 1024.0));
}

[[nodiscard]] inline std::string format_duration(std::chrono::milliseconds ms) {
    auto c = ms.count();
    if (c <= 0) return "0ms";
    if (c < 1000) return std::format("{}ms", c);
    if (c < 60 * 1000) return std::format("{:.1f}s", c / 1000.0);
    auto m = c / 60000;
    auto s = (c % 60000) / 1000;
    return std::format("{}m{}s", m, s);
}

// ============================================================
// Parameter block rendering (delegates to code_highlight)
// ============================================================

/// Render parameters as highlighted JSON/YAML.  When size exceeds threshold
/// the block is collapsed with a `{N keys, M bytes}` summary pill.
[[nodiscard]] inline Element RenderParameterBlock(const ToolUseCallData& call,
                                                  bool force_collapsed,
                                                  std::size_t collapse_threshold) {
    const bool oversized = call.raw_parameters.size() > kLargeContentWarnBytes;
    const auto n_keys = call.param_key_count.value_or(
        static_cast<int>(estimate_key_count(call.raw_parameters, call.parameters_language)));
    const auto n_bytes = call.param_byte_count.value_or(call.raw_parameters.size());

    const bool collapsed_by_size = n_bytes > collapse_threshold;
    const bool collapsed = force_collapsed || call.args_collapsed || collapsed_by_size;

    // Header row: summary pill + collapse hint
    Elements summary_row = {
        text("  Args: ") | dim | color(Color::GrayDark),
        text(std::format("{{{} keys, {}}}",
                         n_keys, format_bytes(n_bytes)))
            | color(Color::Cyan) | dim,
    };

    if (oversized) {
        summary_row.push_back(filler());
        summary_row.push_back(
            text(" ⚠ Oversized — write to file to inspect") | color(Color::Orange1) | dim);
    } else if (collapsed) {
        summary_row.push_back(filler());
        summary_row.push_back(text("▸ Press 'a' to expand ") | dim | color(Color::GrayDark));
    } else {
        summary_row.push_back(filler());
        summary_row.push_back(text("▾ Press 'a' to collapse ") | dim | color(Color::GrayDark));
    }

    Elements body = {hbox(summary_row)};

    if (!collapsed && !oversized) {
        cc::ui::code_highlight::CodeHighlightOptions opts;
        opts.source = call.raw_parameters;
        opts.language = call.parameters_language.empty() ? "json" : call.parameters_language;
        opts.show_line_numbers = true;
        opts.visible_lines = 30;
        auto highlighted = cc::ui::code_highlight::highlight_source(opts.source, opts.language);
        // indent + 2-space inner border
        auto code_block = cc::ui::code_highlight::RenderCodeHighlight(opts, highlighted);
        body.push_back(hbox({text("    "), code_block}));
    }

    return vbox(body);
}

// ============================================================
// Header: tool name + status pill + spinner + duration + retry
// ============================================================

[[nodiscard]] inline Element RenderToolHeader(const ToolUseCallData& call,
                                               int spinner_frame = 0) {
    auto [status_text, status_color] = status_display(call.status);
    Elements left;

    // Spinner / static status glyph
    if (call.status == ToolStatus::Running) {
        spinner_ns::SpinnerOptions sopts;
        sopts.mode = spinner_ns::SpinnerMode::Processing;
        sopts.reduced_motion = false;
        // Caller drives animation by passing a monotonically increasing
        // spinner_frame each repaint; SpinnerElement honours it to index the
        // glyph array so the tool spinner actually animates.
        sopts.frame = spinner_frame;
        left.push_back(spinner_ns::SpinnerElement(sopts));
        left.push_back(text(" "));
    } else {
        auto [icon, _] = status_display(call.status);
        // Only take the first glyph (the icon portion)
        auto space = icon.find(' ');
        auto glyph = space == std::string::npos ? icon : icon.substr(0, space);
        left.push_back(text(glyph) | color(status_color) | bold);
        left.push_back(text(" "));
    }

    left.push_back(text(tool_icon(call.tool_name) + " ") | dim);
    left.push_back(text(call.tool_name) | bold | color(Color::Magenta));

    if (!call.server_name.empty() && call.server_name != "built-in") {
        left.push_back(text(" (" + call.server_name + ")") | dim | color(Color::GrayDark));
    }

    Elements right;

    // Status pill
    right.push_back(text(" [") | color(status_color));
    right.push_back(text(status_text) | color(status_color) | bold);
    right.push_back(text("]") | color(status_color));

    // Retry
    if (call.retry.current_attempt > 1 || call.retry.max_attempts > 1) {
        right.push_back(text(" "));
        right.push_back(
            text(std::format("retry {}/{}", call.retry.current_attempt, call.retry.max_attempts))
                | color(Color::Yellow) | dim);
    }

    // Duration
    if (call.duration.count() > 0 || call.status == ToolStatus::Success ||
        call.status == ToolStatus::Error) {
        right.push_back(text(" "));
        right.push_back(text(format_duration(call.duration)) | color(Color::GrayDark) | dim);
    }

    return hbox({hbox(left), filler(), hbox(right)});
}

// ============================================================
// Footer action buttons
// ============================================================

[[nodiscard]] inline Element RenderToolFooter(const ToolUseCallData& call, bool has_error = false) {
    Elements buttons;
    auto pill = [](std::string_view label, Color col, bool enabled = true) -> Element {
        auto el = text(std::format("[{}]", label))
                  | ftxui::color(enabled ? col : Color::GrayDark)
                  | dim;
        if (!enabled) el = el | dim;
        return el;
    };

    if (has_error || call.status == ToolStatus::Error || call.status == ToolStatus::Cancelled) {
        buttons.push_back(pill("r Retry", Color::Yellow));
        buttons.push_back(text(" "));
    }
    buttons.push_back(pill("c Copy args", Color::Cyan));
    if (!call.raw_parameters.empty()) {
        // (always available)
    }
    if (call.result_preview || call.status == ToolStatus::Success) {
        buttons.push_back(text(" "));
        buttons.push_back(pill("C Copy result", Color::Cyan));
    }
    if (is_file_edit_tool(call.tool_name) && call.file_path) {
        buttons.push_back(text(" "));
        buttons.push_back(pill("o Open", Color::Blue));
    }

    if (buttons.empty()) return text("");
    return hbox({filler(), hbox(buttons)}) | size(HEIGHT, EQUAL, 1);
}

// ============================================================
// Render single ToolUse message
// ============================================================

/// Render a single tool use message.  Returns the Element.
[[nodiscard]] inline Element RenderToolUseMessage(const ToolUseRenderOptions& opts,
                                                  int spinner_frame = 0) {
    const auto& call = opts.call;
    auto [_, status_color] = status_display(call.status);

    auto header = RenderToolHeader(call, spinner_frame);
    Elements body_parts = {header};

    // File path indicator
    if (call.file_path) {
        body_parts.push_back(hbox({
            text("  → ") | color(Color::GrayDark),
            text(*call.file_path) | color(Color::Cyan) | dim,
        }));
    }

    // Parameters
    if (!call.raw_parameters.empty()) {
        body_parts.push_back(RenderParameterBlock(
            call,
            /*force_collapsed*/ !opts.show_result && opts.call.args_collapsed,
            opts.args_collapse_threshold));
    }

    // Error
    if (call.status == ToolStatus::Error && call.error_message) {
        body_parts.push_back(separator() | color(Color::Red));
        // word wrap the error manually, keep first 5 lines
        std::string msg = *call.error_message;
        int lines = 0;
        size_t pos = 0;
        while (pos < msg.size() && lines < 5) {
            auto nl = msg.find('\n', pos);
            std::string line;
            if (nl == std::string::npos) { line = msg.substr(pos); pos = msg.size(); }
            else { line = msg.substr(pos, nl - pos); pos = nl + 1; }
            body_parts.push_back(hbox({
                text("  ✗ ") | color(Color::Red),
                text(line) | color(Color::Red),
            }));
            ++lines;
        }
        if (pos < msg.size()) {
            body_parts.push_back(hbox({
                text("    ") | color(Color::Red),
                text("... (truncated)") | color(Color::Red) | dim,
            }));
        }
    } else if (opts.show_result && call.result_preview) {
        body_parts.push_back(separator() | dim);
        const std::string& preview = *call.result_preview;
        int lines = 0;
        size_t pos = 0;
        while (pos < preview.size() && lines < opts.max_result_lines) {
            auto nl = preview.find('\n', pos);
            std::string line;
            if (nl == std::string::npos) { line = preview.substr(pos); pos = preview.size(); }
            else { line = preview.substr(pos, nl - pos); pos = nl + 1; }
            body_parts.push_back(hbox({
                text("  │ ") | color(Color::GrayDark),
                text(line) | color(Color::GrayLight),
            }));
            ++lines;
        }
        if (pos < preview.size()) {
            body_parts.push_back(hbox({
                text("  ┆ ") | color(Color::GrayDark),
                text("...") | dim,
            }));
        }
    }

    // Footer
    body_parts.push_back(RenderToolFooter(call, /*has_error*/ call.status == ToolStatus::Error));

    Color border_color = status_color;
    if (call.status == ToolStatus::Pending) border_color = Color::Yellow;

    auto el = vbox(body_parts) | borderLight | color(border_color);

    if (opts.is_grouped_member) {
        // Don't double-border when inside a group
        el = vbox(body_parts) | color(border_color);
    }

    return el;
}

// ============================================================
// Grouped Tools (vertical stack with shared collapsible header)
// ============================================================

/// Render a grouped tool-use card.  When collapsed only shows counts and
/// max_visible_preview entries; when expanded renders every call in full.
[[nodiscard]] inline Element RenderGroupedTools(const GroupedToolsOptions& opts) {
    if (opts.calls.empty()) return text("");

    int running = 0, success = 0, error = 0, pending = 0;
    std::chrono::milliseconds total{0};
    for (const auto& c : opts.calls) {
        switch (c.status) {
            case ToolStatus::Running:   ++running; break;
            case ToolStatus::Success:   ++success; break;
            case ToolStatus::Error:     ++error; break;
            case ToolStatus::Pending:   ++pending; break;
            case ToolStatus::Cancelled: break;
        }
        total += c.duration;
    }
    Color overall_color = Color::GrayDark;
    if (error > 0) overall_color = Color::Red;
    else if (running > 0) overall_color = Color::Cyan;
    else if (success == static_cast<int>(opts.calls.size())) overall_color = Color::Green;
    else if (pending > 0) overall_color = Color::Yellow;

    // Header
    Elements header_parts;
    std::string title = opts.group_title.empty()
        ? std::format("{} tool calls", opts.calls.size())
        : opts.group_title;
    header_parts.push_back(text("🔗 ") | dim);
    header_parts.push_back(text(title) | bold | color(overall_color));

    // Status mini-pills
    Elements pills;
    if (running > 0) pills.push_back(text(std::format(" ⟳{}", running)) | color(Color::Cyan) | dim);
    if (success > 0) pills.push_back(text(std::format(" ✓{}", success)) | color(Color::Green) | dim);
    if (error > 0)   pills.push_back(text(std::format(" ✗{}", error))   | color(Color::Red)   | dim);
    if (pending > 0) pills.push_back(text(std::format(" ◯{}", pending)) | color(Color::Yellow) | dim);
    pills.push_back(text(" "));
    pills.push_back(text(format_duration(total)) | dim | color(Color::GrayDark));

    header_parts.push_back(filler());
    for (auto& p : pills) header_parts.push_back(std::move(p));
    header_parts.push_back(text(
        opts.group_collapsed ? " ▸ [Enter] expand" : " ▾ [Enter] collapse"
    ) | dim | color(Color::GrayDark));

    Elements rows = {hbox(header_parts)};

    if (opts.group_collapsed) {
        int limit = std::min(static_cast<int>(opts.calls.size()), opts.max_visible_preview);
        for (int i = 0; i < limit; ++i) {
            const auto& c = opts.calls[i];
            auto [st, sc] = status_display(c.status);
            rows.push_back(hbox({
                text("   ") | dim,
                text(st.substr(0, 1)) | color(sc),
                text(" "),
                text(tool_icon(c.tool_name) + " ") | dim,
                text(c.tool_name) | color(Color::Magenta) | dim,
                c.file_path ? text(std::format(" → {}", *c.file_path)) | color(Color::Cyan) | dim
                           : text(""),
                filler(),
                text(format_duration(c.duration)) | color(Color::GrayDark) | dim,
            }));
        }
        int hidden = static_cast<int>(opts.calls.size()) - limit;
        if (hidden > 0) {
            rows.push_back(
                text(std::format("   … and {} more [expand to see]", hidden))
                    | color(Color::GrayDark) | dim);
        }
    } else {
        for (std::size_t i = 0; i < opts.calls.size(); ++i) {
            ToolUseRenderOptions o;
            o.call = opts.calls[i];
            o.is_grouped_member = true;
            rows.push_back(hbox({text("  "), RenderToolUseMessage(o)}));
        }
    }

    return vbox(rows) | borderRounded | color(overall_color);
}

// ============================================================
// Interactive component (drives animation + events)
// ============================================================

/// Interactive single tool-use block.
[[nodiscard]] inline Component ToolUseMessage(ToolUseRenderOptions options) {
    struct State {
        ToolUseRenderOptions opts;
        bool expanded = false;
        int frame = 0;
    };
    auto s = std::make_shared<State>();
    s->opts = std::move(options);
    s->expanded = !s->opts.call.args_collapsed;

    return Renderer([s] {
        if (s->opts.call.status == ToolStatus::Running) ++s->frame;
        s->opts.show_result = s->expanded || s->opts.call.status == ToolStatus::Error;
        return RenderToolUseMessage(s->opts, s->frame);
    }) | CatchEvent([s](Event e) -> bool {
        if (e == Event::Return || e == Event::Character(' ')) {
            s->expanded = !s->expanded;
            s->opts.call.args_collapsed = !s->expanded;
            if (s->opts.on_toggle_collapse) s->opts.on_toggle_collapse();
            return true;
        }
        if (e == Event::Character('a')) {
            s->opts.call.args_collapsed = !s->opts.call.args_collapsed;
            if (s->opts.on_toggle_args) s->opts.on_toggle_args();
            return true;
        }
        if (e == Event::Character('r')) {
            if (s->opts.on_retry) s->opts.on_retry();
            return true;
        }
        if (e == Event::Character('c')) {
            if (s->opts.on_copy_params) s->opts.on_copy_params();
            return true;
        }
        if (e == Event::Character('C')) {
            if (s->opts.on_copy_result) s->opts.on_copy_result();
            return true;
        }
        if (e == Event::Character('o')) {
            if (s->opts.on_open_in_editor) s->opts.on_open_in_editor();
            return true;
        }
        return false;
    });
}

/// Interactive grouped-tools component
[[nodiscard]] inline Component GroupedToolsComponent(GroupedToolsOptions options) {
    struct State {
        GroupedToolsOptions opts;
    };
    auto s = std::make_shared<State>();
    s->opts = std::move(options);

    return Renderer([s] { return RenderGroupedTools(s->opts); })
         | CatchEvent([s](Event e) -> bool {
             if (e == Event::Return || e == Event::Character(' ')) {
                 s->opts.group_collapsed = !s->opts.group_collapsed;
                 if (s->opts.on_toggle_group) s->opts.on_toggle_group();
                 return true;
             }
             return false;
         });
}

// ============================================================
// M6: Faithful TS renderer (AssistantToolUseMessage.tsx parity)
// ============================================================
// Mirrors the TS default branch of AssistantToolUseMessage.tsx.
// The existing RenderToolUseMessage / ToolUseMessage above are the
// divergent FTXUI reimplementation (borders, status pills, footers,
// parameter blocks — all invented chrome not in TS).  The faithful
// version renders exactly what the TS Ink component does:
//
//   <Box flexDirection="row" justifyContent="space-between"
//        marginTop={addMargin?1:0} width="100%" backgroundColor={bg}>
//     <Box flexDirection="column">
//       <Box flexDirection="row" flexWrap="nowrap" minWidth={...}>
//         {shouldShowDot && <ToolUseLoader .../>}   // ● blinking/solid dot
//         <Box flexShrink={0}>
//           <Text bold wrap="truncate-end">
//             {userFacingToolName}
//           </Text>
//         </Box>
//         {renderedToolUseMessage !== '' && <Box><Text>({msg})</Text></Box>}
//         {tool.renderToolUseTag?.(input)}
//       </Box>
//       {!isResolved && !isQueued && progress/permission/classifier}
//       {!isResolved && isQueued && queuedMessage}
//     </Box>
//   </Box>
//
// No borders, no status pills, no timestamps, no footers, no
// parameter JSON blocks — just the dot + bold name + (summary) +
// optional progress line.

/// Status of a faithful tool-use row (matches TS isQueued / isResolved semantics)
enum class FaithfulToolStatus : std::uint8_t {
    Queued,     // Not in progress, not resolved → waiting in queue
    Running,    // In progress → show progress + blinking dot
    Success,    // Resolved successfully → solid green dot
    Error,      // Resolved with error → solid red dot
};

/// Data for the faithful tool-use renderer.  Mirrors the props that
/// AssistantToolUseMessage.tsx derives from (param + tools + lookups +
/// progressMessagesForMessage).  The caller (e.g. repl_screen projection)
/// is responsible for computing these values from the real tool definition
/// and execution state — this renderer only paints, matching TS visuals.
struct FaithfulToolUseData {
    std::string user_facing_name;    // tool.userFacingName(input) — bold header
    std::string message;             // tool.renderToolUseMessage(...) — in parens
    std::string tag;                 // tool.renderToolUseTag?.(input) — optional
    std::string progress_text;       // progress line text when Running
    std::string queued_text;         // progress line text when Queued
    std::string input_json;          // MCP tools only: raw JSON shown as "Input:"
    std::string output_text;         // MCP tools only: server response preview
    FaithfulToolStatus status{FaithfulToolStatus::Queued};
    bool should_show_dot = true;     // shouldShowDot prop
    bool add_margin = true;          // add marginTop=1
    bool is_transparent_wrapper = false;  // tool.isTransparentWrapper
    int spinner_frame = 0;           // drive blink animation
    bool should_animate = true;      // shouldAnimate prop
    bool is_mcp_tool = false;        // only MCP tools show Input:/Output: sections
};

/// Render a single tool-use header line: [dot] [BoldName] (message) [tag]
/// This is the equivalent of the first <Box flexDirection="row"> in TS.
[[nodiscard]] inline Element RenderFaithfulToolHeader(const FaithfulToolUseData& data) {
    Elements row;

    // Leading dot (ToolUseLoader).
    // TS behavior:
    //   Queued/Running (unresolved) → dim, blinks when animated
    //   Resolved success → solid "success" color (green)
    //   Resolved error → solid "error" color (red)
    if (data.should_show_dot) {
        const bool is_unresolved =
            (data.status == FaithfulToolStatus::Queued ||
             data.status == FaithfulToolStatus::Running);

        // Blink logic: when animating + unresolved + not error, alternate
        // between BLACK_CIRCLE and space on even/odd frames.  TS uses
        // useBlink() which toggles at ~500ms; we approximate with frame count.
        const bool blink_off =
            data.should_animate && is_unresolved &&
            (data.spinner_frame % 2 == 1);  // off every other "tick"

        if (blink_off) {
            // Space placeholder — maintains 2-cell minWidth like TS minWidth={2}
            row.push_back(text("  "));
        } else {
            // BLACK_CIRCLE = ⏺ (U+23FA).  TS uses "●" (U+25CF BLACK_CIRCLE)
            // but Ink/Chalk renders it; we use the same visual glyph.
            std::string glyph = "\xE2\x97\x8F ";  // ● + space (2 cells)
            Color fg = Color{};
            bool is_dim = false;

            if (is_unresolved) {
                is_dim = true;  // dimColor={isUnresolved}
                // color=undefined → default text color
            } else if (data.status == FaithfulToolStatus::Error) {
                fg = Color::Red;  // color="error"
            } else {
                fg = Color::Green;  // color="success"
            }

            auto el = text(glyph);
            if (is_dim) el = el | ftxui::dim;
            if (fg != Color{}) el = el | color(fg);
            row.push_back(std::move(el));
        }
    }

    // Bold user-facing tool name (flexShrink=0, wrap=truncate-end)
    if (!data.user_facing_name.empty()) {
        row.push_back(text(data.user_facing_name) | bold);
    }

    // Rendered message in parens "(message)"
    // TS: <Text>({renderedToolUseMessage})</Text> — plain, not dim
    if (!data.message.empty()) {
        Elements parts;
        parts.push_back(text(" ("));
        parts.push_back(text(data.message));
        parts.push_back(text(")"));
        row.push_back(hbox(std::move(parts)));
    }

    // Tool-specific tag (renderToolUseTag)
    // TS: tool.renderToolUseTag?.(input) — tool decides styling; we render as plain
    if (!data.tag.empty()) {
        row.push_back(text(" "));
        row.push_back(text(data.tag));
    }

    return hbox(std::move(row));
}

/// Render the full faithful tool-use message.  Mirrors the complete
/// AssistantToolUseMessage.tsx output including header + Input/Output
/// sections + progress/queued line below.
///
/// TS VISUAL STRUCTURE (from user screenshot):
///   ● Z.ai Built-in Tool: analyze_image
///     Input:
///     {"imageSource":"...","prompt":"..."}
///     Output:
///     Executing on server...
///     analyze_image_result_summary: [{"text":"..."}]
///
/// Returns a full-width Element (justifyContent: space-between on the
/// outer row, per TS).
[[nodiscard]] inline Element RenderFaithfulToolUseMessage(const FaithfulToolUseData& data) {
    // Transparent wrapper tools (e.g. TungstenTool, certain agent wrappers)
    // only show progress — no header.  TS: if (isTransparentWrapper) { ... }
    if (data.is_transparent_wrapper) {
        if (data.status == FaithfulToolStatus::Queued ||
            data.status == FaithfulToolStatus::Success ||
            data.status == FaithfulToolStatus::Error) {
            return text("");  // null — transparent + not running = invisible
        }
        // Running → show only progress text
        if (!data.progress_text.empty()) {
            Element progress = text(data.progress_text) | dim;
            if (data.add_margin) {
                return vbox({text(""), hbox({std::move(progress), filler()}) | flex});
            }
            return hbox({std::move(progress), filler()}) | flex;
        }
        return text("");
    }

    // If userFacingToolName is empty, return nothing (TS: returns null)
    if (data.user_facing_name.empty()) {
        return text("");
    }

    Elements column;
    column.push_back(RenderFaithfulToolHeader(data));

    const bool is_resolved =
        (data.status == FaithfulToolStatus::Success ||
         data.status == FaithfulToolStatus::Error);

    // ── Input section (MCP tools only) ────────────────────────────────
    // TS built-in tools (Bash, Read, Write, Edit, Glob, Grep) NEVER show
    // an "Input:" section — the command is already in the header parens.
    // Only MCP/custom tools show their raw JSON parameters here.
    if (data.is_mcp_tool && !data.input_json.empty() && data.input_json != "{}") {
        column.push_back(hbox({
            text("  "),
            text("Input:") | dim | bold,
        }));
        std::string input_display = data.input_json;
        constexpr std::size_t kMaxInputLen = 2000;
        if (input_display.size() > kMaxInputLen) {
            input_display = input_display.substr(0, kMaxInputLen) + "\xE2\x80\xA6";
        }
        std::size_t line_start = 0;
        while (line_start < input_display.size()) {
            auto nl = input_display.find('\n', line_start);
            std::string_view line = (nl == std::string::npos)
                ? std::string_view(input_display).substr(line_start)
                : std::string_view(input_display).substr(line_start, nl - line_start);
            column.push_back(hbox({
                text("  "),
                text(std::string(line)) | dim,
                filler(),
            }) | flex);
            if (nl == std::string::npos) break;
            line_start = nl + 1;
        }
    }

    // ── Progress / queued text (only while unresolved) ─────────────────
    // TS: resolved tools show ONLY the header (green/red dot).  The actual
    // tool output appears as a separate tool_result row below (rendered via
    // message_tool_result.cppm with the ⎿ MessageResponse connector).
    // Unresolved tools show progress text below the header.
    if (!is_resolved) {
        if (data.status == FaithfulToolStatus::Running) {
            if (!data.progress_text.empty()) {
                column.push_back(hbox({
                    text("  \xe2\x8e\xbf  ") | dim,
                    text(data.progress_text) | dim,
                }));
            }
        } else if (data.status == FaithfulToolStatus::Queued) {
            if (!data.queued_text.empty()) {
                column.push_back(hbox({
                    text("  \xe2\x8e\xbf  ") | dim,
                    text(data.queued_text) | dim,
                }));
            }
        }
    }

    Element body = vbox(std::move(column));

    // Outer container: flexDirection="row", justifyContent="space-between",
    // width="100%", marginTop={addMargin?1:0}
    Element outer = hbox({body, filler()}) | flex;
    if (data.add_margin) {
        return vbox({text(""), std::move(outer)});
    }
    return outer;
}

} // namespace cc::ui::messages::tool_use_message
