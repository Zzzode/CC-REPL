/// @file vim.cppm
/// @brief VimCommand implementing the /vim slash command.
/// Toggle vim mode on/off, show current status.
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

export module cc.commands.vim;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Vim mode state tracking
enum class VimModeState : std::uint8_t {
    Normal,
    Insert,
    Visual,
    Command,
};

/// VimCommand implements the /vim slash command.
/// Toggles vim-style keybindings for the input line editor.
class VimCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "vim",
            .description = "Toggle vim-style keybindings",
            .args = {
                CommandArg{.name = "action", .description = "on | off | status",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"on", "off", "status"}},
            },
            .category = "config",
            .aliases = {"vi"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        if (action != "on" && action != "off" && action != "status") {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: on|off|status", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            // Toggle
            enabled_ = !enabled_;
            return CommandResult::success(std::format(
                "Vim mode: {}", enabled_ ? "ON" : "OFF"));
        }

        auto action = std::string(ctx.args[0]);

        if (action == "on") {
            enabled_ = true;
            mode_ = VimModeState::Normal;
            return CommandResult::success("Vim mode enabled. Press 'i' to enter insert mode.");
        }
        if (action == "off") {
            enabled_ = false;
            return CommandResult::success("Vim mode disabled. Standard editing restored.");
        }
        if (action == "status") {
            return CommandResult::success(format_status());
        }

        return CommandResult::success(format_status());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"on", "off", "status"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Check if vim mode is enabled
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

    /// Get current vim mode state
    [[nodiscard]] VimModeState current_mode() const noexcept { return mode_; }

    /// Set vim mode state (called by the input handler)
    void set_mode(VimModeState mode) { mode_ = mode; }

private:
    bool enabled_ = false;
    VimModeState mode_ = VimModeState::Normal;

    [[nodiscard]] static constexpr std::string_view mode_to_str(VimModeState mode) noexcept {
        switch (mode) {
            case VimModeState::Normal:  return "NORMAL";
            case VimModeState::Insert:  return "INSERT";
            case VimModeState::Visual:  return "VISUAL";
            case VimModeState::Command: return "COMMAND";
        }
        return "UNKNOWN";
    }

    [[nodiscard]] std::string format_status() const {
        if (!enabled_) return "Vim mode: OFF";
        return std::format("Vim mode: ON\nCurrent mode: {}\n\n"
            "Keybindings:\n"
            "  i     — Enter insert mode\n"
            "  Esc   — Return to normal mode\n"
            "  v     — Enter visual mode\n"
            "  :     — Command mode\n"
            "  dd    — Delete line\n"
            "  yy    — Yank line\n"
            "  p     — Paste",
            mode_to_str(mode_));
    }
};

} // namespace cc::commands
