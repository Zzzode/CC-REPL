/// @file plan.cppm
/// @brief PlanCommand implementing the /plan slash command.
/// Enter/exit plan mode (read-only), show current plan.
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

export module cc.commands.plan;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Represents a step in the generated plan
struct PlanStep {
    std::uint32_t index;
    std::string description;
    bool completed = false;
};

/// PlanCommand implements the /plan slash command.
/// Manages plan mode: a read-only state where the model generates plans without executing.
class PlanCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "plan",
            .description = "Enter plan mode (read-only) or show current plan",
            .aliases = {},
            .args = {
                CommandArg{.name = "action", .description = "on | off | show | clear",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"on", "off", "show", "clear"}}},
            },
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"on", "off", "show", "clear"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: on|off|show|clear", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            // Toggle or show status depending on state
            return CommandResult::success(format_status());
        }

        auto action = std::string(ctx.args[0]);

        if (action == "on") return enter_plan_mode();
        if (action == "off") return exit_plan_mode();
        if (action == "show") return show_plan();
        if (action == "clear") return clear_plan();

        return CommandResult::success(format_status());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"on", "off", "show", "clear"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Check if plan mode is currently active
    [[nodiscard]] bool is_active() const noexcept { return active_; }

    /// Add a step to the current plan (called by the engine during plan generation)
    void add_step(std::string description) {
        auto idx = static_cast<std::uint32_t>(steps_.size() + 1);
        steps_.push_back(PlanStep{idx, std::move(description), false});
    }

    /// Mark a step as completed
    void mark_completed(std::uint32_t index) {
        if (index > 0 && index <= steps_.size()) {
            steps_[index - 1].completed = true;
        }
    }

private:
    bool active_ = false;
    std::vector<PlanStep> steps_;
    std::chrono::system_clock::time_point started_at_;

    [[nodiscard]] std::string format_status() const {
        if (!active_) {
            return "Plan mode: OFF\nUse /plan on to enter read-only planning mode.";
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::system_clock::now() - started_at_);
        return std::format("Plan mode: ON ({}m)\nSteps: {}\nUse /plan off to exit.",
                           elapsed.count(), steps_.size());
    }

    [[nodiscard]] Result<CommandResult> enter_plan_mode() {
        if (active_) {
            return CommandResult::success("Plan mode is already active.");
        }
        active_ = true;
        started_at_ = std::chrono::system_clock::now();
        steps_.clear();
        return CommandResult::success(
            "Plan mode activated. The model will generate plans without executing tools.\n"
            "Use /plan off to exit and execute the plan.");
    }

    [[nodiscard]] Result<CommandResult> exit_plan_mode() {
        if (!active_) {
            return CommandResult::success("Plan mode is not active.");
        }
        active_ = false;
        return CommandResult::success(std::format(
            "Plan mode deactivated. {} steps in plan.\nYou can now execute normally.", steps_.size()));
    }

    [[nodiscard]] Result<CommandResult> show_plan() const {
        if (steps_.empty()) {
            return CommandResult::success("No plan steps recorded.");
        }
        std::string out = "Current Plan:\n";
        for (const auto& step : steps_) {
            auto mark = step.completed ? "✓" : "○";
            out += std::format("  {} {}. {}\n", mark, step.index, step.description);
        }
        auto completed = std::ranges::count_if(steps_, [](const auto& s) { return s.completed; });
        out += std::format("Progress: {}/{}", completed, steps_.size());
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] Result<CommandResult> clear_plan() {
        steps_.clear();
        return CommandResult::success("Plan cleared.");
    }
};

} // namespace cc::commands
