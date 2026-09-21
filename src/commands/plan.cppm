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
import cc.state.app_state;

export namespace cc::commands {

using namespace cc::core;

/// Ordinal of ActionType::SetPermissionMode in the cc::state::ActionType enum.
/// Keep in sync with store.cppm enum ordering.
constexpr int ACTION_SET_PERMISSION_MODE = 12;

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
            .args = {
                CommandArg{.name = "action", .description = "on | plan | off | auto | show | clear",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"on", "plan", "off", "auto", "show", "clear"}},
            },
            .category = "session",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"on", "plan", "off", "auto", "show", "clear"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: on|plan|off|auto|show|clear", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Try the AppState bridge first (set by app.cppm).
        if (const void* raw_state = ctx.get_app_state(); raw_state != nullptr) {
            const auto* state = static_cast<const cc::state::AppState*>(raw_state);

            if (ctx.args.empty()) {
                // No arg: read current mode from AppState
                return format_status_from_state(*state);
            }

            auto action = std::string(ctx.args[0]);

            if (action == "on" || action == "plan") {
                auto mode = cc::state::PermissionMode::Plan;
                ctx.dispatch_action(ACTION_SET_PERMISSION_MODE, &mode);
                auto result = CommandResult::success(
                    "Plan mode activated. The model will generate plans without executing tools.\n"
                    "Use /plan off to exit and execute the plan.");
                result.metadata = "UI:plan";
                return result;
            }
            if (action == "off") {
                auto mode = cc::state::PermissionMode::Default;
                ctx.dispatch_action(ACTION_SET_PERMISSION_MODE, &mode);
                return CommandResult::success(
                    "Plan mode deactivated. You can now execute normally.");
            }
            if (action == "auto") {
                auto mode = cc::state::PermissionMode::Auto;
                ctx.dispatch_action(ACTION_SET_PERMISSION_MODE, &mode);
                return CommandResult::success("Auto mode activated. Tools will run automatically.");
            }
            if (action == "show") return show_plan();
            if (action == "clear") return clear_plan();

            return format_status_from_state(*state);
        }

        // Fallback: static local when no AppState bridge is wired (e.g. unit tests).
        return execute_fallback(ctx);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"on", "plan", "off", "auto", "show", "clear"}) {
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

    [[nodiscard]] Result<CommandResult> format_status_from_state(
        const cc::state::AppState& state) const {
        auto mode = state.tool_permission_context.mode;
        auto mode_str = cc::state::permission_mode_to_string(mode);

        std::string out = std::format("Permission mode: {}\n", mode_str);

        if (mode == cc::state::PermissionMode::Plan) {
            out += "Plan mode is active. The model generates plans without executing tools.\n";
            out += "Use /plan off to exit and execute the plan.";
        } else if (mode == cc::state::PermissionMode::Auto) {
            out += "Auto mode is active. Tools run automatically without prompting.";
        } else {
            out += "Use /plan on to enter read-only planning mode.";
        }

        if (!steps_.empty()) {
            auto completed = std::ranges::count_if(steps_, [](const auto& s) { return s.completed; });
            out += std::format("\n\nPlan steps: {}/{} completed", completed, steps_.size());
        }

        auto result = CommandResult::success(std::move(out));
        if (mode == cc::state::PermissionMode::Plan) {
            result.metadata = "UI:plan";
        }
        return result;
    }

    [[nodiscard]] Result<CommandResult> execute_fallback(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return CommandResult::success(format_status());
        }

        auto action = std::string(ctx.args[0]);

        if (action == "on" || action == "plan") return enter_plan_mode();
        if (action == "off") return exit_plan_mode();
        if (action == "auto") return enter_auto_mode();
        if (action == "show") return show_plan();
        if (action == "clear") return clear_plan();

        return CommandResult::success(format_status());
    }

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
        auto result = CommandResult::success(
            "Plan mode activated. The model will generate plans without executing tools.\n"
            "Use /plan off to exit and execute the plan.");
        result.metadata = "UI:plan";
        return result;
    }

    [[nodiscard]] Result<CommandResult> enter_auto_mode() {
        active_ = false;
        return CommandResult::success(
            "Auto mode activated. Tools will run automatically.");
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
