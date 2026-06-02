module;

#include <string>
#include <unordered_set>
#include <vector>

export module cc.utils.settings_merge;

export namespace cc::utils::settings_merge {

[[nodiscard]] inline std::vector<std::string> merge_arrays_unique(
    const std::vector<std::string>& target,
    const std::vector<std::string>& source
) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    result.reserve(target.size() + source.size());
    for (const auto& value : target) {
        if (seen.insert(value).second) result.push_back(value);
    }
    for (const auto& value : source) {
        if (seen.insert(value).second) result.push_back(value);
    }
    return result;
}

} // namespace cc::utils::settings_merge
