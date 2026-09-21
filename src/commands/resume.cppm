/// @file resume.cppm
/// @brief ResumeCommand implementing the /resume slash command.
/// List recent sessions, resume by ID, resume last session, show session preview.
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

export module cc.commands.resume;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Summary of a resumable session
struct SessionSummary {
    SessionId id;
    std::string title;
    std::uint32_t message_count;
    std::chrono::system_clock::time_point last_active;
    std::string model;
};

/// ResumeCommand implements the /resume slash command.
/// Allows resuming previous conversation sessions.
class ResumeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "resume",
            .description = "Resume a previous conversation session",
            .args = {
                CommandArg{.name = "session_id", .description = "Session ID to resume, or 'last'",
                           .type = ArgType::Text, .required = false},
            },
            .category = "session",
            .aliases = {"r"},
            .hidden = false,
            .argument_hint = "<session_id|last>",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return CommandResult::success(format_recent_sessions());
        }

        auto arg = std::string(ctx.args[0]);

        if (arg == "last") {
            return resume_last();
        }
        if (arg == "list") {
            return CommandResult::success(format_recent_sessions());
        }

        // Treat as session ID
        return resume_by_id(arg);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        if (std::string_view("last").starts_with(partial)) {
            suggestions.emplace_back("last");
        }
        if (std::string_view("list").starts_with(partial)) {
            suggestions.emplace_back("list");
        }
        // Suggest known session IDs
        for (const auto& s : recent_sessions_) {
            if (s.id.str().starts_with(partial)) {
                suggestions.push_back(s.id.str());
            }
        }
        return suggestions;
    }

    /// Set the list of recent sessions (populated by the session manager)
    void set_recent_sessions(std::vector<SessionSummary> sessions) {
        recent_sessions_ = std::move(sessions);
    }

private:
    std::vector<SessionSummary> recent_sessions_;

    [[nodiscard]] std::string format_recent_sessions() const {
        if (recent_sessions_.empty()) {
            return "No recent sessions found.\nStart a new conversation to create a session.";
        }
        std::string out = "Recent sessions:\n";
        for (const auto& s : recent_sessions_) {
            auto age = format_time_ago(s.last_active);
            out += std::format("  {} — {} ({} msgs, {})\n",
                s.id.str().substr(0, 8), s.title, s.message_count, age);
        }
        out += "\nUse /resume <id> or /resume last";
        return out;
    }

    [[nodiscard]] Result<CommandResult> resume_last() const {
        if (recent_sessions_.empty()) {
            return std::unexpected(Error::make(ErrorCode::SessionNotFound,
                "No previous sessions to resume."));
        }
        const auto& last = recent_sessions_.front();
        return CommandResult::success(std::format(
            "Resuming session: {} ({})\n{} messages loaded.",
            last.id.str().substr(0, 8), last.title, last.message_count));
    }

    [[nodiscard]] Result<CommandResult> resume_by_id(const std::string& id) const {
        auto it = std::ranges::find_if(recent_sessions_,
            [&](const auto& s) { return s.id.str().starts_with(id); });
        if (it == recent_sessions_.end()) {
            return std::unexpected(Error::make(ErrorCode::SessionNotFound,
                std::format("Session '{}' not found. Use /resume to list available sessions.", id)));
        }
        return CommandResult::success(std::format(
            "Resuming session: {} ({})\n{} messages loaded.",
            it->id.str().substr(0, 8), it->title, it->message_count));
    }

    [[nodiscard]] static std::string format_time_ago(std::chrono::system_clock::time_point tp) {
        auto elapsed = std::chrono::system_clock::now() - tp;
        auto hours = std::chrono::duration_cast<std::chrono::hours>(elapsed).count();
        if (hours < 1) return "just now";
        if (hours < 24) return std::format("{}h ago", hours);
        return std::format("{}d ago", hours / 24);
    }
};

} // namespace cc::commands
