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


enum class BriefFormat {
    Bullet,
    Narrative,
    Structured,
};

constexpr auto format_name(BriefFormat fmt) -> std::string_view {
    switch (fmt) {
        case BriefFormat::Bullet:     return "bullet";
        case BriefFormat::Narrative:  return "narrative";
        case BriefFormat::Structured: return "structured";
        default:                      return "unknown";
    }
}


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


struct Attachment {
    std::filesystem::path path;
    std::string content;
    std::string mime_type;
    size_t size_bytes{0};
};


struct BriefRequest {
    std::string content;
    std::vector<std::filesystem::path> attachments;
    BriefFormat format{BriefFormat::Bullet};
    size_t token_budget{500};
    std::optional<std::string> focus_area;
};


struct BriefResult {
    std::string summary;
    size_t input_tokens{0};
    size_t output_tokens{0};
    std::vector<std::string> key_points;
    BriefFormat format_used;
};


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

        for (const auto& path : request.attachments) {
            if (!std::filesystem::exists(path)) {
                return std::unexpected(BriefError::AttachmentNotFound);
            }
        }
        return {};
    }

    auto execute(BriefRequest request) -> std::expected<BriefResult, BriefError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());


        std::string full_content = request.content;


        for (const auto& path : request.attachments) {
            auto attachment = read_attachment(path);
            if (!attachment) return std::unexpected(attachment.error());
            full_content += std::format("\n\n--- Attachment: {} ---\n{}", 
                path.filename().string(), attachment->content);
        }

        size_t input_tokens = TokenEstimator::estimate(full_content);
        size_t budget = std::min(request.token_budget, kMaxTokenBudget);


        auto summary = generate_summary(full_content, request.format, budget, request.focus_area);


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


    auto generate_summary(std::string_view content, BriefFormat format,
                          size_t budget, std::optional<std::string> focus) const -> std::string
    {
        size_t max_chars = TokenEstimator::chars_for_tokens(budget);

        auto truncated = content.substr(0, std::min(content.size(), max_chars * 4));
        const auto focus_suffix = focus ? std::format(" focused on {}", *focus) : std::string{};

        switch (format) {
            case BriefFormat::Bullet:
                return std::format("• Summary of {} chars content{} (budget: {} tokens)",
                    content.size(), focus_suffix, budget);
            case BriefFormat::Narrative:
                return std::format("Summary: Content of {} characters{} has been analyzed.",
                    content.size(), focus_suffix);
            case BriefFormat::Structured:
                return std::format("# Summary\n## Overview\nContent length: {} chars{}\n## Key Points\n(generated)",
                    content.size(), focus_suffix);
            default:
                return std::string(truncated.substr(0, max_chars));
        }
    }


    auto extract_key_points(std::string_view content, size_t max_points) const
        -> std::vector<std::string>
    {
        std::vector<std::string> points;


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
