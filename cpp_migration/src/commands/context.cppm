/// @file context.cppm
/// @brief ContextCommand implementing the /context slash command.
/// Show current context window usage, list sources, add/remove context files, token breakdown.
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

export module cc.commands.context;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Source of context in the context window
struct ContextSource {
    std::string name;               // Display name (e.g., file path, "system prompt")
    std::uint32_t token_count;      // Approximate tokens consumed
    std::string type;               // "file", "system", "conversation", "tool_result"
    bool pinned = false;            // Whether it persists across compaction
};

/// ContextCommand implements the /context slash command.
/// Displays and manages the context window content.
class ContextCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "context",
            .description = "Show and manage context window",
            .aliases = {"ctx"},
            .args = {
                CommandArg{.name = "action", .description = "show | add | remove | breakdown | clear",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"show", "add", "remove", "breakdown", "clear"}}},
                CommandArg{.name = "path", .description = "File path to add/remove",
                           .type = ArgType::FilePath, .required = false},
            },
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"show", "add", "remove", "breakdown", "clear"};
        if (std::ranges::find(valid, action) == valid.end()) {
            // May be a file path for add shorthand
            return {};
        }
        if ((action == "add" || action == "remove") && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Usage: /context {} <file_path>", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_overview());

        auto action = std::string(ctx.args[0]);

        if (action == "show") return CommandResult::success(format_overview());
        if (action == "breakdown") return CommandResult::success(format_breakdown());
        if (action == "add") return add_file(std::string(ctx.args[1]));
        if (action == "remove") return remove_file(std::string(ctx.args[1]));
        if (action == "clear") return clear_user_context();

        // Shorthand: treat unknown arg as file path to add
        return add_file(action);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"show", "add", "remove", "breakdown", "clear"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        for (const auto& src : sources_) {
            if (src.name.starts_with(partial)) {
                suggestions.push_back(src.name);
            }
        }
        return suggestions;
    }

    /// Set context window capacity
    void set_capacity(std::uint32_t max_tokens) { max_tokens_ = max_tokens; }

    /// Update context sources (called by the engine before each request)
    void set_sources(std::vector<ContextSource> sources) { sources_ = std::move(sources); }

private:
    std::uint32_t max_tokens_ = 200000;
    std::vector<ContextSource> sources_;
    std::vector<std::string> user_files_;  // Explicitly added files

    [[nodiscard]] std::uint32_t total_used() const {
        std::uint32_t sum = 0;
        for (const auto& s : sources_) sum += s.token_count;
        return sum;
    }

    [[nodiscard]] std::string format_overview() const {
        auto used = total_used();
        auto pct = max_tokens_ > 0 ? (used * 100 / max_tokens_) : 0;

        std::string out = std::format(
            "Context Window: {}/{} tokens ({}% used)\n",
            used, max_tokens_, pct);

        // Visual bar
        constexpr std::uint32_t bar_width = 30;
        auto filled = static_cast<std::uint32_t>(bar_width * pct / 100);
        out += "  [";
        for (std::uint32_t i = 0; i < bar_width; ++i) {
            out += (i < filled) ? "█" : "░";
        }
        out += "]\n";

        out += std::format("  Sources: {}\n", sources_.size());
        if (!user_files_.empty()) {
            out += std::format("  User files: {}", user_files_.size());
        }
        return out;
    }

    [[nodiscard]] std::string format_breakdown() const {
        if (sources_.empty()) return "No context sources loaded.";

        std::string out = "Context breakdown:\n";

        // Group by type
        struct TypeSummary { std::uint32_t tokens = 0; std::uint32_t count = 0; };
        std::vector<std::pair<std::string, TypeSummary>> by_type;

        for (const auto& s : sources_) {
            auto it = std::ranges::find_if(by_type,
                [&](const auto& p) { return p.first == s.type; });
            if (it != by_type.end()) {
                it->second.tokens += s.token_count;
                it->second.count++;
            } else {
                by_type.emplace_back(s.type, TypeSummary{s.token_count, 1});
            }
        }

        // Sort by token count descending
        std::ranges::sort(by_type, [](const auto& a, const auto& b) {
            return a.second.tokens > b.second.tokens;
        });

        for (const auto& [type, summary] : by_type) {
            auto pct = max_tokens_ > 0 ? (summary.tokens * 100 / max_tokens_) : 0;
            out += std::format("  {:<15} {:>6} tokens ({:>2}%) [{} sources]\n",
                type, summary.tokens, pct, summary.count);
        }

        out += std::format("\n  Total: {} / {} tokens", total_used(), max_tokens_);
        return out;
    }

    [[nodiscard]] Result<CommandResult> add_file(const std::string& path) {
        if (std::ranges::find(user_files_, path) != user_files_.end()) {
            return CommandResult::success(std::format("File '{}' is already in context.", path));
        }
        user_files_.push_back(path);
        // In production: read file, estimate tokens, add to sources
        return CommandResult::success(std::format("Added '{}' to context.", path));
    }

    [[nodiscard]] Result<CommandResult> remove_file(const std::string& path) {
        auto it = std::ranges::find(user_files_, path);
        if (it == user_files_.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("File '{}' is not in user context.", path)));
        }
        user_files_.erase(it);
        // Also remove from sources
        std::erase_if(sources_, [&](const auto& s) { return s.name == path; });
        return CommandResult::success(std::format("Removed '{}' from context.", path));
    }

    [[nodiscard]] Result<CommandResult> clear_user_context() {
        auto count = user_files_.size();
        user_files_.clear();
        std::erase_if(sources_, [](const auto& s) { return s.type == "file"; });
        return CommandResult::success(std::format("Cleared {} user context files.", count));
    }
};

} // namespace cc::commands
