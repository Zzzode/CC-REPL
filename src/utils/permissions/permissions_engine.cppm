// C++23 Module: Permissions Engine
// Migrates: src/utils/permissions/ (24 TS files)
// Core permission engine, rule matching, path patterns, caching
module;

#include <algorithm>
#include <chrono>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.utils.permissions_engine;

export namespace cc::utils::permissions {

// --- Enums ---

enum class PermissionScope : uint8_t {
    Global,
    Project,
    Session,
    Command
};

enum class PermissionAction : uint8_t {
    Allow,
    Deny,
    Ask,
    AskOnce
};

enum class MatchStrategy : uint8_t {
    Exact,
    Prefix,
    Glob,
    Regex
};

// --- Data Structures ---

struct PermissionRule {
    std::string id;
    std::string tool_pattern;
    MatchStrategy strategy{MatchStrategy::Glob};
    PermissionAction action{PermissionAction::Ask};
    PermissionScope scope{PermissionScope::Session};
    std::optional<std::string> path_pattern;
    int priority{0};
    std::chrono::system_clock::time_point created_at{std::chrono::system_clock::now()};
};

struct PermissionQuery {
    std::string tool_name;
    std::string operation;
    std::vector<std::string> paths;
    std::optional<std::string> command;
    std::string working_directory;
};

struct PermissionResult {
    PermissionAction action{PermissionAction::Deny};
    std::string matched_rule_id;
    std::string explanation;
};

struct PermissionCache {
    std::unordered_map<std::string, PermissionResult> entries;
    size_t hits{0};
    size_t misses{0};
};

// --- Internal: Glob Matching ---

namespace detail {

[[nodiscard]] inline bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    size_t pi = 0, ti = 0;
    size_t star_p = std::string_view::npos, star_t = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
            ++pi; ++ti;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_p = pi++;
            star_t = ti;
        } else if (star_p != std::string_view::npos) {
            pi = star_p + 1;
            ti = ++star_t;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

} // namespace detail

// --- Functions ---

/// Match a file path against a pattern using the specified strategy.
[[nodiscard]] inline auto match_path_pattern(
    std::string_view path,
    std::string_view pattern,
    MatchStrategy strategy) -> bool
{
    switch (strategy) {
        case MatchStrategy::Exact:
            return path == pattern;
        case MatchStrategy::Prefix:
            return path.starts_with(pattern);
        case MatchStrategy::Glob:
            return detail::glob_match(pattern, path);
        case MatchStrategy::Regex:
            try {
                std::regex re(pattern.begin(), pattern.end());
                return std::regex_match(path.begin(), path.end(), re);
            } catch (...) {
                return false;
            }
    }
    return false;
}

/// Check if a query matches a specific rule.
[[nodiscard]] inline auto match_rule(
    const PermissionQuery& query,
    const PermissionRule& rule) -> bool
{
    // Match tool_pattern against tool_name
    bool tool_matches = match_path_pattern(query.tool_name, rule.tool_pattern, rule.strategy);
    if (!tool_matches) return false;

    // If rule has a path_pattern, at least one query path must match
    if (rule.path_pattern.has_value()) {
        if (query.paths.empty()) return false;
        bool any_path_matches = std::ranges::any_of(query.paths, [&](const auto& p) {
            return detail::glob_match(*rule.path_pattern, p);
        });
        if (!any_path_matches) return false;
    }

    return true;
}

/// Evaluate a permission query against a set of rules.
/// Rules are evaluated in priority order (highest first); first match wins.
[[nodiscard]] inline auto evaluate_permission(
    const PermissionQuery& query,
    const std::vector<PermissionRule>& rules) -> PermissionResult
{
    // Sort a copy by descending priority
    auto sorted = rules;
    std::ranges::sort(sorted, [](const auto& a, const auto& b) {
        return a.priority > b.priority;
    });

    for (const auto& rule : sorted) {
        if (match_rule(query, rule)) {
            return PermissionResult{
                .action = rule.action,
                .matched_rule_id = rule.id,
                .explanation = std::format("matched rule '{}' (pattern='{}', priority={})",
                    rule.id, rule.tool_pattern, rule.priority)
            };
        }
    }

    return PermissionResult{
        .action = PermissionAction::Deny,
        .matched_rule_id = "",
        .explanation = "no matching rule found"
    };
}

// --- Stateful Permission Engine ---

class PermissionEngine {
public:
    PermissionEngine() = default;

    /// Add a new permission rule.
    [[nodiscard]] auto add_rule(PermissionRule rule) -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        if (rule.id.empty()) {
            return std::unexpected(std::string{"rule id must not be empty"});
        }
        rules_.push_back(std::move(rule));
        invalidate_cache_locked();
        return {};
    }

    /// Remove a rule by its id. Returns true if found and removed.
    [[nodiscard]] auto remove_rule(std::string_view rule_id) -> bool {
        std::lock_guard lock(mutex_);
        auto it = std::ranges::find_if(rules_, [&](const auto& r) { return r.id == rule_id; });
        if (it == rules_.end()) return false;
        rules_.erase(it);
        invalidate_cache_locked();
        return true;
    }

    /// Get all rules, optionally filtered by scope.
    [[nodiscard]] auto get_rules(std::optional<PermissionScope> scope = {}) const
        -> std::vector<PermissionRule>
    {
        std::lock_guard lock(mutex_);
        if (!scope.has_value()) return rules_;
        std::vector<PermissionRule> filtered;
        std::ranges::copy_if(rules_, std::back_inserter(filtered),
            [&](const auto& r) { return r.scope == *scope; });
        return filtered;
    }

    /// Get effective rules for a tool name, sorted by priority (desc).
    [[nodiscard]] auto get_effective_rules(std::string_view tool_name) const
        -> std::vector<PermissionRule>
    {
        std::lock_guard lock(mutex_);
        std::vector<PermissionRule> effective;
        for (const auto& rule : rules_) {
            if (match_path_pattern(tool_name, rule.tool_pattern, rule.strategy)) {
                effective.push_back(rule);
            }
        }
        std::ranges::sort(effective, [](const auto& a, const auto& b) {
            return a.priority > b.priority;
        });
        return effective;
    }

    /// Evaluate a query with caching.
    [[nodiscard]] auto evaluate(const PermissionQuery& query) -> PermissionResult {
        std::lock_guard lock(mutex_);
        auto cache_key = query.tool_name + ":" + query.operation;
        if (auto it = cache_.entries.find(cache_key); it != cache_.entries.end()) {
            cache_.hits++;
            return it->second;
        }
        cache_.misses++;
        auto result = evaluate_permission(query, rules_);
        cache_.entries[cache_key] = result;
        return result;
    }

    /// Clear all session-scoped rules.
    void clear_session_rules() {
        std::lock_guard lock(mutex_);
        std::erase_if(rules_, [](const auto& r) { return r.scope == PermissionScope::Session; });
        invalidate_cache_locked();
    }

    /// Export all rules as a JSON array string.
    [[nodiscard]] auto export_rules() const -> std::string {
        std::lock_guard lock(mutex_);
        if (rules_.empty()) return "[]";

        std::string json = "[\n";
        for (size_t i = 0; i < rules_.size(); ++i) {
            const auto& r = rules_[i];
            json += std::format(
                R"(  {{"id":"{}","tool_pattern":"{}","strategy":{},"action":{},"scope":{},"priority":{}}})",
                r.id, r.tool_pattern,
                static_cast<int>(r.strategy),
                static_cast<int>(r.action),
                static_cast<int>(r.scope),
                r.priority);
            if (i + 1 < rules_.size()) json += ",";
            json += "\n";
        }
        json += "]";
        return json;
    }

    /// Import rules from a JSON string. Returns the number of rules imported.
    /// Expects a simple JSON array of objects with id, tool_pattern, strategy, action, scope, priority.
    [[nodiscard]] auto import_rules(std::string_view json) -> std::expected<size_t, std::string> {
        if (json.empty()) {
            return std::unexpected(std::string{"empty JSON input"});
        }

        std::lock_guard lock(mutex_);
        size_t count = 0;

        // Simple parser: find each {"id":"..." object boundary
        size_t pos = 0;
        while (pos < json.size()) {
            auto obj_start = json.find('{', pos);
            if (obj_start == std::string_view::npos) break;
            auto obj_end = json.find('}', obj_start);
            if (obj_end == std::string_view::npos) break;

            auto obj = json.substr(obj_start, obj_end - obj_start + 1);

            // Extract id field
            auto extract_str = [&](std::string_view key) -> std::string {
                auto k = std::format(R"("{}":")", key);
                auto kpos = obj.find(k);
                if (kpos == std::string_view::npos) return "";
                auto vstart = kpos + k.size();
                auto vend = obj.find('"', vstart);
                if (vend == std::string_view::npos) return "";
                return std::string(obj.substr(vstart, vend - vstart));
            };
            auto extract_int = [&](std::string_view key) -> int {
                auto k = std::format(R"("{}":)", key);
                auto kpos = obj.find(k);
                if (kpos == std::string_view::npos) return 0;
                auto vstart = kpos + k.size();
                int val = 0;
                for (size_t i = vstart; i < obj.size() && (obj[i] >= '0' && obj[i] <= '9'); ++i) {
                    val = val * 10 + (obj[i] - '0');
                }
                return val;
            };

            auto id = extract_str("id");
            auto tool_pattern = extract_str("tool_pattern");
            if (!id.empty() && !tool_pattern.empty()) {
                PermissionRule rule;
                rule.id = std::move(id);
                rule.tool_pattern = std::move(tool_pattern);
                rule.strategy = static_cast<MatchStrategy>(extract_int("strategy"));
                rule.action = static_cast<PermissionAction>(extract_int("action"));
                rule.scope = static_cast<PermissionScope>(extract_int("scope"));
                rule.priority = extract_int("priority");
                rules_.push_back(std::move(rule));
                count++;
            }
            pos = obj_end + 1;
        }

        if (count > 0) invalidate_cache_locked();
        return count;
    }

    /// Get cache statistics: (hits, misses).
    [[nodiscard]] auto get_cache_stats() const -> std::pair<size_t, size_t> {
        std::lock_guard lock(mutex_);
        return {cache_.hits, cache_.misses};
    }

    /// Invalidate the entire permission cache.
    void invalidate_cache() {
        std::lock_guard lock(mutex_);
        invalidate_cache_locked();
    }

private:
    void invalidate_cache_locked() {
        cache_.entries.clear();
        cache_.hits = 0;
        cache_.misses = 0;
    }

    mutable std::mutex mutex_;
    std::vector<PermissionRule> rules_;
    PermissionCache cache_;
};

/// Global engine instance (module-internal singleton).
inline PermissionEngine& engine() {
    static PermissionEngine instance;
    return instance;
}

// --- Free function wrappers (delegate to global engine) ---

/// Add a new permission rule to the engine.
[[nodiscard]] inline auto add_rule(PermissionRule rule)
    -> std::expected<void, std::string>
{
    return engine().add_rule(std::move(rule));
}

/// Remove a rule by its id.
[[nodiscard]] inline auto remove_rule(std::string_view rule_id) -> bool {
    return engine().remove_rule(rule_id);
}

/// Get all rules, optionally filtered by scope.
[[nodiscard]] inline auto get_rules(std::optional<PermissionScope> scope = {})
    -> std::vector<PermissionRule>
{
    return engine().get_rules(scope);
}

/// Get the effective (applicable) rules for a given tool name, sorted by priority.
[[nodiscard]] inline auto get_effective_rules(std::string_view tool_name)
    -> std::vector<PermissionRule>
{
    return engine().get_effective_rules(tool_name);
}

/// Clear all session-scoped rules.
inline void clear_session_rules() {
    engine().clear_session_rules();
}

/// Export all rules as a JSON string.
[[nodiscard]] inline auto export_rules() -> std::string {
    return engine().export_rules();
}

/// Import rules from a JSON string. Returns the number of rules imported.
[[nodiscard]] inline auto import_rules(std::string_view json)
    -> std::expected<size_t, std::string>
{
    return engine().import_rules(json);
}

/// Get cache statistics: (hits, misses).
[[nodiscard]] inline auto get_cache_stats() -> std::pair<size_t, size_t> {
    return engine().get_cache_stats();
}

/// Invalidate the entire permission cache.
inline void invalidate_cache() {
    engine().invalidate_cache();
}

} // namespace cc::utils::permissions
