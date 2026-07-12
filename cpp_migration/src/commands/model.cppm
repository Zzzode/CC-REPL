/// @file model.cppm
/// @brief ModelCommand implementing the /model slash command.
/// Switch active model, list available models, show current model info, validate model names.
/// Reads model state from AppState and dispatches model-switch actions.
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

export module cc.commands.model;

import cc.types.types;
import cc.commands.command;
import cc.state.app_state;

export namespace cc::commands {

using namespace cc::core;

// ---------------------------------------------------------------------------
// Action type ordinals (from cc::state::ActionType in store.cppm).
// Kept as explicit constants so the command module doesn't require the
// full store header at every call site; the values are verified against
// the enum via static_assert in the test suite.
// ---------------------------------------------------------------------------
inline constexpr int ACTION_SWITCH_MODEL       = 8;   // ActionType::SwitchModel
inline constexpr int ACTION_SET_MAIN_LOOP_MODEL = 9;  // ActionType::SetMainLoopModel
inline constexpr int ACTION_SET_SETTINGS_MODEL  = 33; // ActionType::SetSettingsModel

/// ModelCommand implements the /model slash command.
/// Allows switching models, listing available ones, and showing current model.
/// Reads available/current models from AppState (via CommandContext bridge)
/// and dispatches SwitchModel / SetMainLoopModel / SetSettingsModel actions.
class ModelCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "model",
            .description = "Switch or display the active model",
            .args = {
                CommandArg{.name = "subcommand", .description = "list | set | info",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"list", "set", "info"}},
                CommandArg{.name = "model_name", .description = "Model identifier to switch to",
                           .type = ArgType::Text, .required = false},
            },
            .category = "config",
            .aliases = {},
            .hidden = false,
            .argument_hint = "<list|set|info> [model_name]",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};  // Default: open model picker dialog
        auto sub = ctx.args[0];
        if (sub != "list" && sub != "set" && sub != "info") {
            // Treat single arg as model name for quick switch
            if (!is_valid_model(sub, ctx)) {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    std::format("Unknown model: '{}'. Use /model list to see available models.", sub)
                ));
            }
        }
        if (sub == "set" && ctx.args.size() < 2) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "Usage: /model set <model_name>"));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            // No arguments: trigger model picker dialog via metadata.
            return CommandResult{
                true,
                "Opening model picker...\nUI:model-picker",
                std::string{"UI:model-picker"},
                CommandStatus::Succeeded,
            };
        }

        auto sub = std::string(ctx.args[0]);

        if (sub == "list") {
            return CommandResult::success(format_model_list(ctx));
        }
        if (sub == "info") {
            auto model_id = ctx.args.size() > 1
                ? std::string(ctx.args[1])
                : get_current_model_id(ctx);
            return format_model_info(model_id, ctx);
        }
        if (sub == "set") {
            return switch_model(std::string(ctx.args[1]), ctx);
        }

        // Quick switch: /model claude-sonnet-4-20250514
        return switch_model(sub, ctx);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        // Suggest subcommands and model names
        for (auto cmd : {"list", "set", "info"}) {
            if (std::string_view(cmd).starts_with(partial)) {
                suggestions.emplace_back(cmd);
            }
        }
        // NOTE: completion cannot access AppState because complete() is not
        // passed a CommandContext. We use the hardcoded fallback list so
        // tab-completion still works without a live AppState bridge.
        for (const auto& id : fallback_model_ids()) {
            if (id.starts_with(partial)) {
                suggestions.push_back(id);
            }
        }
        return suggestions;
    }

private:
    // -----------------------------------------------------------------------
    // Hardcoded fallback models (used when AppState bridge is unavailable)
    // -----------------------------------------------------------------------
    struct FallbackModel {
        std::string id;
        std::string display_name;
        std::uint32_t context_window;
        double input_cost_per_mtok;
        double output_cost_per_mtok;
    };

    [[nodiscard]] static const std::vector<FallbackModel>& fallback_models() {
        static const std::vector<FallbackModel> models = {
            {"claude-sonnet-4-20250514", "Claude Sonnet 4", 200000, 3.0, 15.0},
            {"claude-opus-4-20250514", "Claude Opus 4", 200000, 15.0, 75.0},
            {"claude-haiku-3-5-20241022", "Claude 3.5 Haiku", 200000, 0.25, 1.25},
        };
        return models;
    }

    [[nodiscard]] static std::vector<std::string> fallback_model_ids() {
        std::vector<std::string> ids;
        for (const auto& m : fallback_models()) {
            ids.push_back(m.id);
        }
        return ids;
    }

    // -----------------------------------------------------------------------
    // AppState access helpers
    // -----------------------------------------------------------------------

    /// Get the current model ID string, preferring AppState.
    [[nodiscard]] static std::string get_current_model_id(const CommandContext& ctx) {
        if (const void* raw = ctx.get_app_state()) {
            const auto* state = static_cast<const cc::state::AppState*>(raw);
            if (!state->current_model.model_id.empty()) {
                return state->current_model.model_id;
            }
        }
        return "claude-sonnet-4-20250514";
    }

    /// Get available models from AppState, or fallback to hardcoded list.
    [[nodiscard]] static std::vector<FallbackModel> get_available_models(const CommandContext& ctx) {
        if (const void* raw = ctx.get_app_state()) {
            const auto* state = static_cast<const cc::state::AppState*>(raw);
            if (!state->available_models.empty()) {
                std::vector<FallbackModel> result;
                result.reserve(state->available_models.size());
                for (const auto& m : state->available_models) {
                    result.push_back(FallbackModel{
                        .id = m.model_id,
                        .display_name = m.display_name,
                        .context_window = m.max_tokens > 0 ? m.max_tokens : 200000u,
                        .input_cost_per_mtok = m.input_cost_per_mtok,
                        .output_cost_per_mtok = m.output_cost_per_mtok,
                    });
                }
                return result;
            }
        }
        return fallback_models();
    }

    /// Try to find a model by ID in AppState's available_models.
    /// Returns the matching ModelConfig if found, nullopt otherwise.
    [[nodiscard]] static std::optional<cc::state::ModelConfig> find_model_config(
            const cc::state::AppState& state, std::string_view model_id) {
        auto it = std::ranges::find_if(state.available_models,
            [&](const auto& m) { return m.model_id == model_id; });
        if (it != state.available_models.end()) {
            return *it;
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------------
    // Validation
    // -----------------------------------------------------------------------

    [[nodiscard]] static bool is_valid_model(std::string_view name, const CommandContext& ctx) {
        auto models = get_available_models(ctx);
        return std::ranges::any_of(models, [&](const auto& m) { return m.id == name; });
    }

    // -----------------------------------------------------------------------
    // Formatting
    // -----------------------------------------------------------------------

    [[nodiscard]] static std::string format_model_list(const CommandContext& ctx) {
        auto models = get_available_models(ctx);
        auto current_id = get_current_model_id(ctx);

        std::string out = "Available models:\n";
        for (const auto& m : models) {
            std::string marker = (m.id == current_id) ? "  * " : "    ";
            out += std::format("{}{} ({}K context, ${:.2f}/${:.2f} per MTok)\n",
                               marker, m.id, m.context_window / 1000,
                               m.input_cost_per_mtok, m.output_cost_per_mtok);
        }
        out += std::format("\nCurrent model: {}", current_id);
        return out;
    }

    [[nodiscard]] static Result<CommandResult> format_model_info(
            const std::string& model_id, const CommandContext& ctx) {
        // Try AppState first for richer info
        if (const void* raw = ctx.get_app_state()) {
            const auto* state = static_cast<const cc::state::AppState*>(raw);
            if (auto cfg = find_model_config(*state, model_id); cfg.has_value()) {
                auto info = std::format(
                    "Model: {}\nDisplay: {}\nContext: {}K tokens\n"
                    "Input cost: ${:.2f}/MTok\nOutput cost: ${:.2f}/MTok",
                    cfg->model_id, cfg->display_name,
                    cfg->max_tokens / 1000,
                    cfg->input_cost_per_mtok, cfg->output_cost_per_mtok);
                return CommandResult::success(std::move(info));
            }
        }

        // Fallback: search hardcoded list
        auto models = fallback_models();
        auto it = std::ranges::find_if(models,
            [&](const auto& m) { return m.id == model_id; });
        if (it == models.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Model '{}' not found.", model_id)));
        }
        auto info = std::format(
            "Model: {}\nDisplay: {}\nContext: {}K tokens\n"
            "Input cost: ${:.2f}/MTok\nOutput cost: ${:.2f}/MTok",
            it->id, it->display_name, it->context_window / 1000,
            it->input_cost_per_mtok, it->output_cost_per_mtok);
        return CommandResult::success(std::move(info));
    }

    // -----------------------------------------------------------------------
    // Model switching — dispatches actions to AppState
    // -----------------------------------------------------------------------

    [[nodiscard]] static Result<CommandResult> switch_model(
            const std::string& model_id, const CommandContext& ctx) {
        if (!is_valid_model(model_id, ctx)) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Unknown model: '{}'", model_id)));
        }

        auto previous_id = get_current_model_id(ctx);

        // --- Build ModelConfig for SwitchModel action ---
        cc::state::ModelConfig model_cfg;
        bool found_in_state = false;

        if (const void* raw = ctx.get_app_state()) {
            const auto* state = static_cast<const cc::state::AppState*>(raw);
            if (auto cfg = find_model_config(*state, model_id); cfg.has_value()) {
                model_cfg = *cfg;
                found_in_state = true;
            }
        }

        if (!found_in_state) {
            // Build from fallback list
            auto models = fallback_models();
            auto it = std::ranges::find_if(models,
                [&](const auto& m) { return m.id == model_id; });
            if (it != models.end()) {
                model_cfg.model_id = it->id;
                model_cfg.display_name = it->display_name;
                model_cfg.max_tokens = it->context_window > 0
                    ? static_cast<std::uint32_t>(it->context_window) : 200000u;
                model_cfg.input_cost_per_mtok = it->input_cost_per_mtok;
                model_cfg.output_cost_per_mtok = it->output_cost_per_mtok;
            } else {
                // Minimal config for unknown model
                model_cfg.model_id = model_id;
                model_cfg.display_name = model_id;
                model_cfg.max_tokens = 200000;
            }
        }

        // --- Dispatch SwitchModel (payload: ModelConfig) ---
        ctx.dispatch_action(ACTION_SWITCH_MODEL, &model_cfg);

        // --- Dispatch SetMainLoopModel (payload: std::optional<std::string>) ---
        std::optional<std::string> main_loop_model = model_id;
        ctx.dispatch_action(ACTION_SET_MAIN_LOOP_MODEL, &main_loop_model);

        // --- Dispatch SetSettingsModel (payload: std::string) ---
        std::string settings_model = model_id;
        ctx.dispatch_action(ACTION_SET_SETTINGS_MODEL, &settings_model);

        // --- Attempt to update QueryEngine if runtime_state is available ---
        // NOTE: runtime_state points to QueryEngine, but we cannot safely
        // cast void* to QueryEngine* without importing the full query engine
        // module. The model change will propagate through the AppState
        // observer chain (on_change_app_state) which updates the engine's
        // model params on the next render cycle.

        return CommandResult::success(
            std::format("Switched model: {} → {}", previous_id, model_id));
    }
};

} // namespace cc::commands