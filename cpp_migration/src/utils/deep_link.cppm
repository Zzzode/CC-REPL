// C++23 Module: Deep link/URL protocol handling
// 深度链接处理：解析和处理 claude:// 协议 URL
module;
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

export module cc.utils.deep_link;

export namespace cc::core::deep_link {

// 深度链接动作枚举
enum class DeepLinkAction : uint8_t {
    OpenFile,       // 打开文件
    RunCommand,     // 执行命令
    ResumeSession,  // 恢复会话
    InstallPlugin   // 安装插件
};

// 深度链接结构体
struct DeepLink {
    std::string scheme;      // 协议 (如 "claude")
    DeepLinkAction action;   // 动作类型
    std::unordered_map<std::string, std::string> params;  // 参数键值对
    std::string raw_url;     // 原始 URL

    // 获取参数值
    [[nodiscard]] std::optional<std::string> get_param(std::string_view key) const {
        auto it = params.find(std::string(key));
        if (it != params.end()) return it->second;
        return std::nullopt;
    }

    // 检查是否有效
    [[nodiscard]] bool is_valid() const {
        return !scheme.empty() && !raw_url.empty();
    }
};

// URL 解码辅助
[[nodiscard]] inline std::string url_decode(std::string_view encoded) {
    std::string result;
    result.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            auto hex = encoded.substr(i + 1, 2);
            char decoded = static_cast<char>(
                std::stoi(std::string(hex), nullptr, 16));
            result += decoded;
            i += 2;
        } else if (encoded[i] == '+') {
            result += ' ';
        } else {
            result += encoded[i];
        }
    }
    return result;
}

// URL 编码辅助
[[nodiscard]] inline std::string url_encode(std::string_view text) {
    std::string result;
    result.reserve(text.size() * 3);

    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += std::format("%{:02X}", c);
        }
    }
    return result;
}

// 动作名称映射
[[nodiscard]] inline std::string_view action_to_string(DeepLinkAction action) {
    switch (action) {
        case DeepLinkAction::OpenFile:      return "open";
        case DeepLinkAction::RunCommand:    return "run";
        case DeepLinkAction::ResumeSession: return "resume";
        case DeepLinkAction::InstallPlugin: return "install";
    }
    return "unknown";
}

[[nodiscard]] inline std::optional<DeepLinkAction> string_to_action(std::string_view str) {
    if (str == "open")    return DeepLinkAction::OpenFile;
    if (str == "run")     return DeepLinkAction::RunCommand;
    if (str == "resume")  return DeepLinkAction::ResumeSession;
    if (str == "install") return DeepLinkAction::InstallPlugin;
    return std::nullopt;
}

// 解析深度链接 URL
[[nodiscard]] inline std::expected<DeepLink, std::string> parse_deep_link(std::string_view url) {
    DeepLink link;
    link.raw_url = std::string(url);

    // 解析 scheme (scheme://)
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected("Invalid deep link: missing scheme");
    }
    link.scheme = std::string(url.substr(0, scheme_end));

    // scheme 之后的部分
    auto rest = url.substr(scheme_end + 3);

    // 解析 action (第一个路径段)
    auto action_end = rest.find('/');
    std::string_view action_str;
    std::string_view query_part;

    if (action_end == std::string_view::npos) {
        // 可能 action 后直接跟 ?query
        auto q_pos = rest.find('?');
        if (q_pos == std::string_view::npos) {
            action_str = rest;
        } else {
            action_str = rest.substr(0, q_pos);
            query_part = rest.substr(q_pos + 1);
        }
    } else {
        action_str = rest.substr(0, action_end);
        auto remaining = rest.substr(action_end + 1);
        auto q_pos = remaining.find('?');
        if (q_pos != std::string_view::npos) {
            query_part = remaining.substr(q_pos + 1);
            // 路径部分作为默认参数
            auto path_part = remaining.substr(0, q_pos);
            if (!path_part.empty()) {
                link.params["path"] = url_decode(path_part);
            }
        } else {
            if (!remaining.empty()) {
                link.params["path"] = url_decode(remaining);
            }
        }
    }

    // 解析 action
    auto action = string_to_action(action_str);
    if (!action) {
        return std::unexpected(std::format("Unknown action: {}", action_str));
    }
    link.action = *action;

    // 解析 query parameters
    if (!query_part.empty()) {
        size_t pos = 0;
        while (pos < query_part.size()) {
            auto amp = query_part.find('&', pos);
            auto pair = (amp == std::string_view::npos)
                ? query_part.substr(pos) : query_part.substr(pos, amp - pos);

            auto eq = pair.find('=');
            if (eq != std::string_view::npos) {
                auto key = url_decode(pair.substr(0, eq));
                auto value = url_decode(pair.substr(eq + 1));
                link.params[key] = value;
            }

            pos = (amp == std::string_view::npos) ? query_part.size() : amp + 1;
        }
    }

    return link;
}

// 生成深度链接 URL
[[nodiscard]] inline std::string generate_link(
    DeepLinkAction action,
    const std::unordered_map<std::string, std::string>& params,
    std::string_view scheme = "claude") {

    std::string url = std::format("{}://{}", scheme, action_to_string(action));

    // 如果有 path 参数，放在路径中
    if (auto it = params.find("path"); it != params.end()) {
        url += "/" + url_encode(it->second);
    }

    // 其他参数放在 query string
    std::string query;
    for (const auto& [key, value] : params) {
        if (key == "path") continue;
        if (!query.empty()) query += "&";
        query += url_encode(key) + "=" + url_encode(value);
    }

    if (!query.empty()) url += "?" + query;
    return url;
}

// 深度链接处理器类
class DeepLinkHandler {
public:
    using HandlerFn = std::function<std::expected<void, std::string>(const DeepLink&)>;

    // 注册协议处理 (平台特定)
    [[nodiscard]] std::expected<void, std::string> register_protocol(
        std::string_view scheme = "claude") {
        scheme_ = std::string(scheme);
        // 平台特定注册逻辑
#ifdef __APPLE__
        // macOS: 通过 Info.plist CFBundleURLTypes 注册
        return {};
#elif defined(__linux__)
        // Linux: 通过 xdg-mime 或 .desktop 文件注册
        return {};
#else
        return std::unexpected("Protocol registration not supported on this platform");
#endif
    }

    // 处理深度链接
    [[nodiscard]] std::expected<void, std::string> handle(const DeepLink& link) {
        // 查找注册的处理器
        auto it = handlers_.find(link.action);
        if (it != handlers_.end()) {
            return it->second(link);
        }
        return std::unexpected(std::format("No handler for action: {}",
            action_to_string(link.action)));
    }

    // 注册特定动作的处理器
    void on(DeepLinkAction action, HandlerFn handler) {
        handlers_[action] = std::move(handler);
    }

    [[nodiscard]] const std::string& scheme() const { return scheme_; }

private:
    std::string scheme_{"claude"};
    std::unordered_map<DeepLinkAction, HandlerFn> handlers_;
};

} // namespace cc::core::deep_link
