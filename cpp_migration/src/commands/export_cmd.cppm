/// @file export_cmd.cppm
/// @brief ExportCommand implementing the /export slash command.
/// Export conversation as markdown/JSON, write to file, include/exclude system messages.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <filesystem>
#include <fstream>

export module cc.commands.export_cmd;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Export format for conversation output
enum class ExportFormat : std::uint8_t {
    Markdown,
    Json,
};

/// Options controlling what to export
struct ExportOptions {
    ExportFormat format = ExportFormat::Markdown;
    bool include_system = false;
    bool include_tool_calls = true;
    std::optional<std::string> output_path;
};

/// ExportCommand implements the /export slash command.
/// Exports the conversation to a file in various formats.
class ExportCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "export",
            .description = "Export conversation to a file",
            .aliases = {},
            .args = {
                CommandArg{.name = "format", .description = "markdown | json",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"markdown", "json", "md"}}},
                CommandArg{.name = "path", .description = "Output file path",
                           .type = ArgType::FilePath, .required = false},
                CommandArg{.name = "--system", .description = "Include system messages",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--no-tools", .description = "Exclude tool call details",
                           .type = ArgType::None, .required = false},
            },
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);

        if (messages_.empty()) {
            return CommandResult::success("No messages to export.");
        }

        auto content = format_export(opts);
        auto output_path = resolve_output_path(opts);

        // In production: write to file via libuv async IO
        auto write_result = write_file(output_path, content);
        if (!write_result) return std::unexpected(write_result.error());

        return CommandResult::success(std::format(
            "Exported {} messages to: {}\nFormat: {}, Size: {} bytes",
            messages_.size(), output_path,
            opts.format == ExportFormat::Json ? "JSON" : "Markdown",
            content.size()));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"markdown", "json", "md", "--system", "--no-tools"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Add a message to export buffer (called by the conversation system)
    void add_message(Role role, std::string content) {
        messages_.push_back({role, std::move(content)});
    }

private:
    struct ExportMessage {
        Role role;
        std::string content;
    };
    std::vector<ExportMessage> messages_;

    [[nodiscard]] static ExportOptions parse_options(std::span<const std::string> args) {
        ExportOptions opts;
        for (const auto& arg : args) {
            if (arg == "json") opts.format = ExportFormat::Json;
            else if (arg == "markdown" || arg == "md") opts.format = ExportFormat::Markdown;
            else if (arg == "--system") opts.include_system = true;
            else if (arg == "--no-tools") opts.include_tool_calls = false;
            else if (!arg.starts_with("-")) opts.output_path = arg;
        }
        return opts;
    }

    [[nodiscard]] std::string format_export(const ExportOptions& opts) const {
        if (opts.format == ExportFormat::Json) return format_json(opts);
        return format_markdown(opts);
    }

    [[nodiscard]] std::string format_markdown(const ExportOptions& opts) const {
        std::string out = "# Conversation Export\n\n";
        for (const auto& msg : messages_) {
            if (msg.role == Role::System && !opts.include_system) continue;
            out += std::format("## {}\n\n{}\n\n", role_to_string(msg.role), msg.content);
        }
        return out;
    }

    [[nodiscard]] std::string format_json(const ExportOptions& opts) const {
        std::string out = "[\n";
        bool first = true;
        for (const auto& msg : messages_) {
            if (msg.role == Role::System && !opts.include_system) continue;
            if (!first) out += ",\n";
            first = false;
            // Simple JSON serialization (in production: use yyjson)
            out += std::format(R"(  {{"role": "{}", "content": "{}"}})",
                role_to_string(msg.role), msg.content);
        }
        out += "\n]";
        return out;
    }

    [[nodiscard]] static std::string resolve_output_path(const ExportOptions& opts) {
        if (opts.output_path) return *opts.output_path;
        auto ext = opts.format == ExportFormat::Json ? "json" : "md";
        return std::format("conversation_export.{}", ext);
    }

    [[nodiscard]] static VoidResult write_file(
        const std::string& path, const std::string& content) {
        namespace fs = std::filesystem;

        // Ensure parent directory exists
        auto parent = fs::path(path).parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            std::error_code ec;
            fs::create_directories(parent, ec);
            if (ec) {
                return std::unexpected(
                    AppError{ErrorCode::IoError,
                             std::format("Failed to create directory '{}': {}",
                                         parent.string(), ec.message())});
            }
        }

        // Write content to file
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return std::unexpected(
                AppError{ErrorCode::IoError,
                         std::format("Failed to open file for writing: {}", path)});
        }

        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs.good()) {
            return std::unexpected(
                AppError{ErrorCode::IoError,
                         std::format("Failed to write to file: {}", path)});
        }

        return {};
    }
};

} // namespace cc::commands
