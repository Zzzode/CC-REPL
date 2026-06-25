module;

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module cc.utils.plugin_dependency_resolver;

import cc.utils.plugin_identifier;

export namespace cc::utils::plugin_dependency_resolver {

inline constexpr std::string_view inline_marketplace = "inline";

struct DependencyLookupResult {
    std::vector<std::string> dependencies;
};

enum class ResolutionFailure : unsigned char {
    None,
    Cycle,
    NotFound,
    CrossMarketplace,
};

struct ResolutionResult {
    bool ok = false;
    ResolutionFailure reason = ResolutionFailure::None;
    std::vector<std::string> closure;
    std::vector<std::string> chain;
    std::string dependency;
    std::string missing;
    std::string required_by;
    std::string message;
};

struct LoadedPlugin {
    std::string name;
    std::string source;
    bool enabled = false;
    std::vector<std::string> dependencies;
};

struct DependencyError {
    std::string source;
    std::string plugin;
    std::string dependency;
    std::string reason;
};

struct DemotionResult {
    std::set<std::string> demoted;
    std::vector<DependencyError> errors;
};

[[nodiscard]] inline std::string qualify_dependency(std::string_view dep, std::string_view declaring_plugin_id) {
    if (cc::utils::plugin_identifier::parse_plugin_identifier(dep).marketplace) return std::string(dep);
    const auto declaring = cc::utils::plugin_identifier::parse_plugin_identifier(declaring_plugin_id);
    if (!declaring.marketplace || *declaring.marketplace == inline_marketplace) return std::string(dep);
    std::string out(dep);
    out.push_back('@');
    out.append(*declaring.marketplace);
    return out;
}

namespace detail {

using LookupFn = std::function<std::optional<DependencyLookupResult>(const std::string&)>;

[[nodiscard]] inline std::optional<ResolutionResult> walk(
    const std::string& id,
    const std::string& required_by,
    const std::string& root_id,
    const std::optional<std::string>& root_marketplace,
    const LookupFn& lookup,
    const std::unordered_set<std::string>& already_enabled,
    const std::unordered_set<std::string>& allowed_cross_marketplaces,
    std::vector<std::string>& closure,
    std::unordered_set<std::string>& visited,
    std::vector<std::string>& stack
) {
    if (id != root_id && already_enabled.contains(id)) return std::nullopt;

    const auto parsed = cc::utils::plugin_identifier::parse_plugin_identifier(id);
    if (parsed.marketplace != root_marketplace && !(parsed.marketplace && allowed_cross_marketplaces.contains(*parsed.marketplace))) {
        ResolutionResult result;
        result.reason = ResolutionFailure::CrossMarketplace;
        result.dependency = id;
        result.required_by = required_by;
        result.message = "cross-marketplace dependency";
        return result;
    }

    if (std::find(stack.begin(), stack.end(), id) != stack.end()) {
        ResolutionResult result;
        result.reason = ResolutionFailure::Cycle;
        result.chain = stack;
        result.chain.push_back(id);
        result.message = "dependency cycle";
        return result;
    }
    if (visited.contains(id)) return std::nullopt;
    visited.insert(id);

    auto entry = lookup(id);
    if (!entry) {
        ResolutionResult result;
        result.reason = ResolutionFailure::NotFound;
        result.missing = id;
        result.required_by = required_by;
        result.message = "dependency not found";
        return result;
    }

    stack.push_back(id);
    for (const auto& raw_dep : entry->dependencies) {
        const auto dep = qualify_dependency(raw_dep, id);
        if (auto err = walk(dep, id, root_id, root_marketplace, lookup, already_enabled, allowed_cross_marketplaces, closure, visited, stack)) return err;
    }
    stack.pop_back();
    closure.push_back(id);
    return std::nullopt;
}

} // namespace detail

[[nodiscard]] inline ResolutionResult resolve_dependency_closure(
    const std::string& root_id,
    detail::LookupFn lookup,
    std::unordered_set<std::string> already_enabled = {},
    std::unordered_set<std::string> allowed_cross_marketplaces = {}
) {
    std::vector<std::string> closure;
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack;
    const auto root_marketplace = cc::utils::plugin_identifier::parse_plugin_identifier(root_id).marketplace;
    if (auto err = detail::walk(root_id, root_id, root_id, root_marketplace, lookup, already_enabled, allowed_cross_marketplaces, closure, visited, stack)) return *err;
    ResolutionResult result;
    result.ok = true;
    result.closure = std::move(closure);
    return result;
}

[[nodiscard]] inline DemotionResult verify_and_demote(const std::vector<LoadedPlugin>& plugins) {
    std::unordered_set<std::string> known;
    std::unordered_set<std::string> enabled;
    std::unordered_set<std::string> known_by_name;
    std::unordered_map<std::string, int> enabled_by_name;
    for (const auto& p : plugins) {
        known.insert(p.source);
        known_by_name.insert(cc::utils::plugin_identifier::parse_plugin_identifier(p.source).name);
        if (p.enabled) {
            enabled.insert(p.source);
            enabled_by_name[p.name]++;
        }
    }

    std::vector<DependencyError> errors;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& p : plugins) {
            if (!enabled.contains(p.source)) continue;
            for (const auto& raw_dep : p.dependencies) {
                const auto dep = qualify_dependency(raw_dep, p.source);
                const bool is_bare = !cc::utils::plugin_identifier::parse_plugin_identifier(dep).marketplace.has_value();
                const bool satisfied = is_bare ? ((enabled_by_name[dep]) > 0) : enabled.contains(dep);
                if (!satisfied) {
                    enabled.erase(p.source);
                    auto& count = enabled_by_name[p.name];
                    if (count <= 1) enabled_by_name.erase(p.name);
                    else --count;
                    errors.push_back({
                        .source = p.source,
                        .plugin = p.name,
                        .dependency = dep,
                        .reason = (is_bare ? known_by_name.contains(dep) : known.contains(dep)) ? "not-enabled" : "not-found",
                    });
                    changed = true;
                    break;
                }
            }
        }
    }

    std::set<std::string> demoted;
    for (const auto& p : plugins) {
        if (p.enabled && !enabled.contains(p.source)) demoted.insert(p.source);
    }
    return {.demoted = demoted, .errors = errors};
}

[[nodiscard]] inline std::vector<std::string> find_reverse_dependents(const std::string& plugin_id, const std::vector<LoadedPlugin>& plugins) {
    std::vector<std::string> out;
    const auto target_name = cc::utils::plugin_identifier::parse_plugin_identifier(plugin_id).name;
    for (const auto& p : plugins) {
        if (!p.enabled || p.source == plugin_id) continue;
        for (const auto& raw_dep : p.dependencies) {
            const auto qualified = qualify_dependency(raw_dep, p.source);
            const auto parsed = cc::utils::plugin_identifier::parse_plugin_identifier(qualified);
            if ((parsed.marketplace && qualified == plugin_id) || (!parsed.marketplace && qualified == target_name)) {
                out.push_back(p.name);
                break;
            }
        }
    }
    return out;
}

[[nodiscard]] inline std::string format_dependency_count_suffix(const std::vector<std::string>& installed_deps) {
    if (installed_deps.empty()) return "";
    const auto n = installed_deps.size();
    return " (+ " + std::to_string(n) + " " + (n == 1 ? "dependency" : "dependencies") + ")";
}

[[nodiscard]] inline std::string format_reverse_dependents_suffix(const std::vector<std::string>& reverse_dependents) {
    if (reverse_dependents.empty()) return "";
    std::string out = " — warning: required by ";
    for (std::size_t i = 0; i < reverse_dependents.size(); ++i) {
        if (i != 0) out += ", ";
        out += reverse_dependents[i];
    }
    return out;
}

} // namespace cc::utils::plugin_dependency_resolver
