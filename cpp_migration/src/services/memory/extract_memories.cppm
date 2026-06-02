/// @file extract_memories.cppm
/// @brief 从对话中提取重要记忆，用于长期存储。
/// 分析对话轮次以识别关键事实、决策、偏好；对记忆进行分类和去重；
/// 持久化到 ~/.cc-repl/memory/ 目录。
module;

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <algorithm>
#include <functional>
#include <coroutine>

export module cc.services.extract_memories;

import cc.types.types;
import cc.utils.async;
import cc.utils.error;
import cc.utils.json;
import cc.utils.file;

export namespace cc::services::extract_memories {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::async::Task;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

namespace fs = std::filesystem;

// ============================================================
// 记忆分类枚举
// ============================================================

/// 记忆类别 - 区分不同类型的提取信息
enum class MemoryCategory : std::uint8_t {
    ProjectFact,       // 项目相关的事实 (架构、依赖、约定)
    UserPreference,    // 用户偏好 (代码风格、工具选择)
    Decision,          // 技术决策 (选型、权衡)
    CodePattern,       // 代码模式 (常用 pattern、反模式)
};

/// 将分类转换为可读字符串
[[nodiscard]] constexpr std::string_view category_to_string(MemoryCategory cat) noexcept {
    switch (cat) {
        case MemoryCategory::ProjectFact:    return "project_fact";
        case MemoryCategory::UserPreference: return "user_preference";
        case MemoryCategory::Decision:       return "decision";
        case MemoryCategory::CodePattern:    return "code_pattern";
    }
    return "unknown";
}

/// 从字符串解析分类
[[nodiscard]] constexpr std::optional<MemoryCategory> category_from_string(std::string_view s) noexcept {
    if (s == "project_fact")    return MemoryCategory::ProjectFact;
    if (s == "user_preference") return MemoryCategory::UserPreference;
    if (s == "decision")        return MemoryCategory::Decision;
    if (s == "code_pattern")    return MemoryCategory::CodePattern;
    return std::nullopt;
}

[[nodiscard]] std::string escape_json_string(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '"': escaped += R"(\")"; break;
            case '\\': escaped += R"(\\)"; break;
            case '\n': escaped += R"(\n)"; break;
            case '\r': escaped += R"(\r)"; break;
            case '\t': escaped += R"(\t)"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    escaped += ' ';
                } else {
                    escaped += ch;
                }
                break;
        }
    }
    return escaped;
}

// ============================================================
// 核心数据结构
// ============================================================

/// 提取出的单条记忆
struct ExtractedMemory {
    std::string content;           // 记忆内容
    MemoryCategory category;       // 分类
    double confidence;             // 置信度 [0.0, 1.0]
    std::string source_turn_id;    // 来源对话轮次 ID
    TimePoint timestamp;           // 提取时间戳

    /// 序列化为 JSON 字符串
    [[nodiscard]] std::string to_json() const {
        return std::format(
            R"({{"content":"{}","category":"{}","confidence":{},"source_turn_id":"{}","timestamp":{}}})",
            escape_json_string(content), category_to_string(category), confidence,
            escape_json_string(source_turn_id),
            std::chrono::duration_cast<std::chrono::seconds>(timestamp.time_since_epoch()).count()
        );
    }
};

/// 提取配置
struct ExtractionConfig {
    double min_confidence = 0.7;       // 最低置信度阈值
    std::size_t max_per_turn = 3;      // 每轮最多提取数
    std::vector<MemoryCategory> categories_filter;  // 空表示不过滤

    /// 检查某分类是否在过滤列表中（空列表 = 允许所有）
    [[nodiscard]] bool allows_category(MemoryCategory cat) const noexcept {
        if (categories_filter.empty()) return true;
        return std::find(categories_filter.begin(), categories_filter.end(), cat) != categories_filter.end();
    }
};

// ============================================================
// 记忆提取器
// ============================================================

/// 记忆提取核心类 - 分析对话并提取、去重、持久化记忆
class MemoryExtractor {
public:
    explicit MemoryExtractor(ExtractionConfig config = {})
        : config_(std::move(config))
        , storage_dir_(resolve_storage_dir()) {}

    ~MemoryExtractor() = default;

    // 禁止拷贝，允许移动
    MemoryExtractor(const MemoryExtractor&) = delete;
    MemoryExtractor& operator=(const MemoryExtractor&) = delete;
    MemoryExtractor(MemoryExtractor&&) noexcept = default;
    MemoryExtractor& operator=(MemoryExtractor&&) noexcept = default;

    /// 从对话轮次中提取记忆
    /// @param messages 当前对话消息列表 (JSON 格式)
    /// @param context 附加上下文信息 (项目路径等)
    /// @return 提取到的记忆列表
    Task<std::vector<ExtractedMemory>> extract_from_turn(
        const std::vector<std::string>& messages,
        std::string_view context) {

        // 构建提取 prompt 并调用本地分析逻辑
        [[maybe_unused]] auto prompt = get_extraction_prompt();
        std::vector<ExtractedMemory> candidates;

        // 采用本地确定性启发式规则，避免后台网络依赖。
        for (const auto& msg : messages) {
            auto extracted = analyze_message(msg, context);
            for (auto& mem : extracted) {
                if (mem.confidence >= config_.min_confidence &&
                    config_.allows_category(mem.category)) {
                    candidates.push_back(std::move(mem));
                }
            }
        }

        // 限制每轮提取上限
        if (candidates.size() > config_.max_per_turn) {
            // 按置信度排序，保留 top N
            std::partial_sort(candidates.begin(),
                candidates.begin() + static_cast<std::ptrdiff_t>(config_.max_per_turn),
                candidates.end(),
                [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
            candidates.resize(config_.max_per_turn);
        }

        // 对已有记忆去重
        auto existing = co_await load_existing_memories();
        candidates = filter_duplicates(candidates, existing);

        co_return candidates;
    }

    /// 持久化单条记忆到磁盘
    Task<std::expected<void, Error>> store_memory(const ExtractedMemory& memory) {
        auto filepath = std::format("{}/{}.json", storage_dir_,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                memory.timestamp.time_since_epoch()).count());

        auto json = memory.to_json();

        std::error_code ec;
        fs::create_directories(storage_dir_, ec);
        if (ec) {
            co_return std::unexpected(Error(ErrorCode::io_error,
                std::format("failed to create memory directory '{}': {}", storage_dir_, ec.message())));
        }

        std::ofstream output(filepath, std::ios::trunc);
        if (!output) {
            co_return std::unexpected(Error(ErrorCode::io_error,
                std::format("failed to open memory file '{}' for writing", filepath)));
        }
        output << json << '\n';
        if (!output) {
            co_return std::unexpected(Error(ErrorCode::io_error,
                std::format("failed to write memory file '{}'", filepath)));
        }
        co_return std::expected<void, Error>{};
    }

    /// 获取用于记忆提取的 system prompt
    [[nodiscard]] std::string get_extraction_prompt() const {
        return std::format(
            "Analyze the conversation and extract key memories.\n"
            "Categories: project_fact, user_preference, decision, code_pattern\n"
            "Minimum confidence threshold: {}\n"
            "Maximum memories per turn: {}\n"
            "Output each memory as JSON with fields: content, category, confidence.",
            config_.min_confidence, config_.max_per_turn
        );
    }

    /// 对候选记忆进行去重，移除与已有记忆语义重复的条目
    [[nodiscard]] std::vector<ExtractedMemory> filter_duplicates(
        const std::vector<ExtractedMemory>& candidates,
        const std::vector<ExtractedMemory>& existing) const {

        std::vector<ExtractedMemory> unique;
        unique.reserve(candidates.size());

        for (const auto& candidate : candidates) {
            bool is_dup = std::any_of(existing.begin(), existing.end(), [&](const auto& ex) {
                return is_semantically_similar(candidate.content, ex.content);
            });
            if (!is_dup) {
                unique.push_back(candidate);
            }
        }
        return unique;
    }

    /// 更新提取配置
    void set_config(ExtractionConfig config) { config_ = std::move(config); }

    /// 获取当前配置
    [[nodiscard]] const ExtractionConfig& config() const noexcept { return config_; }

private:
    /// 解析存储目录路径
    [[nodiscard]] static std::string resolve_storage_dir() {
        if (const char* home = std::getenv("HOME")) {
            return (fs::path(home) / ".cc-repl" / "memory").string();
        }
        return (fs::path(".cc-repl") / "memory").string();
    }

    /// 分析单条消息，提取潜在记忆
    [[nodiscard]] std::vector<ExtractedMemory> analyze_message(
        std::string_view message, std::string_view context) const {

        std::vector<ExtractedMemory> results;

        auto lowered = lowercase(message);
        auto content = normalize_memory_content(message, context);
        if (content.size() < 12) return results;

        auto add_if_allowed = [&](MemoryCategory category, double confidence) {
            if (config_.allows_category(category)) {
                results.push_back(ExtractedMemory{
                    .content = content,
                    .category = category,
                    .confidence = confidence,
                    .source_turn_id = std::format("turn_{:016x}", std::hash<std::string_view>{}(message)),
                    .timestamp = Clock::now(),
                });
            }
        };

        if (contains_any(lowered, {"always", "prefer", "preference", "do not", "don't", "avoid", "use "})) {
            add_if_allowed(MemoryCategory::UserPreference, 0.82);
        }
        if (contains_any(lowered, {"decided", "decision", "we will", "we'll", "chosen", "adopt", "settled on"})) {
            add_if_allowed(MemoryCategory::Decision, 0.84);
        }
        if (contains_any(lowered, {"pattern:", "code pattern", "convention", "anti-pattern", "idiom"})) {
            add_if_allowed(MemoryCategory::CodePattern, 0.8);
        }
        if (contains_any(lowered, {"project", "repo", "repository", "module", "dependency", "build", "cmake", "architecture"})) {
            add_if_allowed(MemoryCategory::ProjectFact, 0.76);
        }
        return results;
    }

    /// 加载已有记忆用于去重比较
    Task<std::vector<ExtractedMemory>> load_existing_memories() {
        std::vector<ExtractedMemory> memories;
        std::error_code ec;
        if (!fs::exists(storage_dir_, ec) || ec) co_return memories;

        for (const auto& entry : fs::directory_iterator(storage_dir_, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;

            std::ifstream input(entry.path());
            if (!input) continue;
            std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            auto parsed = parse_memory_json(json);
            if (parsed) memories.push_back(std::move(*parsed));
        }
        co_return memories;
    }

    [[nodiscard]] static bool contains_any(
        std::string_view haystack,
        std::initializer_list<std::string_view> needles) noexcept {
        return std::any_of(needles.begin(), needles.end(), [&](std::string_view needle) {
            return haystack.find(needle) != std::string_view::npos;
        });
    }

    [[nodiscard]] static std::string lowercase(std::string_view input) {
        std::string out;
        out.reserve(input.size());
        for (unsigned char ch : input) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    }

    [[nodiscard]] static std::string normalize_memory_content(
        std::string_view message,
        std::string_view context) {
        std::string content;
        if (!context.empty()) {
            content += "[";
            content += context.substr(0, std::min<std::size_t>(context.size(), 80));
            content += "] ";
        }

        bool previous_space = false;
        for (unsigned char ch : message) {
            if (std::isspace(ch)) {
                if (!previous_space) content.push_back(' ');
                previous_space = true;
            } else {
                content.push_back(static_cast<char>(ch));
                previous_space = false;
            }
            if (content.size() >= 320) break;
        }

        while (!content.empty() && content.back() == ' ') content.pop_back();
        return content;
    }

    [[nodiscard]] static std::optional<std::string> json_string_field(
        std::string_view json,
        std::string_view key) {
        auto marker = std::format("\"{}\":\"", key);
        auto pos = json.find(marker);
        if (pos == std::string_view::npos) return std::nullopt;
        pos += marker.size();

        std::string value;
        bool escaped = false;
        for (; pos < json.size(); ++pos) {
            char ch = json[pos];
            if (escaped) {
                switch (ch) {
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: value.push_back(ch); break;
                }
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                return value;
            } else {
                value.push_back(ch);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<double> json_number_field(
        std::string_view json,
        std::string_view key) {
        auto marker = std::format("\"{}\":", key);
        auto pos = json.find(marker);
        if (pos == std::string_view::npos) return std::nullopt;
        pos += marker.size();
        auto end = json.find_first_of(",}", pos);
        auto token = std::string(json.substr(pos, end == std::string_view::npos ? json.size() - pos : end - pos));
        try {
            return std::stod(token);
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] static std::optional<ExtractedMemory> parse_memory_json(std::string_view json) {
        auto content = json_string_field(json, "content");
        auto category = json_string_field(json, "category");
        auto confidence = json_number_field(json, "confidence");
        auto source_turn_id = json_string_field(json, "source_turn_id");
        auto timestamp = json_number_field(json, "timestamp");
        if (!content || !category || !confidence || !source_turn_id || !timestamp) return std::nullopt;
        auto parsed_category = category_from_string(*category);
        if (!parsed_category) return std::nullopt;

        return ExtractedMemory{
            .content = std::move(*content),
            .category = *parsed_category,
            .confidence = *confidence,
            .source_turn_id = std::move(*source_turn_id),
            .timestamp = TimePoint(std::chrono::seconds(static_cast<std::int64_t>(*timestamp))),
        };
    }

    /// 简单的语义相似度判断 (基于内容重叠)
    [[nodiscard]] bool is_semantically_similar(
        std::string_view a, std::string_view b) const noexcept {
        // 简化实现: 完全匹配或 80% 子串重叠
        if (a == b) return true;
        if (a.size() < 10 || b.size() < 10) return false;
        // 检查较短串是否是较长串的子串
        auto [shorter, longer] = (a.size() < b.size())
            ? std::pair{a, b} : std::pair{b, a};
        return longer.find(shorter) != std::string_view::npos;
    }

    ExtractionConfig config_;
    std::string storage_dir_;
};

} // namespace cc::services::extract_memories
