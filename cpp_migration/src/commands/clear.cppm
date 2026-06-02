/// @file clear.cppm
/// @brief ClearCommand implementing the /clear slash command.
/// Clears the terminal screen, optionally resets conversation state, and clears caches.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <functional>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <cstdio>
#include <unordered_map>

export module cc.commands.clear;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// What to clear when executing the command
enum class ClearScope : std::uint8_t {
    Screen,          // Clear terminal display only
    Conversation,    // Reset conversation history
    Cache,           // Clear cached data (completions, tool results)
    All,             // Clear everything
};

/// ClearCommand implements the /clear slash command.
/// Clears screen, optionally resets conversation and caches.
class ClearCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "clear",
            .description = "Clear the screen and optionally reset conversation state",
            .aliases = {"cls"},
            .args = {
                CommandArg{.name = "--reset", .description = "Also reset conversation history",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--cache", .description = "Clear cached data",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--all", .description = "Clear screen, conversation, and cache",
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
        auto scope = parse_scope(ctx.args);

        std::string output;

        // Step 1: Always clear the screen
        clear_screen();
        output = "Screen cleared.";

        // Step 2: Reset conversation if requested
        if (scope == ClearScope::Conversation || scope == ClearScope::All) {
            auto result = reset_conversation();
            if (!result) return std::unexpected(result.error());
            output += " Conversation reset.";
        }

        // Step 3: Clear cache if requested
        if (scope == ClearScope::Cache || scope == ClearScope::All) {
            auto result = clear_cache();
            if (!result) return std::unexpected(result.error());
            output += std::format(" Cache cleared ({} entries removed).", *result);
        }

        return CommandResult::success(std::move(output));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array flags = {"--reset", "--cache", "--all"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }

    /// Callback for actually clearing the terminal (set by the renderer)
    using ScreenClearFn = std::function<void()>;
    void set_screen_clear_fn(ScreenClearFn fn) { screen_clear_fn_ = std::move(fn); }

    /// Callback for resetting conversation (set by the session manager)
    using ConversationResetFn = std::function<VoidResult()>;
    void set_conversation_reset_fn(ConversationResetFn fn) { conversation_reset_fn_ = std::move(fn); }

private:
    ScreenClearFn screen_clear_fn_;
    ConversationResetFn conversation_reset_fn_;

    /// Parse the scope from command arguments
    [[nodiscard]] static ClearScope parse_scope(std::span<const std::string> args) {
        for (const auto& arg : args) {
            if (arg == "--all" || arg == "-a")    return ClearScope::All;
            if (arg == "--reset" || arg == "-r")  return ClearScope::Conversation;
            if (arg == "--cache" || arg == "-c")  return ClearScope::Cache;
        }
        return ClearScope::Screen;
    }

    /// Clear the terminal screen
    void clear_screen() {
        if (screen_clear_fn_) {
            screen_clear_fn_();
        } else {
            // Fallback: ANSI escape sequence to clear screen and move cursor home
            // \033[2J clears entire screen, \033[H moves cursor to top-left (1,1)
            std::fputs("\033[2J\033[H", stdout);
            std::fflush(stdout);
        }
    }

    /// Reset conversation state
    [[nodiscard]] VoidResult reset_conversation() {
        if (conversation_reset_fn_) {
            return conversation_reset_fn_();
        }
        // If no callback is set, just succeed (the session manager handles it)
        return {};
    }

    /// Clear cached data and return number of entries removed
    [[nodiscard]] static Result<std::uint32_t> clear_cache() {
        std::uint32_t cleared = 0;

        // Clear completion cache
        cleared += clear_completion_cache();

        // Clear tool result cache
        cleared += clear_tool_cache();

        // Clear response cache
        cleared += clear_response_cache();

        return cleared;
    }

    /// Global completion cache (tab completions for commands/paths)
    static std::unordered_map<std::string, std::vector<std::string>>& completion_cache() {
        static std::unordered_map<std::string, std::vector<std::string>> cache;
        return cache;
    }

    /// Global tool result cache (file reads, grep results, etc.)
    static std::unordered_map<std::string, std::string>& tool_cache() {
        static std::unordered_map<std::string, std::string> cache;
        return cache;
    }

    /// Global response cache (prompt/response pairs)
    static std::unordered_map<std::string, std::string>& response_cache() {
        static std::unordered_map<std::string, std::string> cache;
        return cache;
    }

    /// Clear auto-completion cache
    [[nodiscard]] static std::uint32_t clear_completion_cache() {
        auto& cache = completion_cache();
        auto count = static_cast<std::uint32_t>(cache.size());
        cache.clear();
        return count;
    }

    /// Clear tool execution result cache
    [[nodiscard]] static std::uint32_t clear_tool_cache() {
        auto& cache = tool_cache();
        auto count = static_cast<std::uint32_t>(cache.size());
        cache.clear();
        return count;
    }

    /// Clear LLM response cache
    [[nodiscard]] static std::uint32_t clear_response_cache() {
        auto& cache = response_cache();
        auto count = static_cast<std::uint32_t>(cache.size());
        cache.clear();
        return count;
    }
};

} // namespace cc::commands
