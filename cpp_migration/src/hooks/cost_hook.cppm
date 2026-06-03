module;
#include <functional>
#include <optional>
#include <string>
#include <vector>

export module cc.hooks.cost_hook;

export namespace cc::hooks {


struct CostUpdate {
    double session_cost;
    double monthly_cost;
    int input_tokens;
    int output_tokens;
};

namespace detail {

    inline CostUpdate& current_cost_data() {
        static CostUpdate data{0.0, 0.0, 0, 0};
        return data;
    }


    inline std::vector<std::function<void(CostUpdate)>>& cost_listeners() {
        static std::vector<std::function<void(CostUpdate)>> listeners;
        return listeners;
    }

    inline int& next_cost_listener_id() {
        static int id = 0;
        return id;
    }


    inline double& cost_budget_limit() {
        static double limit = 0.0;
        return limit;
    }
} // namespace detail


inline int on_cost_update(std::function<void(CostUpdate)> callback) {
    detail::cost_listeners().push_back(std::move(callback));
    return ++detail::next_cost_listener_id();
}


inline CostUpdate get_current_cost() {
    return detail::current_cost_data();
}


inline std::optional<std::string> check_cost_threshold() {
    double budget = detail::cost_budget_limit();
    if (budget <= 0.0) return std::nullopt;

    auto& cost = detail::current_cost_data();
    if (cost.session_cost >= budget) {
        return "Session cost ($" + std::to_string(cost.session_cost) +
               ") has exceeded budget ($" + std::to_string(budget) + ")";
    }

    if (cost.session_cost >= budget * 0.8) {
        return "Warning: Session cost is at " +
               std::to_string(static_cast<int>(cost.session_cost / budget * 100)) +
               "% of budget";
    }
    return std::nullopt;
}


inline void set_cost_budget(double max_usd) {
    detail::cost_budget_limit() = max_usd;
}

} // namespace cc::hooks
