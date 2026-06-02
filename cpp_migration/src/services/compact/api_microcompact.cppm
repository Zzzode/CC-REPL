/// @file api_microcompact.cppm
/// @brief API-level micro compaction for context management
module;
#include <string>
#include <vector>
#include <expected>
export module cc.services.compact.api_microcompact;
export namespace cc::services::compact {
struct MicroCompactResult { std::vector<std::string> removed_message_ids; uint64_t tokens_freed{0}; };
[[nodiscard]] inline std::expected<MicroCompactResult, std::string> perform_micro_compact(uint64_t target_tokens) {
    return MicroCompactResult{{}, target_tokens > 0 ? target_tokens / 10 : 0};
}
} // namespace
