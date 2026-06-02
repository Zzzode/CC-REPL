/// @file first_token_date.cppm
/// @brief Track first token usage date for billing
module;
#include <string>
#include <optional>
#include <chrono>
export module cc.services.api.first_token_date;
export namespace cc::services::api {
using Clock = std::chrono::system_clock;
[[nodiscard]] inline std::optional<Clock::time_point> get_first_token_date() { return std::nullopt; }
inline void set_first_token_date(Clock::time_point /*tp*/) {}
} // namespace
