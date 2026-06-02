module;
#include <set>
#include <algorithm>
#include <iterator>

export module cc.utils.set_utils;

export namespace cc::utils {

// Set union.
template <typename T>
[[nodiscard]] std::set<T> set_union(const std::set<T>& a, const std::set<T>& b) {
    std::set<T> result;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                   std::inserter(result, result.begin()));
    return result;
}

// Set intersection.
template <typename T>
[[nodiscard]] std::set<T> set_intersection(const std::set<T>& a, const std::set<T>& b) {
    std::set<T> result;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                          std::inserter(result, result.begin()));
    return result;
}

// Set difference (a - b).
template <typename T>
[[nodiscard]] std::set<T> set_difference(const std::set<T>& a, const std::set<T>& b) {
    std::set<T> result;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(result, result.begin()));
    return result;
}

// TypeScript-compatible alias for utils/set.ts difference(a, b).
template <typename T>
[[nodiscard]] std::set<T> difference(const std::set<T>& a, const std::set<T>& b) {
    return set_difference(a, b);
}

// TypeScript-compatible intersects(a, b): true when the sets share at least one element.
template <typename T>
[[nodiscard]] bool intersects(const std::set<T>& a, const std::set<T>& b) {
    if (a.empty() || b.empty()) return false;
    const auto& smaller = a.size() <= b.size() ? a : b;
    const auto& larger = a.size() <= b.size() ? b : a;
    for (const auto& item : smaller) {
        if (larger.contains(item)) return true;
    }
    return false;
}

// TypeScript-compatible every(a, b): true when every item in a is present in b.
template <typename T>
[[nodiscard]] bool every(const std::set<T>& a, const std::set<T>& b) {
    for (const auto& item : a) {
        if (!b.contains(item)) return false;
    }
    return true;
}

// TypeScript utils/set.ts exports union(a, b); `union` is a C++ keyword, so expose union_sets.
template <typename T>
[[nodiscard]] std::set<T> union_sets(const std::set<T>& a, const std::set<T>& b) {
    return set_union(a, b);
}

// Set symmetric difference (items present in exactly one input set).
template <typename T>
[[nodiscard]] std::set<T> set_symmetric_difference(const std::set<T>& a, const std::set<T>& b) {
    std::set<T> result;
    std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(),
                                  std::inserter(result, result.begin()));
    return result;
}

} // namespace cc::utils
