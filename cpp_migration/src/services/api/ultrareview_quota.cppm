/// @file ultrareview_quota.cppm
/// @brief Ultra review quota management
module;
#include <string>
#include <optional>
#include <cstdint>
export module cc.services.api.ultrareview_quota;
export namespace cc::services::api {
struct UltraReviewQuota { uint32_t remaining{0}; uint32_t total{0}; bool is_unlimited{false}; };
[[nodiscard]] inline UltraReviewQuota get_ultrareview_quota() { return {10, 10, false}; }
[[nodiscard]] inline bool can_use_ultrareview() { return get_ultrareview_quota().remaining > 0; }
} // namespace
