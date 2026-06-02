// C++23 module: System prompt section management types and utilities.
// Provides a registry pattern for memoized/volatile prompt sections.
module;
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.constants.system_prompt_sections;


export namespace cc::constants::system_prompt_sections {

// Compute function signature: returns optional string content
using ComputeFn = std::function<std::optional<std::string>()>;

// Represents a system prompt section with optional caching
struct SystemPromptSection {
    std::string name;
    ComputeFn compute;
    bool cache_break; // If true, recomputes every turn (breaks prompt cache)
};

// Create a memoized system prompt section.
// Computed once, cached until /clear or /compact.
inline SystemPromptSection system_prompt_section(
    std::string name,
    ComputeFn compute
) {
    return SystemPromptSection{
        .name = std::move(name),
        .compute = std::move(compute),
        .cache_break = false,
    };
}

// Create a volatile system prompt section that recomputes every turn.
// This WILL break the prompt cache when the value changes.
// Requires a reason explaining why cache-breaking is necessary.
inline SystemPromptSection dangerous_uncached_system_prompt_section(
    std::string name,
    ComputeFn compute,
    [[maybe_unused]] std::string_view reason
) {
    return SystemPromptSection{
        .name = std::move(name),
        .compute = std::move(compute),
        .cache_break = true,
    };
}

// Section cache: maps section name to computed value
using SectionCache = std::unordered_map<std::string, std::optional<std::string>>;

// Resolve all system prompt sections, using cache when available
inline std::vector<std::optional<std::string>> resolve_system_prompt_sections(
    const std::vector<SystemPromptSection>& sections,
    SectionCache& cache
) {
    std::vector<std::optional<std::string>> results;
    results.reserve(sections.size());

    for (const auto& section : sections) {
        if (!section.cache_break) {
            if (auto it = cache.find(section.name); it != cache.end()) {
                results.push_back(it->second);
                continue;
            }
        }
        auto value = section.compute();
        cache[section.name] = value;
        results.push_back(std::move(value));
    }

    return results;
}

// Clear all cached sections and reset state
inline void clear_system_prompt_sections(SectionCache& cache) {
    cache.clear();
}

} // namespace cc::constants::system_prompt_sections
