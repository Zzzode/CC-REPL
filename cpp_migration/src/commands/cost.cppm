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
import cc.state.app_state;
import cc.constants.cost_tracker;

export namespace cc::commands {

using namespace cc::core;

/// Ordinal of ActionType::UpdateUsage in the cc::state::ActionType enum.
/// Keep in sync with store.cppm enum ordering.
constexpr int ACTION_UPDATE_USAGE = 7;

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
            .args = {
                CommandArg{.name = "view", .description = "summary | messages | models",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"summary", "messages", "models"}},
            },
            .category = "session",
            .aliases = {},
            .hidden = false,
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

        // Try AppState bridge first (set by app.cppm) — authoritative data
        // populated by QueryEngine via UpdateUsage dispatches.
        if (const void* raw_state = ctx.get_app_state(); raw_state != nullptr) {
            const auto* state = static_cast<const cc::state::AppState*>(raw_state);
            if (view == "summary") {
                return CommandResult::success(format_summary_from_app_state(*state));
            }
            if (view == "messages") {
                return CommandResult::success(format_per_message_from_app_state(*state));
            }
            if (view == "models") {
                return CommandResult::success(format_by_model_from_tracker());
            }
            return CommandResult::success(format_summary_from_app_state(*state));
        }

        // Fallback: read from global CostTracker (populated by record() calls
        // from QueryEngine when no AppState dispatch is wired).
        auto& tracker = global_cost_tracker();
        double tracker_cost_usd = tracker.get_total_cost() / 1'000'000.0;
        auto tracker_usage = tracker.get_total_usage();

        if (view == "summary") {
            if (tracker_usage.total() == 0 && entries_.empty()) {
                return CommandResult::success("No API requests made yet. Cost: $0.00");
            }
            // Merge: CostTracker has authoritative totals, entries_ has per-request records.
            std::uint32_t total_in = tracker_usage.input_tokens ? tracker_usage.input_tokens : 0;
            std::uint32_t total_out = tracker_usage.output_tokens ? tracker_usage.output_tokens : 0;
            double cost = tracker_cost_usd > 0.0 ? tracker_cost_usd : total_cost_;
            std::size_t num_requests = entries_.empty()
                ? static_cast<std::size_t>(total_in + total_out > 0 ? 1 : 0)
                : entries_.size();

            if (num_requests == 0 && cost <= 0.0) {
                return CommandResult::success("No API requests made yet. Cost: $0.00");
            }

            return CommandResult::success(std::format(
                "Session Cost Summary:\n"
                "  Total cost:     ${:.4f}\n"
                "  Input tokens:   {}\n"
                "  Output tokens:  {}\n"
                "  Total requests: {}\n"
                "  Avg per request: ${:.4f}",
                cost, total_in, total_out, num_requests,
                num_requests > 0 ? cost / static_cast<double>(num_requests) : 0.0));
        }
        if (view == "messages") {
            if (entries_.empty() && tracker_usage.total() == 0) {
                return CommandResult::success("No messages yet.");
            }
            return CommandResult::success(format_per_message());
        }
        if (view == "models") {
            return CommandResult::success(format_by_model_from_tracker());
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

    /// Record a cost entry (called after each API request).
    /// Updates internal entries_ list AND the global CostTracker.
    void record(CostEntry entry) {
        total_cost_ += entry.cost_usd;
        entries_.push_back(std::move(entry));

        // Also record in the global CostTracker so /cost can read it
        // even when no AppState bridge is available.
        const auto& e = entries_.back();
        TokenUsage usage;
        usage.input_tokens = e.input_tokens;
        usage.output_tokens = e.output_tokens;
        global_cost_tracker().record(e.model, usage, "api_request");
    }

    /// Record usage via CommandContext — dispatches UpdateUsage to AppState
    /// AND records in the global CostTracker. Use this when QueryEngine has
    /// access to the CommandContext.
    void record(const CommandContext& ctx, std::string_view model,
                const TokenUsage& usage, double cost_usd) {
        // Update the global CostTracker
        global_cost_tracker().record(std::string(model), usage, "api_request");

        // Also keep internal entries_ for per-message /cost messages view
        CostEntry entry;
        entry.model = std::string(model);
        entry.input_tokens = usage.input_tokens;
        entry.output_tokens = usage.output_tokens;
        entry.cost_usd = cost_usd;
        entry.timestamp = std::chrono::system_clock::now();
        total_cost_ += cost_usd;
        entries_.push_back(std::move(entry));

        // Dispatch UpdateUsage action to AppState if bridge is available
        if (ctx.dispatch_fn && ctx.app_store) {
            ctx.dispatch_action(ACTION_UPDATE_USAGE, &usage);
        }
    }

private:
    std::vector<CostEntry> entries_;
    double total_cost_ = 0.0;

    // ----------------------------------------------------------
    // Format from AppState (authoritative — set by QueryEngine dispatch)
    // ----------------------------------------------------------

    [[nodiscard]] std::string format_summary_from_app_state(
        const cc::state::AppState& state) const {

        const auto& usage = state.total_usage;
        double cost = state.total_cost_usd;

        if (usage.total() == 0 && cost <= 0.0) {
            return "No API requests made yet. Cost: $0.00";
        }

        // Estimate request count from entries_ if available, else derive from usage.
        std::size_t num_requests = entries_.empty()
            ? static_cast<std::size_t>(usage.total() > 0 ? 1 : 0)
            : entries_.size();

        return std::format(
            "Session Cost Summary:\n"
            "  Total cost:     ${:.4f}\n"
            "  Input tokens:   {}\n"
            "  Output tokens:  {}\n"
            "  Cache read:     {}\n"
            "  Total requests: {}\n"
            "  Avg per request: ${:.4f}",
            cost,
            usage.input_tokens,
            usage.output_tokens,
            usage.cache_read_tokens,
            num_requests,
            num_requests > 0 ? cost / static_cast<double>(num_requests) : 0.0);
    }

    [[nodiscard]] std::string format_per_message_from_app_state(
        const cc::state::AppState& state) const {

        if (entries_.empty()) {
            // No per-message records — show aggregate from AppState.
            const auto& usage = state.total_usage;
            if (usage.total() == 0 && state.total_cost_usd <= 0.0) {
                return "No messages yet.";
            }
            return std::format(
                "Cost per message:\n"
                "  (aggregate — no per-request records)\n"
                "  Input: {} tokens, Output: {} tokens\n"
                "  Total: ${:.4f}",
                usage.input_tokens, usage.output_tokens, state.total_cost_usd);
        }
        return format_per_message();
    }

    // ----------------------------------------------------------
    // Format from global CostTracker (for by-model breakdown)
    // ----------------------------------------------------------

    [[nodiscard]] std::string format_by_model_from_tracker() const {
        auto breakdown = global_cost_tracker().get_per_model_breakdown();
        if (breakdown.empty() && entries_.empty()) {
            return "No messages yet.";
        }

        std::string out = "Cost by model:\n";
        double total = 0.0;
        for (const auto& [model, pair] : breakdown) {
            const auto& [model_usage, model_cost_microusd] = pair;
            double model_cost_usd = model_cost_microusd / 1'000'000.0;
            total += model_cost_usd;
            out += std::format("  {} — ${:.4f} (in:{} out:{} cache_r:{})\n",
                model, model_cost_usd,
                model_usage.input_tokens, model_usage.output_tokens,
                model_usage.cache_read_tokens);
        }

        // If CostTracker is empty but entries_ has data, fall back to entries
        if (breakdown.empty()) {
            return format_by_model();
        }

        // Use AppState total cost if available, else compute from tracker
        double display_total = total > 0.0 ? total : total_cost_;
        out += std::format("  Total: ${:.4f}", display_total);
        return out;
    }

    // ----------------------------------------------------------
    // Legacy formatters (from internal entries_)
    // ----------------------------------------------------------

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
