// C++23 module: Error IDs for tracking error sources in production.
// These IDs are obfuscated identifiers that help trace which logError() call generated an error.
// ADDING A NEW ERROR TYPE:
// 1. Add a constexpr based on next_id.
// 2. Increment next_id.
// Next ID: 346
module;
#include <string>
#include <string_view>

export module cc.constants.error_ids;


export namespace cc::constants::error_ids {

inline constexpr int next_id = 346;

inline constexpr int e_tool_use_summary_generation_failed = 344;

} // namespace cc::constants::error_ids
