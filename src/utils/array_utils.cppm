module;
#include <vector>
#include <span>
#include <optional>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <cstddef>

export module cc.utils.array_utils;

export namespace cc::utils {


template <typename T>
[[nodiscard]] std::vector<std::vector<T>> chunk(const std::vector<T>& vec, size_t chunk_size) {
    if (chunk_size == 0) return {};
    std::vector<std::vector<T>> result;
    result.reserve((vec.size() + chunk_size - 1) / chunk_size);
    for (size_t i = 0; i < vec.size(); i += chunk_size) {
        auto end = std::min(i + chunk_size, vec.size());
        result.emplace_back(vec.begin() + static_cast<ptrdiff_t>(i),
                           vec.begin() + static_cast<ptrdiff_t>(end));
    }
    return result;
}


template <typename T>
[[nodiscard]] std::vector<T> unique(const std::vector<T>& vec) {
    std::vector<T> result;
    result.reserve(vec.size());
    std::unordered_set<T> seen;
    for (const auto& item : vec) {
        if (seen.insert(item).second) {
            result.push_back(item);
        }
    }
    return result;
}

// TypeScript-compatible alias for unique().
template <typename T>
[[nodiscard]] std::vector<T> uniq(const std::vector<T>& vec) {
    return unique(vec);
}

// Count elements matching a predicate.
template <typename T, typename Predicate>
[[nodiscard]] std::size_t count(std::span<const T> s, Predicate predicate) {
    std::size_t n = 0;
    for (const auto& item : s) {
        if (predicate(item)) ++n;
    }
    return n;
}

template <typename T, typename Predicate>
[[nodiscard]] std::size_t count(const std::vector<T>& vec, Predicate predicate) {
    return count<T, Predicate>(std::span<const T>(vec.data(), vec.size()), predicate);
}

// Insert separator(index) before every item except the first, matching TS intersperse().
template <typename T, typename SeparatorFn>
[[nodiscard]] std::vector<T> intersperse(const std::vector<T>& vec, SeparatorFn separator) {
    if (vec.empty()) return {};
    std::vector<T> result;
    result.reserve(vec.size() * 2 - 1);
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (i != 0) result.push_back(separator(i));
        result.push_back(vec[i]);
    }
    return result;
}


template <typename T>
[[nodiscard]] std::vector<T> flatten(const std::vector<std::vector<T>>& nested) {
    std::vector<T> result;
    size_t total = 0;
    for (const auto& inner : nested) total += inner.size();
    result.reserve(total);
    for (const auto& inner : nested) {
        result.insert(result.end(), inner.begin(), inner.end());
    }
    return result;
}


template <typename T>
[[nodiscard]] bool includes(std::span<const T> s, const T& value) {
    return std::find(s.begin(), s.end(), value) != s.end();
}


template <typename T>
[[nodiscard]] std::optional<size_t> find_index(std::span<const T> s,
                                                std::function<bool(const T&)> predicate) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (predicate(s[i])) return i;
    }
    return std::nullopt;
}


template <typename T>
[[nodiscard]] std::vector<T> take(std::span<const T> s, size_t n) {
    auto count = std::min(n, s.size());
    return std::vector<T>(s.begin(), s.begin() + static_cast<ptrdiff_t>(count));
}


template <typename T>
[[nodiscard]] std::vector<T> drop(std::span<const T> s, size_t n) {
    if (n >= s.size()) return {};
    return std::vector<T>(s.begin() + static_cast<ptrdiff_t>(n), s.end());
}

} // namespace cc::utils
