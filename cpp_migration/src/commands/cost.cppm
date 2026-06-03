/// @file cost.cppm
/// @brief CostCommand implementing the /cost slash command.
/// Show accumulated cost, per-message breakdown, and cost by model.
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
#include <chrono>

export module cc.commands.cost;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Record of cost for a single API request
struct CostEntry {
    std::string model;
    std::uint32_t input_tokens;
    std::uint32_t output_tokens;
    double cost_usd;
    std::chrono::system_clock::time_point timestamp;
};

/// CostCommand implements the /cost slash command.
/// Displays accumulated cost information with breakdowns.
class CostCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "cost",
            .description = "Show cost breakdown for the current session",
            .aliases = {},
            .args = {
                CommandArg{.name = "view", .description = "summary | messages | models",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"summary", "messages", "models"}},
            },
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto view = ctx.args[0];
        if (view != "summary" && view != "messages" && view != "models") {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid view: '{}'. Use: summary|messages|models", view)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto view = ctx.args.empty() ? std::string("summary") : std::string(ctx.args[0]);

        if (view == "summary") {
            return CommandResult::success(format_summary());
        }
        if (view == "messages") {
            return CommandResult::success(format_per_message());
        }
        if (view == "models") {
            return CommandResult::success(format_by_model());
        }
        return CommandResult::success(format_summary());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"summary", "messages", "models"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Record a cost entry (called after each API request)
    void record(CostEntry entry) {
        total_cost_ += entry.cost_usd;
        entries_.push_back(std::move(entry));
    }

private:
    std::vector<CostEntry> entries_;
    double total_cost_ = 0.0;

    [[nodiscard]] std::string format_summary() const {
        if (entries_.empty()) {
            return "No API requests made yet. Cost: $0.00";
        }
        return std::format(
            "Session Cost Summary:\n"
            "  Total cost:     ${:.4f}\n"
            "  Total requests: {}\n"
            "  Avg per request: ${:.4f}",
            total_cost_, entries_.size(), total_cost_ / static_cast<double>(entries_.size()));
    }

    [[nodiscard]] std::string format_per_message() const {
        if (entries_.empty()) return "No messages yet.";

        std::string out = "Cost per message:\n";
        std::size_t idx = 1;
        for (const auto& e : entries_) {
            out += std::format("  #{:<3} {} — ${:.4f} (in:{} out:{})\n",
                idx++, e.model, e.cost_usd, e.input_tokens, e.output_tokens);
        }
        out += std::format("  Total: ${:.4f}", total_cost_);
        return out;
    }

    [[nodiscard]] std::string format_by_model() const {
        if (entries_.empty()) return "No messages yet.";

        // Aggregate cost by model
        struct ModelCost { double cost = 0.0; std::uint32_t count = 0; };
        std::vector<std::pair<std::string, ModelCost>> model_costs;

        for (const auto& e : entries_) {
            auto it = std::ranges::find_if(model_costs,
                [&](const auto& p) { return p.first == e.model; });
            if (it != model_costs.end()) {
                it->second.cost += e.cost_usd;
                it->second.count++;
            } else {
                model_costs.emplace_back(e.model, ModelCost{e.cost_usd, 1});
            }
        }

        std::string out = "Cost by model:\n";
        for (const auto& [model, mc] : model_costs) {
            out += std::format("  {} — ${:.4f} ({} requests)\n", model, mc.cost, mc.count);
        }
        out += std::format("  Total: ${:.4f}", total_cost_);
        return out;
    }
};

} // namespace cc::commands
