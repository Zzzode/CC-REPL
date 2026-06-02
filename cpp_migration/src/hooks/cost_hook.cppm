module;
#include <functional>
#include <optional>
#include <string>
#include <vector>

export module cc.hooks.cost_hook;

export namespace cc::hooks {

// 成本更新信息
struct CostUpdate {
    double session_cost;    // 当前会话花费（美元）
    double monthly_cost;    // 本月累计花费（美元）
    int input_tokens;       // 输入 token 数量
    int output_tokens;      // 输出 token 数量
};

namespace detail {
    // 当前成本数据
    inline CostUpdate& current_cost_data() {
        static CostUpdate data{0.0, 0.0, 0, 0};
        return data;
    }

    // 成本更新监听器
    inline std::vector<std::function<void(CostUpdate)>>& cost_listeners() {
        static std::vector<std::function<void(CostUpdate)>> listeners;
        return listeners;
    }

    inline int& next_cost_listener_id() {
        static int id = 0;
        return id;
    }

    // 预算上限（0 表示无限制）
    inline double& cost_budget_limit() {
        static double limit = 0.0;
        return limit;
    }
} // namespace detail

// 注册成本更新回调，返回监听器 ID
inline int on_cost_update(std::function<void(CostUpdate)> callback) {
    detail::cost_listeners().push_back(std::move(callback));
    return ++detail::next_cost_listener_id();
}

// 获取当前成本数据
inline CostUpdate get_current_cost() {
    return detail::current_cost_data();
}

// 检查是否超出预算阈值，返回警告消息或空
inline std::optional<std::string> check_cost_threshold() {
    double budget = detail::cost_budget_limit();
    if (budget <= 0.0) return std::nullopt;

    auto& cost = detail::current_cost_data();
    if (cost.session_cost >= budget) {
        return "Session cost ($" + std::to_string(cost.session_cost) +
               ") has exceeded budget ($" + std::to_string(budget) + ")";
    }
    // 预警：超过 80%
    if (cost.session_cost >= budget * 0.8) {
        return "Warning: Session cost is at " +
               std::to_string(static_cast<int>(cost.session_cost / budget * 100)) +
               "% of budget";
    }
    return std::nullopt;
}

// 设置成本预算上限
inline void set_cost_budget(double max_usd) {
    detail::cost_budget_limit() = max_usd;
}

} // namespace cc::hooks
