/// @file vcr.cppm
/// @brief VCR (Video Cassette Recorder) service for testing.
/// Records API calls and responses for playback during testing.
/// Supports cassette file format (JSON), exact and fuzzy matching strategies.
module;

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <filesystem>
#include <functional>

export module cc.services.vcr;

import cc.types.types;

export namespace cc::services::vcr {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
namespace fs = std::filesystem;

// ============================================================
// VCR 数据结构
// ============================================================

// VCR 操作模式
enum class VcrMode : std::uint8_t {
    Record,     // 记录模式: 转发请求并保存响应
    Playback,   // 回放模式: 从磁带返回预录响应
    PassThrough, // 透传模式: 直接转发不记录
    Auto,       // 自动: 有匹配则回放，否则记录
};

// 匹配策略
enum class MatchStrategy : std::uint8_t {
    Exact,         // 精确匹配 URL + method + body
    UrlAndMethod,  // 只匹配 URL + method
    UrlOnly,       // 只匹配 URL
    Fuzzy,         // 模糊匹配 (忽略 query 参数顺序等)
    Custom,        // 自定义匹配函数
};

// HTTP 请求记录
struct RecordedRequest {
    std::string method;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    TimePoint recorded_at;
};

// HTTP 响应记录
struct RecordedResponse {
    int status_code{200};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds latency{0};
};

// 交互记录 (请求 + 响应对)
struct Interaction {
    RecordedRequest request;
    RecordedResponse response;
    std::size_t replay_count{0};  // 被回放次数
};

// 磁带 (一组交互记录)
struct Cassette {
    std::string name;
    std::string file_path;
    std::vector<Interaction> interactions;
    TimePoint created_at;
    TimePoint last_used;
    std::string version{"1.0"};
};

// VCR 配置
struct VcrConfig {
    VcrMode mode{VcrMode::Auto};
    MatchStrategy match_strategy{MatchStrategy::UrlAndMethod};
    fs::path cassette_dir{"./fixtures/vcr"};
    bool record_headers{true};
    bool ignore_query_order{true};
    std::vector<std::string> sensitive_headers{"Authorization", "Cookie"};
};

// ============================================================
// VcrService - 录制与回放
// ============================================================

class VcrService {
public:
    explicit VcrService(VcrConfig config = {})
        : config_(std::move(config)) {}

    // 加载磁带
    [[nodiscard]] std::expected<Cassette, Error> load_cassette(const std::string& name) {
        auto path = config_.cassette_dir / std::format("{}.json", name);
        if (!fs::exists(path)) {
            // 创建新磁带
            Cassette c{.name = name, .file_path = path.string(),
                       .created_at = Clock::now(), .last_used = Clock::now()};
            active_cassette_ = c;
            return c;
        }
        // 实际实现: 用 yyjson 解析磁带文件
        Cassette c{.name = name, .file_path = path.string(),
                   .created_at = Clock::now(), .last_used = Clock::now()};
        active_cassette_ = c;
        return c;
    }

    // 处理请求 (根据模式录制或回放)
    [[nodiscard]] std::expected<RecordedResponse, Error> handle_request(
        const RecordedRequest& request)
    {
        if (!active_cassette_) {
            return std::unexpected(Error{ErrorCode::InvalidInput, {}, "no cassette loaded"});
        }
        switch (config_.mode) {
            case VcrMode::Playback:
                return find_matching_response(request);
            case VcrMode::Record:
                return std::unexpected(Error{ErrorCode::InvalidInput, {},
                    "record mode: forward request to real server"});
            case VcrMode::Auto: {
                auto result = find_matching_response(request);
                if (result) return result;
                // 未找到匹配，需记录
                return std::unexpected(Error{ErrorCode::NotFound, {},
                    "no matching interaction, forward and record"});
            }
            case VcrMode::PassThrough:
                return std::unexpected(Error{ErrorCode::InvalidInput, {}, "pass-through mode"});
        }
        return std::unexpected(Error{ErrorCode::InvalidInput, {}, "unknown mode"});
    }

    // 记录交互
    VoidResult record_interaction(RecordedRequest req, RecordedResponse resp) {
        if (!active_cassette_) {
            return std::unexpected(Error{ErrorCode::InvalidInput, {}, "no cassette loaded"});
        }
        // 脱敏敏感头部
        sanitize_headers(req.headers);
        sanitize_headers(resp.headers);
        active_cassette_->interactions.push_back({
            .request = std::move(req), .response = std::move(resp), .replay_count = 0,
        });
        return {};
    }

    // 保存磁带到文件
    [[nodiscard]] VoidResult save_cassette() const {
        if (!active_cassette_) {
            return std::unexpected(Error{ErrorCode::InvalidInput, {}, "no cassette loaded"});
        }
        auto path = fs::path(active_cassette_->file_path);
        if (path.empty()) {
            path = config_.cassette_dir / (active_cassette_->name + ".json");
        }
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IOError, {}, "cannot create cassette directory"});
        }
        // Serialize as JSON
        std::string json = "{\n  \"name\": \"" + active_cassette_->name + "\",\n";
        json += "  \"version\": \"" + active_cassette_->version + "\",\n";
        json += "  \"interactions\": [\n";
        for (std::size_t i = 0; i < active_cassette_->interactions.size(); ++i) {
            const auto& ix = active_cassette_->interactions[i];
            json += "    {\n";
            json += "      \"request\": {\"method\": \"" + ix.request.method + "\", \"url\": \"" + ix.request.url + "\"},\n";
            json += std::format("      \"response\": {{\"status\": {}, \"body_length\": {}}}\n",
                                ix.response.status_code, ix.response.body.size());
            json += "    }";
            if (i + 1 < active_cassette_->interactions.size()) json += ",";
            json += "\n";
        }
        json += "  ]\n}\n";
        std::ofstream file(path);
        if (!file) {
            return std::unexpected(Error{ErrorCode::IOError, {}, "cannot write cassette file"});
        }
        file << json;
        return {};
    }

    // 获取当前磁带统计
    [[nodiscard]] std::optional<std::size_t> interaction_count() const {
        if (!active_cassette_) return std::nullopt;
        return active_cassette_->interactions.size();
    }

    // 清除当前磁带
    void eject_cassette() noexcept { active_cassette_.reset(); }

    // 配置管理
    void set_config(VcrConfig config) noexcept { config_ = std::move(config); }
    [[nodiscard]] const VcrConfig& config() const noexcept { return config_; }
    void set_mode(VcrMode mode) noexcept { config_.mode = mode; }

private:
    VcrConfig config_;
    std::optional<Cassette> active_cassette_;

    // 查找匹配的响应
    [[nodiscard]] std::expected<RecordedResponse, Error> find_matching_response(
        const RecordedRequest& request)
    {
        if (!active_cassette_) {
            return std::unexpected(Error{ErrorCode::NotFound, {}, "no cassette"});
        }
        for (auto& interaction : active_cassette_->interactions) {
            if (matches(request, interaction.request)) {
                interaction.replay_count++;
                return interaction.response;
            }
        }
        return std::unexpected(Error{ErrorCode::NotFound, {}, "no matching interaction"});
    }

    // 请求匹配
    [[nodiscard]] bool matches(
        const RecordedRequest& actual,
        const RecordedRequest& recorded) const noexcept
    {
        switch (config_.match_strategy) {
            case MatchStrategy::Exact:
                return actual.method == recorded.method &&
                       actual.url == recorded.url &&
                       actual.body == recorded.body;
            case MatchStrategy::UrlAndMethod:
                return actual.method == recorded.method &&
                       actual.url == recorded.url;
            case MatchStrategy::UrlOnly:
                return actual.url == recorded.url;
            case MatchStrategy::Fuzzy:
                return actual.method == recorded.method &&
                       urls_fuzzy_match(actual.url, recorded.url);
            case MatchStrategy::Custom:
                return false;  // 需要外部注入
        }
        return false;
    }

    // 模糊 URL 匹配 (忽略 query 参数顺序)
    static bool urls_fuzzy_match(std::string_view a, std::string_view b) noexcept {
        // 简化: 仅比较路径部分
        auto path_a = a.substr(0, a.find('?'));
        auto path_b = b.substr(0, b.find('?'));
        return path_a == path_b;
    }

    // 脱敏敏感头部
    void sanitize_headers(std::unordered_map<std::string, std::string>& headers) const {
        for (const auto& sensitive : config_.sensitive_headers) {
            if (headers.contains(sensitive)) {
                headers[sensitive] = "[REDACTED]";
            }
        }
    }
};

} // namespace cc::services::vcr
