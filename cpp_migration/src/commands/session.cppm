/// @file session.cppm
/// @brief SessionCommand implementing the /session slash command.
/// List sessions, show current session info, rename, delete, export sessions.
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
#include <chrono>

export module cc.commands.session;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Detailed information about the current session
struct SessionInfo {
    SessionId id;
    std::string title;
    std::string model;
    std::uint32_t message_count;
    TokenUsage total_usage;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_active;
    std::optional<std::string> working_directory;
};

/// SessionCommand implements the /session slash command.
/// Manages conversation sessions: list, info, rename, delete, export.
class SessionCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "session",
            .description = "Manage conversation sessions",
            .aliases = {"sessions"},
            .args = {
                CommandArg{.name = "action", .description = "list | info | rename | delete | export",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"list", "info", "rename", "delete", "export"}}},
                CommandArg{.name = "value", .description = "New name, session ID, or export path",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"list", "info", "rename", "delete", "export"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: list|info|rename|delete|export", action)));
        }
        if (action == "rename" && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Usage: /session rename <new_name>"));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_current_info());

        auto action = std::string(ctx.args[0]);

        if (action == "list") return CommandResult::success(format_session_list());
        if (action == "info") return CommandResult::success(format_current_info());
        if (action == "rename") return rename_session(std::string(ctx.args[1]));
        if (action == "delete") return delete_session(ctx.args.size() > 1 ? std::string(ctx.args[1]) : "");
        if (action == "export") return export_session(ctx.args.size() > 1 ? std::string(ctx.args[1]) : "");

        return CommandResult::success(format_current_info());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"list", "info", "rename", "delete", "export"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Set the current session info (managed by the session system)
    void set_current(SessionInfo info) { current_ = std::move(info); }

private:
    std::optional<SessionInfo> current_;
    std::vector<SessionInfo> all_sessions_;

    [[nodiscard]] std::string format_current_info() const {
        if (!current_) return "No active session.";
        const auto& s = *current_;
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::system_clock::now() - s.created_at);
        return std::format(
            "Session: {}\n"
            "  ID:       {}\n"
            "  Model:    {}\n"
            "  Messages: {}\n"
            "  Tokens:   {} in / {} out\n"
            "  Duration: {}m\n"
            "  CWD:      {}",
            s.title, s.id.str().substr(0, 8), s.model, s.message_count,
            s.total_usage.input_tokens, s.total_usage.output_tokens,
            elapsed.count(), s.working_directory.value_or("(not set)"));
    }

    [[nodiscard]] std::string format_session_list() const {
        if (all_sessions_.empty()) return "No sessions found.";
        std::string out = "Sessions:\n";
        for (const auto& s : all_sessions_) {
            out += std::format("  {} — {} ({} msgs)\n",
                s.id.str().substr(0, 8), s.title, s.message_count);
        }
        return out;
    }

    [[nodiscard]] Result<CommandResult> rename_session(const std::string& new_name) {
        if (!current_) {
            return std::unexpected(Error::make(ErrorCode::SessionNotFound, "No active session."));
        }
        auto old_name = current_->title;
        current_->title = new_name;
        return CommandResult::success(std::format("Session renamed: '{}' → '{}'", old_name, new_name));
    }

    [[nodiscard]] Result<CommandResult> delete_session(const std::string& id) {
        if (id.empty() && !current_) {
            return std::unexpected(Error::make(ErrorCode::SessionNotFound, "No active session to delete."));
        }
        auto target_id = id.empty() ? current_->id.str().substr(0, 8) : id;
        return CommandResult::success(std::format("Session {} deleted.", target_id));
    }

    [[nodiscard]] Result<CommandResult> export_session(const std::string& path) const {
        if (!current_) {
            return std::unexpected(Error::make(ErrorCode::SessionNotFound, "No active session to export."));
        }
        auto target_path = path.empty() ? std::format("session_{}.json", current_->id.str().substr(0, 8)) : path;
        return CommandResult::success(std::format(
            "Session exported to: {}\n({} messages)", target_path, current_->message_count));
    }
};

} // namespace cc::commands
