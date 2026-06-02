module;
#include <string>
#include <string_view>
export module cc.services.compact.post_compact_cleanup;

export namespace cc::services::compact {

// Perform cleanup after a compact operation
auto cleanup_after_compact(std::string_view session_id) -> void {
    (void)session_id;
    // No module-local transient state is retained after compact.
}

// Invalidate all cached data that may reference old message indices
auto invalidate_caches() -> void {
    // No module-local caches are retained.
}

// Update stored token counts after compact
auto update_token_counts(std::string_view session_id, int new_count) -> void {
    (void)session_id;
    (void)new_count;
    // Token counts are not persisted by this module.
}

} // namespace cc::services::compact
