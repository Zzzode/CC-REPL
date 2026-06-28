/// @file skill_usage.cppm
/// @brief SL-04: recency-weighted skill-usage scoring, faithful to TS
/// skillUsageTracking.ts. Powers empty-'/' recent-skill-first ordering.
///
/// Persistence: a per-user sidecar at ~/.claude/skill_usage.json (TS stores it
/// in ~/.claude.json's GlobalConfig.skillUsage; cpp uses a dedicated sidecar to
/// avoid owning ConfigManager). Format is line-oriented (name<TAB>count<TAB>ms)
/// for robustness; semantically equivalent to TS's Record<name,{usageCount,lastUsedAt}>.
module;

#include <string_view>

export module cc.utils.skill_usage;

export namespace cc::utils::skill_usage {

/// Recency-weighted usage score (higher = more recent + more frequent).
/// Mirrors TS getSkillUsageScore (skillUsageTracking.ts:44-55):
///   days = (now - last_used)/86400000; factor = max(pow(0.5, days/7), 0.1);
///   return usage_count * factor.
/// Returns 0 if the skill has never been used.
[[nodiscard]] double get_skill_usage_score(std::string_view skill_name);

/// Record a skill invocation. 60s per-skill in-process debounce
/// (skillUsageTracking.ts:3,18) — repeated invocations within the window don't
/// double-count. Lazy-loads + writes-through the sidecar so usage survives restarts.
void record_skill_usage(std::string_view skill_name);

}  // namespace cc::utils::skill_usage
