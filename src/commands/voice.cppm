/// @file voice.cppm
/// @brief VoiceCommand implementing the /voice slash command.
/// Toggle voice input mode, set voice options, show status.
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

export module cc.commands.voice;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Voice input configuration
struct VoiceConfig {
    std::string language = "en-US";
    double sensitivity = 0.5;   // 0.0 to 1.0
    bool auto_punctuate = true;
    bool echo_transcript = true;
};

/// VoiceCommand implements the /voice slash command.
/// Manages voice input mode for hands-free interaction.
class VoiceCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "voice",
            .description = "Toggle voice input mode",
            .args = {
                CommandArg{.name = "action", .description = "on | off | status | config",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"on", "off", "status", "config"}},
                CommandArg{.name = "option", .description = "Configuration key=value",
                           .type = ArgType::Text, .required = false},
            },
            .category = "config",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"on", "off", "status", "config"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: on|off|status|config", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            // Toggle voice mode
            active_ = !active_;
            return CommandResult::success(std::format("Voice input: {}", active_ ? "ON" : "OFF"));
        }

        auto action = std::string(ctx.args[0]);

        if (action == "on") return enable_voice();
        if (action == "off") return disable_voice();
        if (action == "status") return CommandResult::success(format_status());
        if (action == "config") return configure(ctx.args);

        return CommandResult::success(format_status());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"on", "off", "status", "config"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        // Suggest config keys
        for (auto k : {"language=", "sensitivity=", "auto_punctuate=", "echo="}) {
            if (std::string_view(k).starts_with(partial)) {
                suggestions.emplace_back(k);
            }
        }
        return suggestions;
    }

    /// Check if voice input is active
    [[nodiscard]] bool is_active() const noexcept { return active_; }

    /// Get current configuration
    [[nodiscard]] const VoiceConfig& config() const noexcept { return config_; }

private:
    bool active_ = false;
    VoiceConfig config_;

    [[nodiscard]] Result<CommandResult> enable_voice() {
        if (active_) return CommandResult::success("Voice input is already active.");
        active_ = true;
        return CommandResult::success(std::format(
            "Voice input enabled.\n"
            "  Language: {}\n"
            "  Sensitivity: {:.0f}%\n"
            "Speak clearly into your microphone. Say 'stop listening' to pause.",
            config_.language, config_.sensitivity * 100));
    }

    [[nodiscard]] Result<CommandResult> disable_voice() {
        if (!active_) return CommandResult::success("Voice input is already inactive.");
        active_ = false;
        return CommandResult::success("Voice input disabled.");
    }

    [[nodiscard]] std::string format_status() const {
        return std::format(
            "Voice Input: {}\n"
            "  Language:        {}\n"
            "  Sensitivity:     {:.0f}%\n"
            "  Auto-punctuate:  {}\n"
            "  Echo transcript: {}",
            active_ ? "ON" : "OFF",
            config_.language, config_.sensitivity * 100,
            config_.auto_punctuate ? "yes" : "no",
            config_.echo_transcript ? "yes" : "no");
    }

    [[nodiscard]] Result<CommandResult> configure(std::span<const std::string> args) {
        if (args.size() < 2) {
            return CommandResult::success(format_status());
        }

        // Parse key=value pairs from remaining args
        for (std::size_t i = 1; i < args.size(); ++i) {
            auto kv = std::string(args[i]);
            auto eq_pos = kv.find('=');
            if (eq_pos == std::string::npos) continue;

            auto key = kv.substr(0, eq_pos);
            auto val = kv.substr(eq_pos + 1);

            if (key == "language") {
                config_.language = val;
            } else if (key == "sensitivity") {
                auto s = std::stod(val);
                config_.sensitivity = std::clamp(s, 0.0, 1.0);
            } else if (key == "auto_punctuate") {
                config_.auto_punctuate = (val == "true" || val == "1" || val == "yes");
            } else if (key == "echo") {
                config_.echo_transcript = (val == "true" || val == "1" || val == "yes");
            }
        }

        return CommandResult::success("Voice configuration updated.\n" + format_status());
    }
};

} // namespace cc::commands
