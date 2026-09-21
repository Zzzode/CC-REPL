module;
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.model.model_allowlist;

export namespace cc::utils {

namespace detail {
    inline std::vector<std::string>& allowlist_ref() {
        static std::vector<std::string> list = {
            "claude-sonnet-4-20250514",
            "claude-opus-4-20250514",
            "claude-haiku-4-20250514",
        };
        return list;
    }
} // namespace detail

bool is_model_allowed(std::string_view model_id) {
    auto& list = detail::allowlist_ref();
    return std::find(list.begin(), list.end(), model_id) != list.end();
}

std::vector<std::string> get_allowed_models() {
    return detail::allowlist_ref();
}

bool add_to_allowlist(std::string_view model_id) {
    auto& list = detail::allowlist_ref();
    // Don't add duplicates
    if (std::find(list.begin(), list.end(), model_id) != list.end()) {
        return false;
    }
    list.emplace_back(model_id);
    return true;
}

} // namespace cc::utils
