/// @file copy_cmd.cppm
/// @brief CopyCommand implementing the /copy slash command.
/// Copy last response to clipboard, extract code blocks, export as
/// markdown/json, write to a specific path. Supports OSC 52 clipboard over SSH.
module;

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <string_view>

export module cc.commands.copy_cmd;

import cc.types.types;
import cc.commands.command;
import cc.utils.bash_execution;

export namespace cc::commands {

using namespace cc::core;

// ============================================================
// Pure helpers (formatting / data prep)
// ============================================================

/// A single fenced code block extracted from markdown text.
struct CodeBlock {
    std::string code;      /// Content between the fences (no fences)
    std::string lang;      /// Language identifier (may be empty)
};

/// Extract fenced code blocks from markdown-like text.
/// Simple `` ``` `` fence parser — no external dependency.
[[nodiscard]] inline std::vector<CodeBlock> extract_code_blocks(std::string_view markdown) {
    std::vector<CodeBlock> blocks;
    enum class State { Outside, Inside } state = State::Outside;
    std::size_t i = 0;
    std::string current_lang;
    std::string current_code;

    while (i < markdown.size()) {
        // Look for a triple backtick at the start of a line
        const bool line_start = (i == 0) || (markdown[i - 1] == '\n');
        if (line_start && i + 2 < markdown.size() &&
            markdown[i] == '`' && markdown[i + 1] == '`' && markdown[i + 2] == '`') {
            i += 3;
            if (state == State::Outside) {
                // Opening fence: read optional language until newline
                state = State::Inside;
                current_lang.clear();
                current_code.clear();
                while (i < markdown.size() && markdown[i] != '\n' && markdown[i] != '\r') {
                    current_lang.push_back(markdown[i]);
                    ++i;
                }
                // Trim trailing whitespace from lang
                while (!current_lang.empty() &&
                       (current_lang.back() == ' ' || current_lang.back() == '\t')) {
                    current_lang.pop_back();
                }
                // Skip the newline
                if (i < markdown.size() && markdown[i] == '\r') ++i;
                if (i < markdown.size() && markdown[i] == '\n') ++i;
            } else {
                // Closing fence: record block, skip rest of line
                state = State::Outside;
                // Trim trailing newline(s) from code
                while (!current_code.empty() && current_code.back() == '\n') {
                    current_code.pop_back();
                }
                blocks.push_back({std::move(current_code), std::move(current_lang)});
                // Skip to end of line
                while (i < markdown.size() && markdown[i] != '\n') ++i;
                if (i < markdown.size() && markdown[i] == '\n') ++i;
            }
            continue;
        }

        if (state == State::Inside) {
            current_code.push_back(markdown[i]);
        }
        ++i;
    }

    return blocks;
}

/// Format a collection of messages (role/content pairs) as a single Markdown string.
[[nodiscard]] inline std::string format_for_copy_as_markdown(
    std::span<const std::pair<std::string_view, std::string_view>> role_content_pairs) {
    std::string out;
    for (const auto& [role, content] : role_content_pairs) {
        if (!out.empty()) out += "\n\n---\n\n";
        out += std::format("## {}\n\n{}", role, content);
    }
    return out;
}

/// Format a collection of messages as a compact JSON array.
[[nodiscard]] inline std::string format_for_copy_as_json(
    std::span<const std::pair<std::string_view, std::string_view>> role_content_pairs) {
    std::string out = "[\n";
    bool first = true;
    for (const auto& [role, content] : role_content_pairs) {
        if (!first) out += ",\n";
        first = false;

        // Minimal JSON escape for content and role strings
        auto escape = [](std::string_view s) -> std::string {
            std::string e;
            e.reserve(s.size());
            for (unsigned char ch : s) {
                switch (ch) {
                    case '"':  e += "\\\""; break;
                    case '\\': e += "\\\\"; break;
                    case '\b': e += "\\b";  break;
                    case '\f': e += "\\f";  break;
                    case '\n': e += "\\n";  break;
                    case '\r': e += "\\r";  break;
                    case '\t': e += "\\t";  break;
                    default:
                        if (ch < 0x20) {
                            e += std::format("\\u{:04x}", ch);
                        } else {
                            e.push_back(static_cast<char>(ch));
                        }
                        break;
                }
            }
            return e;
        };

        out += std::format(R"(  {{"role": "{}", "content": "{}"}})",
                           escape(role), escape(content));
    }
    out += "\n]";
    return out;
}

/// Clipboard method used for copying
enum class ClipboardMethod : std::uint8_t {
    Native,     // pbcopy/xclip/wl-copy
    OSC52,      // Terminal escape sequence (works over SSH)
};

/// Destination for the copy operation.
enum class CopyDestination : std::uint8_t {
    Clipboard,    // System clipboard (or OSC 52)
    Stdout,       // Print to stdout
    FilePath,     // Write to a specific path
    TempFile,     // Write to a temp file (fallback for OSC 52)
};

/// Parsed sub-command target for /copy.
enum class CopyTargetKind : std::uint8_t {
    Last,             // Last assistant message (default)
    MessageIndex,     // Specific message by 1-based index
    CodeBlock,        // N-th code block within the last message
    AllAsMarkdown,    // All recorded responses, combined as Markdown
    AllAsJson,        // All recorded responses, combined as JSON
};

struct CopyTarget {
    CopyTargetKind kind = CopyTargetKind::Last;
    std::size_t index = 0;                  // For MessageIndex / CodeBlock
    std::optional<std::string> file_path;   // For FilePath destination
};

// ============================================================
// CopyCommand
// ============================================================

/// CopyCommand implements the /copy slash command.
/// Copies conversation content to the system clipboard, stdout, or a file.
class CopyCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "copy",
            .description = "Copy response content to clipboard or file",
            .args = {
                CommandArg{.name = "subcommand", .description =
                    "last | message N | codeblock N | all-as-markdown | all-as-json | path PATH",
                    .type = ArgType::Text, .required = false},
                CommandArg{.name = "--osc52", .description = "Force OSC 52 clipboard method",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--stdout", .description = "Print to stdout instead of clipboard",
                           .type = ArgType::None, .required = false},
            },
            .category = "tools",
            .aliases = {"cp"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto method = detect_clipboard_method(ctx.args);
        auto dest   = detect_destination(ctx.args);
        auto target = parse_target(ctx.args);

        if (dest == CopyDestination::FilePath && !target.file_path) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput,
                "Usage: /copy path <file_path>"));
        }

        auto [content, label] = resolve_content(target);
        if (content.empty()) {
            return CommandResult::success("Nothing to copy. No assistant messages in this session.");
        }

        std::string result_msg;
        if (dest == CopyDestination::Clipboard) {
            auto r = copy_to_clipboard(content, method);
            if (!r) return std::unexpected(r.error());
            result_msg = std::format("Copied {} ({} bytes, {} method)",
                label, content.size(),
                method == ClipboardMethod::OSC52 ? "OSC 52" : "native");
            // Also write to a temp file for reliability
            if (auto p = write_temp_file(content, "response.md"); !p.empty()) {
                result_msg += std::format("\nAlso written to {}", p);
            }
        } else if (dest == CopyDestination::FilePath) {
            auto r = write_file(*target.file_path, content);
            if (!r) return std::unexpected(r.error());
            result_msg = std::format("Written {} to: {} ({} bytes)",
                label, *target.file_path, content.size());
        } else {
            // Stdout
            return CommandResult::success(std::move(content));
        }

        return CommandResult::success(std::move(result_msg));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"last", "message", "codeblock", "all-as-markdown", "all-as-json",
                       "path", "--osc52", "--stdout"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Record an assistant response text (called after each assistant turn).
    void record_response(std::string content) {
        responses_.push_back(std::move(content));
    }

private:
    std::vector<std::string> responses_;

    // ---- flag / arg parsing ----

    [[nodiscard]] static ClipboardMethod detect_clipboard_method(std::span<const std::string> args) {
        for (const auto& a : args) if (a == "--osc52") return ClipboardMethod::OSC52;
        return ClipboardMethod::Native;
    }

    [[nodiscard]] static CopyDestination detect_destination(std::span<const std::string> args) {
        for (const auto& a : args) {
            if (a == "--stdout") return CopyDestination::Stdout;
            if (a == "path")     return CopyDestination::FilePath;
        }
        return CopyDestination::Clipboard;
    }

    [[nodiscard]] static CopyTarget parse_target(std::span<const std::string> args) {
        CopyTarget t;
        // Strip flags
        std::vector<std::string> positional;
        positional.reserve(args.size());
        for (const auto& a : args) {
            if (a.starts_with("-")) continue;
            positional.push_back(a);
        }

        if (positional.empty()) {
            t.kind = CopyTargetKind::Last;
            return t;
        }

        const auto& first = positional[0];
        if (first == "last") {
            t.kind = CopyTargetKind::Last;
        } else if (first == "all-as-markdown") {
            t.kind = CopyTargetKind::AllAsMarkdown;
        } else if (first == "all-as-json") {
            t.kind = CopyTargetKind::AllAsJson;
        } else if (first == "message" && positional.size() >= 2) {
            t.kind = CopyTargetKind::MessageIndex;
            t.index = parse_size(positional[1], 1) - 1; // 1-based user input
        } else if (first == "codeblock" && positional.size() >= 2) {
            t.kind = CopyTargetKind::CodeBlock;
            t.index = parse_size(positional[1], 1) - 1;
        } else if (first == "path" && positional.size() >= 2) {
            t.kind = CopyTargetKind::Last; // content is still "last"
            t.file_path = positional[1];
        } else {
            // Bare number: treat as message index
            auto n = parse_size(first, 0);
            if (n > 0) {
                t.kind = CopyTargetKind::MessageIndex;
                t.index = n - 1;
            } else {
                t.kind = CopyTargetKind::Last;
            }
        }
        return t;
    }

    [[nodiscard]] static std::size_t parse_size(std::string_view s, std::size_t fallback) {
        std::size_t value = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec != std::errc{} || ptr != s.data() + s.size()) return fallback;
        return value;
    }

    // ---- content resolution ----

    /// Resolve a target -> (content, human-readable label describing what was copied).
    [[nodiscard]] std::pair<std::string, std::string> resolve_content(const CopyTarget& target) const {
        if (responses_.empty()) return {"", "nothing"};

        switch (target.kind) {
            case CopyTargetKind::Last: {
                return {responses_.back(), "last assistant message"};
            }
            case CopyTargetKind::MessageIndex: {
                if (target.index >= responses_.size()) {
                    return {responses_.back(),
                            std::format("message {} (out of range, using last)", target.index + 1)};
                }
                return {responses_[target.index],
                        std::format("assistant message #{}", target.index + 1)};
            }
            case CopyTargetKind::CodeBlock: {
                auto blocks = extract_code_blocks(responses_.back());
                if (blocks.empty()) return {"", "no code blocks found"};
                if (target.index >= blocks.size()) {
                    return {blocks.back().code,
                            std::format("code block {} (out of range, using last)",
                                        target.index + 1)};
                }
                return {blocks[target.index].code,
                        std::format("code block #{} ({})",
                                    target.index + 1,
                                    blocks[target.index].lang.empty()
                                        ? std::string{"plain"}
                                        : blocks[target.index].lang)};
            }
            case CopyTargetKind::AllAsMarkdown: {
                std::vector<std::pair<std::string_view, std::string_view>> pairs;
                pairs.reserve(responses_.size());
                for (std::size_t i = 0; i < responses_.size(); ++i) {
                    pairs.emplace_back("assistant", responses_[i]);
                }
                return {format_for_copy_as_markdown(pairs),
                        std::format("{} messages as Markdown", responses_.size())};
            }
            case CopyTargetKind::AllAsJson: {
                std::vector<std::pair<std::string_view, std::string_view>> pairs;
                pairs.reserve(responses_.size());
                for (std::size_t i = 0; i < responses_.size(); ++i) {
                    pairs.emplace_back("assistant", responses_[i]);
                }
                return {format_for_copy_as_json(pairs),
                        std::format("{} messages as JSON", responses_.size())};
            }
        }
        return {"", "unknown"};
    }

    // ---- clipboard backends ----

    [[nodiscard]] static VoidResult copy_to_clipboard(
        const std::string& content, ClipboardMethod method) {
        if (method == ClipboardMethod::OSC52) return copy_osc52(content);
        return copy_native(content);
    }

    [[nodiscard]] static VoidResult copy_osc52(const std::string& content) {
        static constexpr std::array<char, 64> b64_table = {
            'A','B','C','D','E','F','G','H','I','J','K','L','M',
            'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
            'a','b','c','d','e','f','g','h','i','j','k','l','m',
            'n','o','p','q','r','s','t','u','v','w','x','y','z',
            '0','1','2','3','4','5','6','7','8','9','+','/'
        };
        std::string b64;
        b64.reserve(((content.size() + 2) / 3) * 4);
        auto src = reinterpret_cast<const unsigned char*>(content.data());
        auto len = content.size();
        for (std::size_t i = 0; i < len; i += 3) {
            uint32_t triple = static_cast<uint32_t>(src[i]) << 16;
            if (i + 1 < len) triple |= static_cast<uint32_t>(src[i + 1]) << 8;
            if (i + 2 < len) triple |= static_cast<uint32_t>(src[i + 2]);
            b64 += b64_table[(triple >> 18) & 0x3F];
            b64 += b64_table[(triple >> 12) & 0x3F];
            b64 += (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
            b64 += (i + 2 < len) ? b64_table[triple & 0x3F] : '=';
        }
        std::fprintf(stdout, "\033]52;c;%s\a", b64.c_str());
        std::fflush(stdout);
        return {};
    }

    [[nodiscard]] static VoidResult copy_native(const std::string& content) {
        const char* cmd = nullptr;
#if defined(__APPLE__)
        cmd = "pbcopy";
#elif defined(_WIN32)
        cmd = "clip.exe";
#elif defined(__linux__)
        if (std::getenv("WAYLAND_DISPLAY") != nullptr) {
            cmd = "wl-copy";
        } else {
            cmd = "xclip -selection clipboard";
        }
#else
        return std::unexpected(Error::make(
            ErrorCode::InternalError, "Clipboard not supported on this platform"));
#endif
        auto wr = cc::utils::bash::exec_write(cmd, content);
        if (!wr) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Failed to open clipboard command: {}", cmd)));
        }
        int status = *wr;
        if (status != 0) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Clipboard command exited with status {}", status)));
        }
        return {};
    }

    // ---- file backends ----

    [[nodiscard]] static VoidResult write_file(const std::string& path, const std::string& content) {
        namespace fs = std::filesystem;
        auto parent = fs::path(path).parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            std::error_code ec;
            fs::create_directories(parent, ec);
            if (ec) return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                std::format("Failed to create directory '{}': {}", parent.string(), ec.message())));
        }
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
            std::format("Failed to open file for writing: {}", path)));
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs.good()) return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
            std::format("Failed to write to file: {}", path)));
        return {};
    }

    /// Write to temp dir/filename; returns the absolute path or "" on failure.
    [[nodiscard]] static std::string write_temp_file(const std::string& content,
                                                     std::string_view filename) {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto tmp_dir = fs::temp_directory_path(ec) / "cc-repl";
        if (ec) return "";
        fs::create_directories(tmp_dir, ec);
        if (ec) return "";
        auto p = tmp_dir / filename;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return "";
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs.good()) return "";
        return p.string();
    }
};

} // namespace cc::commands
