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

// API Key 状态信息
struct KeyStatus {
    bool valid;
    std::optional<std::string> org_name;
    std::optional<std::chrono::system_clock::time_point> expires;
};

namespace detail {
    // 当前 key 状态缓存
    inline KeyStatus& cached_key_status() {
        static KeyStatus status{.valid = false, .org_name = std::nullopt, .expires = std::nullopt};
        return status;
    }

    // key 失效回调列表
    inline std::vector<std::function<void(std::string)>>& key_invalid_callbacks() {
        static std::vector<std::function<void(std::string)>> callbacks;
        return callbacks;
    }
} // namespace detail

// 验证 API Key 的有效性
inline std::expected<void, std::string> verify_api_key(std::string_view key) {
    if (key.empty()) {
        return std::unexpected("API key cannot be empty");
    }
    // 基本格式校验：Anthropic key 以 "sk-ant-" 开头
    if (!key.starts_with("sk-ant-")) {
        return std::unexpected("Invalid API key format");
    }
    // 实际实现会向 API 发起验证请求
    detail::cached_key_status().valid = true;
    return {};
}

// 检查 key 是否有效（使用缓存结果）
inline bool is_key_valid() {
    return detail::cached_key_status().valid;
}

// 获取 key 的详细状态
inline KeyStatus get_key_status() {
    return detail::cached_key_status();
}

// 注册 key 失效时的回调
inline void on_key_invalid(std::function<void(std::string)> callback) {
    detail::key_invalid_callbacks().push_back(std::move(callback));
}

} // namespace cc::hooks
