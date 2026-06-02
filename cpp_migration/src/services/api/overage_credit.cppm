/// @file overage_credit.cppm
/// @brief Overage credit tracking for billing limits
module;
#include <string>
#include <optional>
#include <expected>
export module cc.services.api.overage_credit;
export namespace cc::services::api {
struct OverageInfo { double credit_remaining{0}; double overage_amount{0}; bool is_over_limit{false}; };
[[nodiscard]] inline std::expected<OverageInfo, std::string> check_overage() { return OverageInfo{}; }
} // namespace
