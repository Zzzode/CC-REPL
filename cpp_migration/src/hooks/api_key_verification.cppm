module;
#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.api_key_verification;

export namespace cc::hooks {


struct KeyStatus {
    bool valid;
    std::optional<std::string> org_name;
    std::optional<std::chrono::system_clock::time_point> expires;
};

namespace detail {

    inline KeyStatus& cached_key_status() {
        static KeyStatus status{.valid = false, .org_name = std::nullopt, .expires = std::nullopt};
        return status;
    }


    inline std::vector<std::function<void(std::string)>>& key_invalid_callbacks() {
        static std::vector<std::function<void(std::string)>> callbacks;
        return callbacks;
    }
} // namespace detail


inline std::expected<void, std::string> verify_api_key(std::string_view key) {
    if (key.empty()) {
        return std::unexpected("API key cannot be empty");
    }

    if (!key.starts_with("sk-ant-")) {
        return std::unexpected("Invalid API key format");
    }

    detail::cached_key_status().valid = true;
    return {};
}


inline bool is_key_valid() {
    return detail::cached_key_status().valid;
}


inline KeyStatus get_key_status() {
    return detail::cached_key_status();
}


inline void on_key_invalid(std::function<void(std::string)> callback) {
    detail::key_invalid_callbacks().push_back(std::move(callback));
}

} // namespace cc::hooks
