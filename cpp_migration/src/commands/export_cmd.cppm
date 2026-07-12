/// @file export_cmd.cppm
/// @brief ExportCommand implementing the /export slash command.
/// Export conversation in Markdown / JSON / JSONL / Transcript formats,
/// with filters (system/tool/thinking/user+assistant), truncation options,
/// and destinations (file / stdout / clipboard). Reuses Message types from
/// cc.types.types (FTXUI rendering DEFERRED to Phase 4).
module;

#include <cstdint>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <string_view>

export module cc.commands.export_cmd;

import cc.types.types;
import cc.commands.command;
import cc.utils.bash_execution;

export namespace cc::commands {

using namespace cc::core;

// ============================================================
// Export formats
// ============================================================

enum class ExportFormat : std::uint8_t {
    Markdown,
    Json,
    Jsonl,
    Transcript,
};

/// Destination for the export output.
enum class ExportDestination : std::uint8_t {
    File,      // Write to a file (default)
    Stdout,    // Print to stdout
    Clipboard, // Copy to system clipboard
};

/// What kinds of messages to include in the export.
struct ExportFilters {
    bool include_system    = false;
    bool include_tool      = false;   // tool_use + tool_result
    bool include_thinking  = false;   // thinking blocks within assistant messages
    bool user_assistant_only = true;  // if true, only user+assistant (overrides above)
};

/// Truncation / sizing options.
struct ExportTruncation {
    std::optional<std::size_t> max_messages;        // total message cap
    std::optional<std::size_t> max_chars_per_message;
    bool full = false;                              // no truncation at all
};

/// Complete set of export options.
struct ExportOptions {
    ExportFormat format = ExportFormat::Markdown;
    ExportFilters filters{};
    ExportTruncation truncation{};
    ExportDestination destination = ExportDestination::File;
    std::optional<std::string> output_path;
};

// ============================================================
// Pure helpers (formatting / data prep)
// ============================================================

/// Format a system_clock timestamp as "YYYY-MM-DD-HHMMSS".
[[nodiscard]] inline std::string format_timestamp(
    std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d-%H%M%S");
    return oss.str();
}

/// Extract the first user prompt's text (first line, max ~50 chars).
/// Returns "" if no user message with text content is found.
[[nodiscard]] inline std::string extract_first_prompt(std::span<const Message> messages) {
    for (const auto& msg : messages) {
        if (std::holds_alternative<UserMessage>(msg)) {
            const auto& um = std::get<UserMessage>(msg);
            // Find first TextBlock
            for (const auto& block : um.content) {
                if (std::holds_alternative<TextBlock>(block)) {
                    std::string text = std::get<TextBlock>(block).text;
                    // Trim leading/trailing whitespace
                    auto start = text.find_first_not_of(" \t\n\r");
                    auto end = text.find_last_not_of(" \t\n\r");
                    if (start == std::string::npos) return "";
                    text = text.substr(start, end - start + 1);
                    // Take first line only
                    auto nl = text.find('\n');
                    if (nl != std::string::npos) text = text.substr(0, nl);
                    if (text.size() > 50) {
                        text = text.substr(0, 49) + "\u2026";
                    }
                    return text;
                }
            }
            return "";
        }
    }
    return "";
}

/// Sanitize a string for use as a filename (lowercase, replace non-alnum with hyphens).
[[nodiscard]] inline std::string sanitize_filename(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool prev_hyphen = false;
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            prev_hyphen = false;
        } else if (ch == ' ' || ch == '-' || ch == '_') {
            if (!prev_hyphen && !out.empty()) {
                out.push_back('-');
                prev_hyphen = true;
            }
        }
    }
    // Trim trailing hyphens
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

/// Escape a string for inclusion in a JSON string literal.
[[nodiscard]] inline std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b";  break;
            case '\f': escaped += "\\f";  break;
            case '\n': escaped += "\\n";  break;
            case '\r': escaped += "\\r";  break;
            case '\t': escaped += "\\t";  break;
            default:
                if (ch < 0x20) {
                    escaped += std::format("\\u{:04x}", ch);
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

// ============================================================
// Message content extraction (pure, from Message variant)
// ============================================================

/// Extract all text from a Message's content blocks.
/// If include_thinking is false, thinking blocks are skipped.
[[nodiscard]] inline std::string extract_message_text(
    const Message& msg, bool include_thinking = false,
    std::optional<std::size_t> max_chars = std::nullopt) {
    return std::visit([&](const auto& m) -> std::string {
        std::string out;
        using T = std::remove_cvref_t<decltype(m)>;
        // Tool-use / tool-result messages carry their "payload" text directly
        if constexpr (std::is_same_v<T, ToolUseMessage>) {
            out = std::format("Tool: {}\nInput: {}", m.tool_name, m.tool_input_json);
        }
        // Iterate content blocks for every message type
        for (const auto& block : m.content) {
            if (std::holds_alternative<TextBlock>(block)) {
                if (!out.empty()) out += "\n";
                out += std::get<TextBlock>(block).text;
            } else if (std::holds_alternative<ToolUseBlock>(block)) {
                if (!out.empty()) out += "\n";
                const auto& tu = std::get<ToolUseBlock>(block);
                out += std::format("[Tool use: {}]", tu.name);
            } else if (std::holds_alternative<ToolResultBlock>(block)) {
                if (!out.empty()) out += "\n";
                const auto& tr = std::get<ToolResultBlock>(block);
                out += std::format("[Tool result: {}]", tool_result_content_text(tr));
            } else if (std::holds_alternative<ThinkingBlock>(block) && include_thinking) {
                if (!out.empty()) out += "\n";
                out += std::format("[Thinking]: {}",
                                   std::get<ThinkingBlock>(block).thinking);
            }
        }
        if (max_chars.has_value() && out.size() > *max_chars) {
            out.resize(*max_chars - 1);
            out += "\u2026";
        }
        return out;
    }, msg);
}

/// Decide whether a message passes the filters.
[[nodiscard]] inline bool message_passes_filters(const Message& msg,
                                                 const ExportFilters& filters) {
    Role role = get_role(msg);
    if (filters.user_assistant_only) {
        return role == Role::User || role == Role::Assistant;
    }
    switch (role) {
        case Role::User:
        case Role::Assistant:  return true;
        case Role::System:     return filters.include_system;
        case Role::Tool:       return filters.include_tool;
    }
    return false;
}

/// Apply truncation: drop oldest messages if > max_messages, apply per-message char cap.
[[nodiscard]] inline std::vector<const Message*> apply_truncation(
    std::span<const Message> messages, const ExportTruncation& trunc) {

    std::vector<const Message*> out;
    out.reserve(messages.size());
    for (const auto& m : messages) out.push_back(&m);

    if (!trunc.full) {
        if (trunc.max_messages.has_value() && out.size() > *trunc.max_messages) {
            // Keep newest N
            out.erase(out.begin(),
                      out.begin() + static_cast<std::ptrdiff_t>(
                          out.size() - *trunc.max_messages));
        }
    }
    return out;
}

// ============================================================
// Per-format formatters
// ============================================================

[[nodiscard]] inline std::string format_markdown(
    std::span<const Message* const> msgs, const ExportOptions& opts) {
    std::string out = "# Conversation Export\n\n";
    for (const auto* mp : msgs) {
        if (!message_passes_filters(*mp, opts.filters)) continue;
        Role role = get_role(*mp);
        auto text = extract_message_text(*mp, opts.filters.include_thinking,
                                         opts.truncation.max_chars_per_message);
        out += std::format("## {}\n\n{}\n\n", role_to_string(role), text);
    }
    return out;
}

[[nodiscard]] inline std::string format_json(
    std::span<const Message* const> msgs, const ExportOptions& opts) {
    std::string out = "[\n";
    bool first = true;
    for (const auto* mp : msgs) {
        if (!message_passes_filters(*mp, opts.filters)) continue;
        Role role = get_role(*mp);
        auto text = extract_message_text(*mp, opts.filters.include_thinking,
                                         opts.truncation.max_chars_per_message);
        if (!first) out += ",\n";
        first = false;
        out += std::format(R"(  {{"role": "{}", "content": "{}"}})",
                           role_to_string(role), json_escape(text));
    }
    out += "\n]";
    return out;
}

[[nodiscard]] inline std::string format_jsonl(
    std::span<const Message* const> msgs, const ExportOptions& opts) {
    std::string out;
    for (const auto* mp : msgs) {
        if (!message_passes_filters(*mp, opts.filters)) continue;
        Role role = get_role(*mp);
        auto text = extract_message_text(*mp, opts.filters.include_thinking,
                                         opts.truncation.max_chars_per_message);
        out += std::format(R"({{"role": "{}", "content": "{}"}})",
                           role_to_string(role), json_escape(text));
        out += "\n";
    }
    return out;
}

[[nodiscard]] inline std::string format_transcript(
    std::span<const Message* const> msgs, const ExportOptions& opts) {
    std::string out = "=== Conversation Transcript ===\n\n";
    for (const auto* mp : msgs) {
        if (!message_passes_filters(*mp, opts.filters)) continue;
        Role role = get_role(*mp);
        auto text = extract_message_text(*mp, opts.filters.include_thinking,
                                         opts.truncation.max_chars_per_message);
        auto ts = std::visit(
            [](const auto& m) { return m.timestamp; }, *mp);
        auto ts_str = format_timestamp(ts);
        std::string role_label(role_to_string(role));
        for (auto& c : role_label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        out += std::format("[{}] {}:\n{}\n\n", ts_str, role_label, text);
    }
    return out;
}

[[nodiscard]] inline std::string format_export(
    std::span<const Message* const> msgs, const ExportOptions& opts) {
    switch (opts.format) {
        case ExportFormat::Markdown:   return format_markdown(msgs, opts);
        case ExportFormat::Json:       return format_json(msgs, opts);
        case ExportFormat::Jsonl:      return format_jsonl(msgs, opts);
        case ExportFormat::Transcript: return format_transcript(msgs, opts);
    }
    return "";
}

// ============================================================
// Destination helpers (file write, stdout, clipboard)
// ============================================================

[[nodiscard]] inline VoidResult write_file(const std::string& path,
                                            const std::string& content) {
    namespace fs = std::filesystem;
    auto parent = fs::path(path).parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
            std::format("Failed to create directory '{}': {}",
                        parent.string(), ec.message())));
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
        std::format("Failed to open file for writing: {}", path)));
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs.good()) return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
        std::format("Failed to write to file: {}", path)));
    return {};
}

/// Platform-aware copy-to-clipboard (best-effort). Returns true on success.
[[nodiscard]] inline bool copy_to_clipboard_best_effort(const std::string& content) {
    const char* cmd = nullptr;
#if defined(__APPLE__)
    cmd = "pbcopy";
#elif defined(_WIN32)
    cmd = "clip.exe";
#elif defined(__linux__)
    if (std::getenv("WAYLAND_DISPLAY") != nullptr) cmd = "wl-copy";
    else                                          cmd = "xclip -selection clipboard";
#else
    return false;
#endif
    if (!cmd) return false;
    auto wr = cc::utils::bash::exec_write(cmd, content);
    return wr && *wr == 0;
}

// ============================================================
// ExportCommand
// ============================================================

/// ExportCommand implements the /export slash command.
/// Operates on a Message span injected via the runtime context
/// (set_messages called from the engine).
class ExportCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "export",
            .description = "Export conversation to Markdown / JSON / JSONL / Transcript",
            .args = {
                CommandArg{.name = "format", .description = "md|markdown|json|jsonl|transcript",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"md", "markdown", "json", "jsonl", "transcript"}},
                CommandArg{.name = "path", .description = "Output file path",
                           .type = ArgType::FilePath, .required = false},
                CommandArg{.name = "--system", .description = "Include system messages",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--include-tool", .description = "Include tool use/result",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--include-thinking", .description = "Include thinking blocks",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--only-user-assistant", .description = "Only user+assistant (default)",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--max-messages", .description = "Cap number of messages",
                           .type = ArgType::Number, .required = false},
                CommandArg{.name = "--max-chars", .description = "Cap chars per message",
                           .type = ArgType::Number, .required = false},
                CommandArg{.name = "--full", .description = "No truncation",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--stdout", .description = "Print to stdout",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--clipboard", .description = "Copy to clipboard",
                           .type = ArgType::None, .required = false},
            },
            .category = "session",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    /// Provide the conversation snapshot (called by the engine before execute).
    void set_messages(std::vector<Message> messages) {
        messages_ = std::move(messages);
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            // No arguments: trigger interactive export dialog via metadata.
            return CommandResult{
                true,
                "Opening export dialog...\nUI:export",
                std::string{"UI:export"},
                CommandStatus::Succeeded,
            };
        }

        auto opts = parse_options(ctx.args);
        if (messages_.empty()) {
            return CommandResult::success("No messages to export.");
        }
        auto filtered = apply_truncation(std::span{messages_}, opts.truncation);
        auto content = format_export(std::span{filtered}, opts);

        if (opts.destination == ExportDestination::Stdout) {
            return CommandResult::success(std::move(content));
        }
        if (opts.destination == ExportDestination::Clipboard) {
            bool ok = copy_to_clipboard_best_effort(content);
            return CommandResult::success(std::format(
                "Exported {} messages ({} bytes) to clipboard{}.",
                filtered.size(), content.size(),
                ok ? "" : " (best-effort copy failed — fall back to --stdout or --file)"));
        }

        // File destination
        auto path = resolve_output_path(opts);
        auto r = write_file(path, content);
        if (!r) return std::unexpected(r.error());

        std::string format_name;
        switch (opts.format) {
            case ExportFormat::Markdown:   format_name = "Markdown"; break;
            case ExportFormat::Json:       format_name = "JSON"; break;
            case ExportFormat::Jsonl:      format_name = "JSONL"; break;
            case ExportFormat::Transcript: format_name = "Transcript"; break;
        }
        return CommandResult::success(std::format(
            "Exported {} messages to: {}\nFormat: {}, Size: {} bytes",
            filtered.size(), path, format_name, content.size()));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"md", "markdown", "json", "jsonl", "transcript",
                       "--system", "--include-tool", "--include-thinking",
                       "--only-user-assistant", "--max-messages", "--max-chars",
                       "--full", "--stdout", "--clipboard"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

private:
    std::vector<Message> messages_;

    // ---- option parsing ----

    [[nodiscard]] static std::size_t parse_uint(std::string_view s, std::size_t fallback) {
        std::size_t v = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || ptr != s.data() + s.size()) return fallback;
        return v;
    }

    [[nodiscard]] static ExportOptions parse_options(std::span<const std::string> args) {
        ExportOptions opts;
        // Default: user+assistant only (no system, no tool, no thinking)
        opts.filters.user_assistant_only = true;

        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& a = args[i];
            if (a == "md" || a == "markdown") opts.format = ExportFormat::Markdown;
            else if (a == "json")             opts.format = ExportFormat::Json;
            else if (a == "jsonl")            opts.format = ExportFormat::Jsonl;
            else if (a == "transcript")       opts.format = ExportFormat::Transcript;
            else if (a == "--system")         { opts.filters.include_system = true; opts.filters.user_assistant_only = false; }
            else if (a == "--include-tool")   { opts.filters.include_tool = true;   opts.filters.user_assistant_only = false; }
            else if (a == "--include-thinking") { opts.filters.include_thinking = true; }
            else if (a == "--only-user-assistant") { opts.filters.user_assistant_only = true; }
            else if (a == "--full")           opts.truncation.full = true;
            else if (a == "--stdout")         opts.destination = ExportDestination::Stdout;
            else if (a == "--clipboard")      opts.destination = ExportDestination::Clipboard;
            else if (a == "--max-messages" && i + 1 < args.size()) {
                opts.truncation.max_messages = parse_uint(args[++i], 0);
                if (!*opts.truncation.max_messages) opts.truncation.max_messages.reset();
            } else if (a == "--max-chars" && i + 1 < args.size()) {
                opts.truncation.max_chars_per_message = parse_uint(args[++i], 0);
                if (!*opts.truncation.max_chars_per_message) opts.truncation.max_chars_per_message.reset();
            } else if (!a.starts_with("-")) {
                // Positional: path
                opts.output_path = a;
                opts.destination = ExportDestination::File;
            }
        }
        return opts;
    }

    [[nodiscard]] std::string resolve_output_path(const ExportOptions& opts) const {
        if (opts.output_path) return *opts.output_path;

        std::string ts = format_timestamp();
        std::string ext;
        switch (opts.format) {
            case ExportFormat::Markdown:   ext = "md"; break;
            case ExportFormat::Json:       ext = "json"; break;
            case ExportFormat::Jsonl:      ext = "jsonl"; break;
            case ExportFormat::Transcript: ext = "txt"; break;
        }
        auto first_prompt = extract_first_prompt(std::span{messages_});
        std::string base;
        if (!first_prompt.empty()) {
            auto s = sanitize_filename(first_prompt);
            base = s.empty() ? std::format("conversation-{}", ts)
                             : std::format("{}-{}", ts, s);
        } else {
            base = std::format("conversation-{}", ts);
        }
        return std::format("{}.{}", base, ext);
    }
};

} // namespace cc::commands
