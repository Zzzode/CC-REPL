/// @file local_command_output_message.cppm
/// @brief User-local-command output viewer — left gutter with gray right-aligned
/// line numbers, red stderr highlight, >200-line fold with middle-hide marker,
/// top command-line header with colorized exit code + elapsed, and copy-all
/// footer button.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.local_command_output_message;

export namespace cc::ui::messages::local_cmd {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Which stream produced a given output line
enum class StreamKind : std::uint8_t {
    Stdout,
    Stderr,
    StdinEcho,     // The command line itself echoed back
};

/// One line of interleaved command output
struct OutputLine {
    StreamKind kind{StreamKind::Stdout};
    std::string text;
};

/// The full data model for a local-command invocation
struct LocalCommandData {
    std::string command_line;        // "ls -la foo/"
    std::string working_dir;         // Optional CWD banner
    std::vector<OutputLine> lines;   // Interleaved stdout / stderr
    std::optional<int> exit_code;    // nullopt = still running
    std::chrono::milliseconds elapsed_ms{0};
    std::optional<std::size_t> total_bytes; // Total bytes emitted
    bool is_running{false};

    // Collapsing state
    bool collapsed{false};           // Top-level fold
    int head_lines{100};             // Top kept when hiding
    int tail_lines{100};             // Bottom kept when hiding
    std::size_t collapse_threshold{200}; // >200 lines → show fold marker
};

/// Component options
struct LocalCommandOptions {
    LocalCommandData data;
    bool show_line_numbers{true};
    bool show_stderr_red{true};
    bool wrap_long_lines{false};
    std::function<void()> on_copy_all;
    std::function<void()> on_copy_cmdline;
    std::function<void()> on_toggle_collapse;
    std::function<void()> on_scroll_up;
    std::function<void()> on_scroll_down;
};

// ============================================================
// Helpers
// ============================================================

[[nodiscard]] inline const char* exit_status_icon(std::optional<int> code) {
    if (!code) return "⏳";
    return *code == 0 ? "✓" : "✗";
}

[[nodiscard]] inline Color exit_status_color(std::optional<int> code) {
    if (!code) return Color::Cyan;
    return *code == 0 ? Color::Green : Color::Red;
}

[[nodiscard]] inline std::string format_duration(std::chrono::milliseconds ms) {
    auto n = ms.count();
    if (n >= 60 * 1000LL) {
        return std::format("{:.1f}m", n / 60000.0);
    }
    if (n >= 1000) {
        return std::format("{:.2f}s", n / 1000.0);
    }
    return std::format("{}ms", n);
}

[[nodiscard]] inline std::string format_bytes(std::size_t n) {
    if (n >= 1024ULL * 1024) return std::format("{:.1f}MB", n / (1024.0 * 1024.0));
    if (n >= 1024) return std::format("{:.1f}KB", n / 1024.0);
    return std::format("{}B", n);
}

// ============================================================
// Header
// ============================================================

[[nodiscard]] inline Element RenderCommandHeader(const LocalCommandData& d) {
    Elements left;
    left.push_back(text(std::string{"$ "}) | color(Color::Green) | bold);
    left.push_back(text(d.command_line) | color(Color::White) | bold);

    Elements right;
    if (d.is_running) {
        right.push_back(text(" ⏳ running ") | color(Color::Cyan));
    }
    if (d.exit_code) {
        right.push_back(
            text(std::format(" {} exit {}",
                             exit_status_icon(d.exit_code),
                             *d.exit_code))
            | bold | color(exit_status_color(d.exit_code)));
    }
    if (d.elapsed_ms.count() > 0) {
        right.push_back(text(std::format(" · {} ", format_duration(d.elapsed_ms)))
                        | dim);
    }
    if (d.total_bytes) {
        right.push_back(text(std::format("· {} ", format_bytes(*d.total_bytes)))
                        | dim);
    }
    // Copy cmdline hint
    right.push_back(text("[x] copy cmd") | dim | color(Color::GrayDark));

    return hbox({
        hbox(left),
        filler(),
        hbox(right),
    });
}

// ============================================================
// Body — lines with line numbers + stderr highlight + middle hide
// ============================================================

[[nodiscard]] inline Element RenderOutputBody(const LocalCommandOptions& opts) {
    const auto& d = opts.data;
    const std::size_t total = d.lines.size();

    if (total == 0) {
        return text("  (no output)") | dim;
    }

    int gutter_w = 0;
    if (opts.show_line_numbers) {
        gutter_w = static_cast<int>(std::format("{}", total).size());
        gutter_w = std::max(2, gutter_w);
    }

    // Compute indices to render (respect collapse)
    std::vector<std::size_t> indices;
    bool show_fold_marker = false;
    std::size_t hidden = 0;
    if (d.collapsed && total > d.collapse_threshold) {
        const auto head = std::min<std::size_t>(d.head_lines, total);
        const auto tail = std::min<std::size_t>(d.tail_lines, total - head);
        for (std::size_t i = 0; i < head; ++i) indices.push_back(i);
        if (tail > 0 && head + tail < total) {
            show_fold_marker = true;
            hidden = total - head - tail;
            for (std::size_t i = total - tail; i < total; ++i) indices.push_back(i);
        } else if (head < total) {
            show_fold_marker = true;
            hidden = total - head;
        }
    } else {
        indices.reserve(total);
        for (std::size_t i = 0; i < total; ++i) indices.push_back(i);
    }

    Elements rows;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto idx = indices[i];

        // If we need to insert the "…N hidden…" marker between head and tail.
        if (show_fold_marker && i > 0 &&
            indices[i - 1] + 1 != idx) {
            std::string mark = std::format(
                "  ⋮  ({} lines hidden — press [f] to unfold)", hidden);
            rows.push_back(text(mark) | dim | color(Color::GrayDark));
        }

        const auto& line = d.lines[idx];
        Elements parts;
        if (opts.show_line_numbers) {
            auto num_str = std::format("{:>{}} │", idx + 1, gutter_w);
            parts.push_back(text(num_str) | color(Color::GrayDark));
        }

        // Actual line content
        std::string text_body = line.text.empty() ? std::string(" ") : line.text;
        Element el = text(text_body);
        switch (line.kind) {
            case StreamKind::Stderr:
                el = el | color(opts.show_stderr_red ? Color::Red : Color::GrayLight);
                break;
            case StreamKind::StdinEcho:
                el = el | color(Color::Cyan) | dim;
                break;
            case StreamKind::Stdout:
            default:
                el = el | color(Color::GrayLight);
                break;
        }
        parts.push_back(std::move(el));
        rows.push_back(hbox(parts));
    }

    // Always show fold marker when collapsed and head alone reached threshold
    if (show_fold_marker && !d.collapsed) {
        // no-op — covered above already
    }

    return vbox(rows);
}

// ============================================================
// Footer
// ============================================================

[[nodiscard]] inline Element RenderOutputFooter(const LocalCommandOptions& opts) {
    const auto& d = opts.data;
    const std::size_t total = d.lines.size();

    Elements left;
    left.push_back(text(" ") | dim);
    if (total > d.collapse_threshold) {
        if (d.collapsed) {
            left.push_back(text("[f] unfold ") | color(Color::BlueLight) | dim);
        } else {
            left.push_back(text("[f] fold ") | color(Color::BlueLight) | dim);
        }
    }
    left.push_back(text("[c] copy all ") | dim);

    Elements right;
    right.push_back(text(std::format(" {} line{}", total, total == 1 ? "" : "s"))
                    | dim | color(Color::GrayDark));
    if (d.exit_code) {
        right.push_back(text(std::format("  exit {}", *d.exit_code))
                        | color(exit_status_color(d.exit_code)) | dim);
    }

    return hbox({hbox(left), filler(), hbox(right)});
}

// ============================================================
// Top-level renderer
// ============================================================

[[nodiscard]] inline Element RenderLocalCommandOutput(
    const LocalCommandOptions& opts) {
    const auto& d = opts.data;

    Elements parts;
    // Working dir banner
    if (!d.working_dir.empty()) {
        parts.push_back(hbox({
            text("in ") | dim | color(Color::GrayDark),
            text(d.working_dir) | dim | color(Color::Blue),
        }));
    }
    parts.push_back(RenderCommandHeader(d));
    parts.push_back(separator());
    parts.push_back(RenderOutputBody(opts));
    parts.push_back(separator());
    parts.push_back(RenderOutputFooter(opts));

    Color border = d.exit_code && *d.exit_code != 0
        ? Color::Red
        : (d.is_running ? Color::Cyan : Color::GrayDark);
    return vbox(parts) | borderRounded | color(border);
}

// ============================================================
// Faithful renderer — matches TS UserLocalCommandOutputMessage.tsx
// ============================================================

// Render cloud-launch content (diamond-prefixed: ◇ or ◆).
// TS REF: src/components/messages/UserLocalCommandOutputMessage.tsx:88-166 (CloudLaunchContent)
// TS structure:
//   <Text color="background">{diamond} </Text>   // hidden diamond
//   <Text bold>{label}</Text>                     // bold command name
//   {suffix && <Text dimColor>{suffix}</Text>}    // dim " · extra"
//   {rest && (                                    // body after \n
//     <Box flexDirection="row">
//       <Text dimColor>{"  ⎿  "}</Text>
//       <Text dimColor>{rest}</Text>
//     </Box>)}
[[nodiscard]] inline Element RenderCloudLaunchFaithful(
    const std::vector<OutputLine>& lines) {
    if (lines.empty()) return text("") | dim;

    // Parse first line: "◆ label · suffix" possibly followed by "\nrest..."
    const std::string& first_text = lines[0].text;

    // Extract diamond (first UTF-8 char, ◇=U+25C7 or ◆=U+25C6, both 3 bytes in UTF-8).
    // Diamond is hidden in TS (color="background"), so we only skip its bytes.
    std::string after_diamond;
    if (first_text.size() >= 3 &&
        (static_cast<unsigned char>(first_text[0]) == 0xE2)) {
        // Valid 3-byte UTF-8 for ◇ (E2 97 87) or ◆ (E2 97 86)
        // Skip past diamond + space if present
        std::size_t pos = 3;
        if (pos < first_text.size() && first_text[pos] == ' ') ++pos;
        after_diamond = first_text.substr(pos);
    } else {
        after_diamond = first_text;
    }

    // Split header from rest (after first newline)
    std::string header_part = after_diamond;
    std::string rest_part;
    auto nl_pos = after_diamond.find('\n');
    if (nl_pos != std::string::npos) {
        header_part = after_diamond.substr(0, nl_pos);
        rest_part = after_diamond.substr(nl_pos + 1);
    }

    // Also collect rest from subsequent lines
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (!rest_part.empty()) rest_part += '\n';
        rest_part += lines[i].text;
    }

    // Parse label and suffix (separator: " · " middle dot = UTF-8 C2 B7 + space)
    std::string label = header_part;
    std::string suffix;
    // TS: header.indexOf(" \xB7 ") — middle dot U+00B7 = UTF-8 "\xC2\xB7"
    const std::string mid_dot = " \xC2\xB7 ";  // " · "
    auto sep_pos = header_part.find(mid_dot);
    if (sep_pos != std::string::npos) {
        label = header_part.substr(0, sep_pos);
        suffix = header_part.substr(sep_pos);
    }

    // Build header line
    Elements header_parts;
    // TS: <Text color="background">{diamond} </Text> — diamond is hidden (background color).
    // FTXUI has no Color::Background; the diamond is intentionally invisible, so omit it.
    // TS: <Text bold>{label}</Text>
    header_parts.push_back(text(label) | bold);
    if (!suffix.empty()) {
        // TS: <Text dimColor>{suffix}</Text>
        header_parts.push_back(text(suffix) | dim);
    }

    Element header = hbox(std::move(header_parts));

    // Build body rest
    if (!rest_part.empty()) {
        // Trim trailing whitespace per TS: children.slice(nl + 1).trim()
        // Simple trim
        auto start = rest_part.find_first_not_of(" \t\n\r");
        auto end = rest_part.find_last_not_of(" \t\n\r");
        if (start != std::string::npos) {
            rest_part = rest_part.substr(start, end - start + 1);
        } else {
            rest_part.clear();
        }
    }

    if (!rest_part.empty()) {
        Element rest_line = hbox({
            text("  ⎿  ") | dim,                          // ⎿ prefix dim
            paragraph(rest_part) | dim,                        // rest dim
        });
        return vbox({header, rest_line});
    }

    return header;
}

[[nodiscard]] inline Element RenderLocalCommandOutputFaithful(
    const LocalCommandOptions& opts) {
    // TS REF: src/components/messages/UserLocalCommandOutputMessage.tsx:12-54
    // Faithful variant: no line numbers, no borders, just ⎿ prefix + ANSI passthrough.
    // TS extracts <local-command-stdout> / <local-command-stderr> blocks; each
    // non-empty trimmed block renders as IndentedContent.
    //
    // TS IndentedContent (normal path):
    //   <Text dimColor>{"  ⎿  "}</Text>          ← dim prefix only
    //   <Markdown>{children}</Markdown>          ← ANSI passthrough, NOT dim
    //
    // TS does NOT dim content body and does NOT color stderr red in normal path.

    const auto& lines = opts.data.lines;

    // TS REF: src/components/messages/UserLocalCommandOutputMessage.tsx:24-33
    // If neither stdout nor stderr, show NO_CONTENT_MESSAGE = "(no content)" dimColor.
    if (lines.empty()) {
        return text("(no content)") | dim;
    }

    // Check for cloud-launch diamond prefix on first output line.
    // TS REF: src/components/messages/UserLocalCommandOutputMessage.tsx:60-70
    // (IndentedContent startsWith DIAMOND_OPEN / DIAMOND_FILLED check)
    if (!lines.empty()) {
        const auto& t = lines[0].text;
        if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xE2 &&
            (static_cast<unsigned char>(t[1]) == 0x97)) {
            // E2 97 86 = ◆ (U+25C6), E2 97 87 = ◇ (U+25C7)
            return RenderCloudLaunchFaithful(lines);
        }
    }

    // Normal path: build content lines, each rendered via paragraph (ANSI passthrough).
    // TS REF: src/components/messages/UserLocalCommandOutputMessage.tsx:71-87
    Elements content;
    content.reserve(lines.size());
    for (const auto& line : lines) {
        // TS does NOT dim content and does NOT color stderr.
        // paragraph() = ANSI passthrough, equivalent to TS <Markdown>.
        content.push_back(paragraph(line.text.empty() ? " " : line.text));
    }

    return hbox({
        text("  ⎿  ") | dim,                          // ⎿ prefix — dim only
        vbox(std::move(content)) | flex,
    });
}

// ============================================================
// Interactive Component
// ============================================================

[[nodiscard]] inline Component LocalCommandOutput(LocalCommandOptions options) {
    struct State {
        LocalCommandOptions opts;
    };
    auto s = std::make_shared<State>();
    s->opts = std::move(options);

    return Renderer([s] { return RenderLocalCommandOutput(s->opts); })
        | CatchEvent([s](Event event) -> bool {
              auto& o = s->opts;
              auto& d = o.data;

              // Copy all
              if (event == Event::Character('c') || event == Event::Character('C')) {
                  if (o.on_copy_all) { o.on_copy_all(); return true; }
              }
              // Copy command line
              if (event == Event::Character('x') || event == Event::Character('X')) {
                  if (o.on_copy_cmdline) { o.on_copy_cmdline(); return true; }
              }
              // Fold / unfold middle
              if (event == Event::Character('f') || event == Event::Character('F')) {
                  if (d.lines.size() > d.collapse_threshold) {
                      d.collapsed = !d.collapsed;
                      if (o.on_toggle_collapse) o.on_toggle_collapse();
                      return true;
                  }
              }
              // Scroll (pass through via callbacks; optional)
              if (event == Event::PageDown || event == Event::Character('J')) {
                  if (o.on_scroll_down) { o.on_scroll_down(); return true; }
              }
              if (event == Event::PageUp || event == Event::Character('K')) {
                  if (o.on_scroll_up) { o.on_scroll_up(); return true; }
              }
              // Space → toggle top-level collapse
              if (event == Event::Character(' ')) {
                  d.collapsed = !d.collapsed;
                  if (o.on_toggle_collapse) o.on_toggle_collapse();
                  return true;
              }
              return false;
          });
}

} // namespace cc::ui::messages::local_cmd
