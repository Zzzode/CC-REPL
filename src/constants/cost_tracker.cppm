/// @file cost_tracker.cppm
/// @brief Cost tracking module for monitoring API usage, token consumption,
/// and budget management.
/// Tracks both aggregate costs per-model, per-agent, and per-session.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <optional>
#include <expected>
#include <chrono>
#include <mutex>
#include <format>

export module cc.constants.cost_tracker;

import cc.types.types;

export namespace cc::core {

// ============================================================
// Model Pricing
// ============================================================

/// Pricing per-model pricing (in microdollars per token
struct ModelPricing {
    double input_per_token;
    double output_per_token;
    double cache_creation_per_token;
    double cache_read_per_token;
};

/// Known model pricing
const std::unordered_map<std::string, ModelPricing> MODEL_PRICES = {
    {"claude-3-5-sonnet-20241022", {3.0, 15.0, 3.75, 0.30}},
    {"claude-3-7-sonnet-20250219", {3.0, 15.0, 3.75, 0.30}},
    {"claude-3-opus-20240229", {15.0, 75.0, 18.75, 1.50}},
    {"claude-3-sonnet-20240229", {3.0, 15.0, 3.75, 0.30}},
    {"claude-3-haiku-20240307", {0.25, 1.25, 0.30, 0.03}},
};

/// Default fallback pricing for unknown models
const ModelPricing FALLBACK_PRICING = {3.0, 15.0, 3.75, 0.30};

// ============================================================
// Usage Record
// ============================================================

/// Single record of an individual API call and its cost
struct UsageRecord {
    std::string model;
    TokenUsage usage;
    double cost_microusd;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> description;
};

// ============================================================
// Budget
// ============================================================

/// Per-period budget configuration
struct BudgetConfig {
    std::optional<double> hard_limit_microusd;
    std::optional<double> soft_limit_microusd;
    std::optional<std::chrono::milliseconds> period;
};

// ============================================================
// Cost Tracker
// ============================================================

/// Tracks token usage and budget management
class CostTracker {
    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<UsageRecord>> records_;
    TokenUsage total_usage_;
    double total_cost_microusd_ = 0.0;
    std::optional<BudgetConfig> budget_;
    std::chrono::system_clock::time_point last_reset_;

    /// Get pricing for a model
    [[nodiscard]] static ModelPricing get_pricing(const std::string& model) {
        auto it = MODEL_PRICES.find(model);
        if (it != MODEL_PRICES.end()) {
            return it->second;
        }
        return FALLBACK_PRICING;
    }

public:
    CostTracker() 
        : last_reset_(std::chrono::system_clock::now()) {}

    explicit CostTracker(BudgetConfig budget)
        : budget_(std::move(budget)), last_reset_(std::chrono::system_clock::now()) {}

    /// Record an API call's usage
    void record(const std::string& model, const TokenUsage& usage, 
                std::optional<std::string> description = std::nullopt) {
        std::lock_guard lock(mutex_);
        
        auto pricing = get_pricing(model);
        double cost = (static_cast<double>(usage.input_tokens) * pricing.input_per_token) +
                    (static_cast<double>(usage.output_tokens) * pricing.output_per_token) +
                    (static_cast<double>(usage.cache_creation_tokens) * pricing.cache_creation_per_token) +
                    (static_cast<double>(usage.cache_read_tokens) * pricing.cache_read_per_token);
        
        total_cost_microusd_ += cost;
        
        total_usage_.input_tokens += usage.input_tokens;
        total_usage_.output_tokens += usage.output_tokens;
        total_usage_.cache_creation_tokens += usage.cache_creation_tokens;
        total_usage_.cache_read_tokens += usage.cache_read_tokens;
        
        records_[model].push_back(UsageRecord{
            model, usage, cost, std::chrono::system_clock::now(), std::move(description)});
    }

    /// Get total costs since reset
    [[nodiscard]] double get_total_cost() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return total_cost_microusd_;
    }

    /// Get total usage since reset
    [[nodiscard]] TokenUsage get_total_usage() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return total_usage_;
    }

    /// Get per-model breakdown
    [[nodiscard]] std::map<std::string, std::pair<TokenUsage, double>> get_per_model_breakdown() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        std::map<std::string, std::pair<TokenUsage, double>> result;
        
        for (const auto& [model, records] : records_) {
            TokenUsage model_usage;
            double model_cost = 0.0;
            for (const auto& rec : records) {
                model_usage += rec.usage;
                model_cost += rec.cost_microusd;
            }
            result.emplace(model, std::make_pair(model_usage, model_cost));
        }
        return result;
    }

    /// Check if budget is exceeded
    [[nodiscard]] bool is_over_budget() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        if (!budget_ || !budget_->hard_limit_microusd) return false;
        return total_cost_microusd_ >= *budget_->hard_limit_microusd;
    }

    /// Check if approaching budget is within 90%
    [[nodiscard]] bool is_near_budget() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        if (!budget_ || !budget_->soft_limit_microusd) return false;
        return total_cost_microusd_ >= *budget_->soft_limit_microusd;
    }

    /// Reset tracking
    void reset() {
        std::lock_guard lock(mutex_);
        records_.clear();
        total_usage_ = TokenUsage{};
        total_cost_microusd_ = 0.0;
        last_reset_ = std::chrono::system_clock::now();
    }

    /// Set budget
    void set_budget(BudgetConfig budget) {
        std::lock_guard lock(mutex_);
        budget_ = std::move(budget);
    }

    /// Format cost in a readable format
    [[nodiscard]] static std::string format_cost(double microusd) {
        if (microusd < 1000.0) {
            return std::format("{:.2f} μUSD", microusd);
        } else if (microusd < 1000000.0) {
            return std::format("{:.4f} USD", microusd / 1000000.0);
        } else {
            return std::format("{:.2f} USD", microusd / 1000000.0);
        }
    }
};

// ============================================================
// Global Cost Tracker Singleton
// ============================================================

/// Access the process-wide global CostTracker instance.
/// QueryEngine and other subsystems record usage here; /cost
/// reads from this singleton when no AppState bridge is available.
[[nodiscard]] inline CostTracker& global_cost_tracker() {
    static CostTracker instance;
    return instance;
}

} // namespace cc::core
