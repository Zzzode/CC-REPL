// BriefTool - Generates summaries/briefs with format and token budget control
module;
#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.brief;


export namespace cc::tools {

// 摘要格式类型
enum class BriefFormat {
    Bullet,      // 要点列表
    Narrative,   // 叙述性段落
    Structured,  // 结构化 (标题+小节)
};

constexpr auto format_name(BriefFormat fmt) -> std::string_view {
    switch (fmt) {
        case BriefFormat::Bullet:     return "bullet";
        case BriefFormat::Narrative:  return "narrative";
        case BriefFormat::Structured: return "structured";
        default:                      return "unknown";
    }
}

// 摘要工具错误类型
enum class BriefError {
    ContentEmpty,
    AttachmentNotFound,
    AttachmentReadFailed,
    TokenBudgetTooSmall,
    FormatInvalid,
    GenerationFailed,
};

constexpr auto format_error(BriefError err) -> std::string_view {
    switch (err) {
        case BriefError::ContentEmpty:        return "No content provided for summarization";
        case BriefError::AttachmentNotFound:  return "Attachment file not found";
        case BriefError::AttachmentReadFailed: return "Failed to read attachment file";
        case BriefError::TokenBudgetTooSmall: return "Token budget is too small for meaningful summary";
        case BriefError::FormatInvalid:       return "Invalid summary format specified";
        case BriefError::GenerationFailed:    return "Summary generation failed";
        default:                              return "Unknown brief error";
    }
}

// 附件信息
struct Attachment {
    std::filesystem::path path;
    std::string content;          // 读取后的内容
    std::string mime_type;
    size_t size_bytes{0};
};

// 摘要请求
struct BriefRequest {
    std::string content;                      // 需要摘要的主文本
    std::vector<std::filesystem::path> attachments;  // 附件路径列表
    BriefFormat format{BriefFormat::Bullet};   // 输出格式
    size_t token_budget{500};                 // 摘要最大 token 数
    std::optional<std::string> focus_area;    // 关注方向 (可选)
};

// 摘要结果
struct BriefResult {
    std::string summary;
    size_t input_tokens{0};       // 输入内容的估算 token 数
    size_t output_tokens{0};      // 输出摘要的估算 token 数
    std::vector<std::string> key_points;  // 提取的关键要点
    BriefFormat format_used;
};

// 简单 token 计数器 (粗略估算: 1 token ≈ 4 字符)
class TokenEstimator {
public:
    static constexpr size_t kCharsPerToken = 4;

    static auto estimate(std::string_view text) -> size_t {
        return (text.size() + kCharsPerToken - 1) / kCharsPerToken;
    }

    static auto chars_for_tokens(size_t tokens) -> size_t {
        return tokens * kCharsPerToken;
    }
};

// BriefTool - 生成摘要/简报
class BriefTool {
public:
    static constexpr std::string_view name = "brief";
    static constexpr std::string_view description = "Generate summaries and briefs from content and attachments";
    static constexpr size_t kMinTokenBudget = 50;
    static constexpr size_t kMaxTokenBudget = 4096;

    auto validate(const BriefRequest& request) const -> std::expected<void, BriefError> {
        if (request.content.empty() && request.attachments.empty()) {
            return std::unexpected(BriefError::ContentEmpty);
        }
        if (request.token_budget < kMinTokenBudget) {
            return std::unexpected(BriefError::TokenBudgetTooSmall);
        }
        // 验证附件路径存在性
        for (const auto& path : request.attachments) {
            if (!std::filesystem::exists(path)) {
                return std::unexpected(BriefError::AttachmentNotFound);
            }
        }
        return {};
    }

    auto execute(BriefRequest request) -> std::expected<BriefResult, BriefError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        // 收集所有输入内容
        std::string full_content = request.content;

        // 读取附件并追加到内容中
        for (const auto& path : request.attachments) {
            auto attachment = read_attachment(path);
            if (!attachment) return std::unexpected(attachment.error());
            full_content += std::format("\n\n--- Attachment: {} ---\n{}", 
                path.filename().string(), attachment->content);
        }

        size_t input_tokens = TokenEstimator::estimate(full_content);
        size_t budget = std::min(request.token_budget, kMaxTokenBudget);

        // 根据格式生成摘要框架
        auto summary = generate_summary(full_content, request.format, budget, request.focus_area);

        // 提取关键要点
        auto key_points = extract_key_points(full_content, 5);

        return BriefResult{
            .summary = std::move(summary),
            .input_tokens = input_tokens,
            .output_tokens = TokenEstimator::estimate(summary),
            .key_points = std::move(key_points),
            .format_used = request.format,
        };
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "content": {{ "type": "string", "description": "Text content to summarize" }},
      "attachments": {{ "type": "array", "items": {{ "type": "string" }}, "description": "File paths to include" }},
      "format": {{ "type": "string", "enum": ["bullet", "narrative", "structured"], "description": "Summary format" }},
      "token_budget": {{ "type": "integer", "description": "Maximum tokens for the summary (default 500)" }},
      "focus_area": {{ "type": "string", "description": "Specific area to focus the summary on" }}
    }},
    "required": ["content"]
  }}
}})json", name, description);
    }

private:
    // 读取附件内容
    auto read_attachment(const std::filesystem::path& path) const
        -> std::expected<Attachment, BriefError>
    {
        std::ifstream file(path);
        if (!file) return std::unexpected(BriefError::AttachmentReadFailed);

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        return Attachment{
            .path = path,
            .content = std::move(content),
            .mime_type = "text/plain",
            .size_bytes = std::filesystem::file_size(path),
        };
    }

    // 生成摘要 (实际由 LLM 完成，这里做结构化封装)
    auto generate_summary(std::string_view content, BriefFormat format,
                          size_t budget, std::optional<std::string> focus) const -> std::string
    {
        size_t max_chars = TokenEstimator::chars_for_tokens(budget);
        // 截断输入以适配预算
        auto truncated = content.substr(0, std::min(content.size(), max_chars * 4));

        switch (format) {
            case BriefFormat::Bullet:
                return std::format("• Summary of {} chars content (budget: {} tokens)",
                    content.size(), budget);
            case BriefFormat::Narrative:
                return std::format("Summary: Content of {} characters has been analyzed.",
                    content.size());
            case BriefFormat::Structured:
                return std::format("# Summary\n## Overview\nContent length: {} chars\n## Key Points\n(generated)",
                    content.size());
            default:
                return std::string(truncated.substr(0, max_chars));
        }
    }

    // 提取关键要点 (占位实现)
    auto extract_key_points(std::string_view content, size_t max_points) const
        -> std::vector<std::string>
    {
        std::vector<std::string> points;
        // 实际实现会使用 NLP 或 LLM 提取
        // 简单占位：按行分割取前 N 行非空行
        size_t count = 0;
        size_t pos = 0;
        while (pos < content.size() && count < max_points) {
            auto end = content.find('\n', pos);
            if (end == std::string_view::npos) end = content.size();
            auto line = content.substr(pos, end - pos);
            if (!line.empty() && line.size() > 10) {
                points.emplace_back(line.substr(0, 100));
                ++count;
            }
            pos = end + 1;
        }
        return points;
    }
};

} // namespace cc::tools
