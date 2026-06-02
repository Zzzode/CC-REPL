/// @file copy_cmd.cppm
/// @brief CopyCommand implementing the /copy slash command.
/// Copy last response to clipboard, copy specific message, OSC 52 clipboard support.
module;

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>

export module cc.commands.copy_cmd;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Clipboard method used for copying
enum class ClipboardMethod : std::uint8_t {
    Native,     // pbcopy/xclip/wl-copy
    OSC52,      // Terminal escape sequence (works over SSH)
};

/// CopyCommand implements the /copy slash command.
/// Copies conversation content to the system clipboard.
class CopyCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "copy",
            .description = "Copy response content to clipboard",
            .aliases = {"cp"},
            .args = {
                CommandArg{.name = "target", .description = "last | all | <message_index>",
                           .type = ArgType::Text, .required = false},
                CommandArg{.name = "--osc52", .description = "Force OSC 52 clipboard method",
                           .type = ArgType::None, .required = false},
            },
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto method = detect_clipboard_method(ctx.args);
        auto target = parse_target(ctx.args);

        std::string content = resolve_content(target);
        if (content.empty()) {
            return CommandResult::success("Nothing to copy. No assistant messages in this session.");
        }

        auto result = copy_to_clipboard(content, method);
        if (!result) return std::unexpected(result.error());

        auto bytes = content.size();
        auto method_name = method == ClipboardMethod::OSC52 ? "OSC 52" : "native";
        return CommandResult::success(std::format(
            "Copied to clipboard ({} bytes, {} method).", bytes, method_name));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"last", "all", "--osc52"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Add a message to the history (called after each assistant response)
    void record_response(std::string content) {
        responses_.push_back(std::move(content));
    }

private:
    std::vector<std::string> responses_;

    /// Determine which clipboard method to use
    [[nodiscard]] static ClipboardMethod detect_clipboard_method(std::span<const std::string> args) {
        for (const auto& arg : args) {
            if (arg == "--osc52") return ClipboardMethod::OSC52;
        }
        // Default: try native first, fall back to OSC 52 in SSH sessions
        return ClipboardMethod::Native;
    }

    /// Parse which message(s) to copy
    [[nodiscard]] std::string parse_target(std::span<const std::string> args) const {
        // Filter out flags
        for (const auto& arg : args) {
            if (arg.starts_with("-")) continue;
            return arg;
        }
        return "last";
    }

    /// Resolve the target to actual content
    [[nodiscard]] std::string resolve_content(const std::string& target) const {
        if (responses_.empty()) return "";

        if (target == "last") {
            return responses_.back();
        }
        if (target == "all") {
            std::string combined;
            for (const auto& r : responses_) {
                if (!combined.empty()) combined += "\n\n---\n\n";
                combined += r;
            }
            return combined;
        }

        // Try to parse as index
        try {
            auto idx = std::stoul(target);
            if (idx > 0 && idx <= responses_.size()) {
                return responses_[idx - 1];
            }
        } catch (...) {}

        return responses_.back();
    }

    /// Copy content to clipboard using the specified method
    [[nodiscard]] static VoidResult copy_to_clipboard(
        const std::string& content, ClipboardMethod method) {
        if (method == ClipboardMethod::OSC52) {
            return copy_osc52(content);
        }
        return copy_native(content);
    }

    /// Copy using OSC 52 escape sequence (works over SSH/tmux)
    [[nodiscard]] static VoidResult copy_osc52(const std::string& content) {
        // Base64 encode the content
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

        // Emit OSC 52 escape sequence: \033]52;c;<base64>\a
        std::fprintf(stdout, "\033]52;c;%s\a", b64.c_str());
        std::fflush(stdout);
        return {};
    }

    /// Copy using native clipboard command (platform-detected)
    [[nodiscard]] static VoidResult copy_native(const std::string& content) {
        // Detect clipboard command based on platform
        const char* cmd = nullptr;

#if defined(__APPLE__)
        cmd = "pbcopy";
#elif defined(__linux__)
        // Prefer wl-copy for Wayland, fall back to xclip for X11
        if (std::getenv("WAYLAND_DISPLAY") != nullptr) {
            cmd = "wl-copy";
        } else {
            cmd = "xclip -selection clipboard";
        }
#else
        return std::unexpected(Error::make(
            ErrorCode::InternalError, "Clipboard not supported on this platform"));
#endif

        // Open pipe to clipboard command
        FILE* pipe = popen(cmd, "w");
        if (!pipe) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Failed to open clipboard command: {}", cmd)));
        }

        std::size_t written = std::fwrite(content.data(), 1, content.size(), pipe);
        int status = pclose(pipe);

        if (written != content.size()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Failed to write all content to clipboard"));
        }
        if (status != 0) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Clipboard command exited with status {}", status)));
        }

        return {};
    }
};

} // namespace cc::commands
