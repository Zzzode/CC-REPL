/// @file model.cppm
/// @brief ModelCommand implementing the /model slash command.
/// Switch active model, list available models, show current model info, validate model names.
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

export namespace cc::commands {

using namespace cc::core;

/// Supported model identifiers
struct ModelInfo {
    std::string id;
    std::string display_name;
    std::uint32_t context_window;
    double input_cost_per_mtok;   // USD per million tokens
    double output_cost_per_mtok;
};

/// ModelCommand implements the /model slash command.
/// Allows switching models, listing available ones, and showing current model.
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
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};  // Default: show current model
        auto sub = ctx.args[0];
        if (sub != "list" && sub != "set" && sub != "info") {
            // Treat single arg as model name for quick switch
            if (!is_valid_model(sub)) {
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
            return CommandResult::success(format_current_model());
        }

        auto sub = std::string(ctx.args[0]);

        if (sub == "list") {
            return CommandResult::success(format_model_list());
        }
        if (sub == "info") {
            auto model_id = ctx.args.size() > 1 ? std::string(ctx.args[1]) : current_model_;
            return format_model_info(model_id);
        }
        if (sub == "set") {
            return switch_model(std::string(ctx.args[1]));
        }

        // Quick switch: /model claude-sonnet-4-20250514
        return switch_model(sub);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        // Suggest subcommands and model names
        for (auto cmd : {"list", "set", "info"}) {
            if (std::string_view(cmd).starts_with(partial)) {
                suggestions.emplace_back(cmd);
            }
        }
        for (const auto& m : available_models()) {
            if (m.id.starts_with(partial)) {
                suggestions.push_back(m.id);
            }
        }
        return suggestions;
    }

private:
    std::string current_model_ = "claude-sonnet-4-20250514";

    [[nodiscard]] static const std::vector<ModelInfo>& available_models() {
        static const std::vector<ModelInfo> models = {
            {"claude-sonnet-4-20250514", "Claude Sonnet 4", 200000, 3.0, 15.0},
            {"claude-opus-4-20250514", "Claude Opus 4", 200000, 15.0, 75.0},
            {"claude-haiku-3-5-20241022", "Claude 3.5 Haiku", 200000, 0.25, 1.25},
        };
        return models;
    }

    [[nodiscard]] static bool is_valid_model(std::string_view name) {
        return std::ranges::any_of(available_models(), [&](const auto& m) { return m.id == name; });
    }

    [[nodiscard]] std::string format_current_model() const {
        return std::format("Current model: {}", current_model_);
    }

    [[nodiscard]] static std::string format_model_list() {
        std::string out = "Available models:\n";
        for (const auto& m : available_models()) {
            out += std::format("  • {} ({}K context, ${:.2f}/${:.2f} per MTok)\n",
                               m.id, m.context_window / 1000, m.input_cost_per_mtok, m.output_cost_per_mtok);
        }
        return out;
    }

    [[nodiscard]] static Result<CommandResult> format_model_info(const std::string& model_id) {
        auto it = std::ranges::find_if(available_models(), [&](const auto& m) { return m.id == model_id; });
        if (it == available_models().end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Model '{}' not found.", model_id)));
        }
        auto info = std::format("Model: {}\nDisplay: {}\nContext: {}K tokens\nInput cost: ${:.2f}/MTok\nOutput cost: ${:.2f}/MTok",
            it->id, it->display_name, it->context_window / 1000, it->input_cost_per_mtok, it->output_cost_per_mtok);
        return CommandResult::success(std::move(info));
    }

    [[nodiscard]] Result<CommandResult> switch_model(const std::string& model_id) {
        if (!is_valid_model(model_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Unknown model: '{}'", model_id)));
        }
        auto previous = current_model_;
        current_model_ = model_id;
        return CommandResult::success(std::format("Switched model: {} → {}", previous, current_model_));
    }
};

} // namespace cc::commands
