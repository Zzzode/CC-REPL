/// @file clear.cppm
/// @brief ClearCommand implementing the /clear slash command.
/// Clears the terminal screen, optionally resets conversation state, and clears caches.
/// Dispatches ClearMessages and ResetSession actions to properly reset AppState.
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
import cc.state.app_state;

export namespace cc::commands {

using namespace cc::core;

// ============================================================
// Action type ordinals (keep in sync with cc::state::ActionType
// enum ordering in store.cppm)
// ============================================================

/// Ordinal of ActionType::ClearMessages in cc::state::ActionType.
/// Clears the messages vector in AppState.
constexpr int ACTION_CLEAR_MESSAGES = 1;

/// Ordinal of ActionType::ResetSession in cc::state::ActionType.
/// Resets AppState to defaults, preserving model/config fields.
constexpr int ACTION_RESET_SESSION = 29;

/// Ordinal of ActionType::AddNotification in cc::state::ActionType.
/// Pushes a notification string into the notifications list.
constexpr int ACTION_ADD_NOTIFICATION = 18;

/// What to clear when executing the command
enum class ClearScope : std::uint8_t {
    Screen,          // Clear terminal display only
    Conversation,    // Reset conversation history
    Cache,           // Clear cached data (completions, tool results)
    All,             // Clear everything
};

/// ClearCommand implements the /clear slash command.
/// Clears screen, optionally resets conversation and caches.
/// Dispatches AppState actions (ClearMessages / ResetSession) via
/// the CommandContext bridge so the reactive UI picks up the change.
class ClearCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "clear",
            .description = "Clear the screen and optionally reset conversation state",
            .args = {
                CommandArg{.name = "--reset", .description = "Also reset conversation history",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--cache", .description = "Clear cached data",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--all", .description = "Clear screen, conversation, and cache",
                           .type = ArgType::None, .required = false},
            },
            .category = "session",
            .aliases = {"cls"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto scope = parse_scope(ctx.args);

        switch (scope) {
            case ClearScope::Screen:
                return execute_screen_clear(ctx);

            case ClearScope::Cache:
                return execute_cache_clear(ctx);

            case ClearScope::Conversation:
                return execute_conversation_reset(ctx);

            case ClearScope::All:
                return execute_full_reset(ctx);
        }

        return CommandResult::fail("Unknown clear scope");
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

    // ── Scope-specific execution ────────────────────────────────────────

    /// Default /clear: dispatch ClearMessages, call conversation reset
    /// callback, clear the screen.
    [[nodiscard]] Result<CommandResult> execute_screen_clear(const CommandContext& ctx) {
        // 1. Dispatch ClearMessages so the AppState messages vector is cleared
        //    and the reactive UI re-renders with an empty message list.
        ctx.dispatch_action(ACTION_CLEAR_MESSAGES, nullptr);

        // 2. If a session-manager reset callback is registered, invoke it so
        //    the engine-level conversation state is also reset.
        if (conversation_reset_fn_) {
            auto result = conversation_reset_fn_();
            if (!result) return std::unexpected(result.error());
        }

        // 3. Clear the terminal display.
        clear_screen();

        return CommandResult::success("Screen cleared.");
    }

    /// /clear --cache: clear static caches (completions, tool results,
    /// responses) and push a cache-cleared notification.
    [[nodiscard]] Result<CommandResult> execute_cache_clear(const CommandContext& ctx) {
        auto result = clear_cache();
        if (!result) return std::unexpected(result.error());

        // Dispatch a notification so the user sees confirmation in the UI.
        std::string note = std::format("Cache cleared ({} entries removed).", *result);
        ctx.dispatch_action(ACTION_ADD_NOTIFICATION, &note);

        return CommandResult::success(std::move(note));
    }

    /// /clear --reset: dispatch ResetSession action, clear screen.
    [[nodiscard]] Result<CommandResult> execute_conversation_reset(const CommandContext& ctx) {
        // 1. Dispatch ResetSession — resets AppState to defaults, preserving
        //    model config, permissions, working directory, and settings.
        ctx.dispatch_action(ACTION_RESET_SESSION, nullptr);

        // 2. Also invoke the engine-level reset callback if set.
        if (conversation_reset_fn_) {
            auto result = conversation_reset_fn_();
            if (!result) return std::unexpected(result.error());
        }

        // 3. Clear static caches as well (session-level caches).
        (void)clear_cache();

        // 4. Clear the terminal display.
        clear_screen();

        return CommandResult::success("Session reset. Screen cleared. Conversation cleared.");
    }

    /// /clear --all: full reset — ResetSession + all caches + clear screen.
    [[nodiscard]] Result<CommandResult> execute_full_reset(const CommandContext& ctx) {
        // 1. Dispatch ResetSession action.
        ctx.dispatch_action(ACTION_RESET_SESSION, nullptr);

        // 2. Invoke engine-level reset callback.
        if (conversation_reset_fn_) {
            auto result = conversation_reset_fn_();
            if (!result) return std::unexpected(result.error());
        }

        // 3. Clear all static caches.
        auto cache_result = clear_cache();
        std::uint32_t cache_entries = cache_result ? *cache_result : 0;

        // 4. Clear the terminal display.
        clear_screen();

        std::string msg = "Screen cleared. Conversation reset.";
        if (cache_entries > 0) {
            msg += std::format(" {} cache entries removed.", cache_entries);
        }
        return CommandResult::success(std::move(msg));
    }

    // ── Helpers ─────────────────────────────────────────────────────────

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