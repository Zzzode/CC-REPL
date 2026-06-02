module;

#include <cstddef>
#include <cstdint>
#include <string_view>

export module cc.services.token_estimation;


export namespace cc::services {

// Token 估算结果
struct TokenEstimate {
    size_t input_tokens{0};
    size_t output_tokens{0};
    size_t cache_read_tokens{0};
    size_t total() const { return input_tokens + output_tokens + cache_read_tokens; }
};

struct ContentTokens {
    size_t text_tokens{0};
    size_t image_tokens{0};
    size_t tool_tokens{0};
    size_t total() const { return text_tokens + image_tokens + tool_tokens; }
};

// 图片细节级别
enum class ImageDetail { low, high, auto_detect };

// Token 估算器 — 无需 API 调用的本地快速估算
class TokenEstimator {
public:
    // 估算单条文本的 token 数 (cl100k_base 近似: ~4字符/token, CJK修正: ~2字符/token)
    [[nodiscard]] static auto estimate_text(std::string_view text) -> size_t {
        size_t count = 0;
        size_t i = 0;
        while (i < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0xE0) {
                // CJK/多字节字符: 约 2 字符 = 1 token
                count += 1;
                i += (c >= 0xF0) ? 4 : 3;
            } else if (c >= 0xC0) {
                count += 1;
                i += 2;
            } else {
                // ASCII: 约 4 字符 = 1 token
                size_t ascii_run = 0;
                while (i < text.size() && static_cast<unsigned char>(text[i]) < 0x80) {
                    ++i; ++ascii_run;
                }
                count += (ascii_run + 3) / 4;  // 向上取整
            }
        }
        return count == 0 ? 1 : count;
    }

    // 估算图片 token (基于分辨率分块)
    [[nodiscard]] static auto estimate_image(int width, int height, ImageDetail detail = ImageDetail::high) -> size_t {
        if (detail == ImageDetail::low) return 85;
        // 高细节: 每 512x512 块 = 170 tokens, 加基础 85
        int tiles_w = (width + 511) / 512;
        int tiles_h = (height + 511) / 512;
        return static_cast<size_t>(170 * tiles_w * tiles_h + 85);
    }

    // 估算工具调用的 token 开销
    [[nodiscard]] static auto estimate_tool_use(std::string_view tool_name, std::string_view input_json) -> size_t {
        // 工具名 + JSON schema 开销 ~50 tokens, 加上 input 本身
        return 50 + estimate_text(tool_name) + estimate_text(input_json);
    }

    // 估算工具结果
    [[nodiscard]] static auto estimate_tool_result(std::string_view output) -> size_t {
        return 10 + estimate_text(output);  // 10 tokens 结构开销
    }

    // 获取模型上下文窗口限制
    [[nodiscard]] static auto get_model_limit(std::string_view model_id) -> size_t {
        // Claude 4 系列: 200k
        if (model_id.find("claude-4") != std::string_view::npos ||
            model_id.find("claude-sonnet-4") != std::string_view::npos ||
            model_id.find("claude-opus-4") != std::string_view::npos)
            return 200'000;
        // Claude 3.5 系列: 200k
        if (model_id.find("claude-3-5") != std::string_view::npos) return 200'000;
        // Claude 3 系列: 200k (opus/sonnet), 200k (haiku)
        if (model_id.find("claude-3") != std::string_view::npos) return 200'000;
        // 默认
        return 100'000;
    }

    // 检查消息是否能放入上下文
    [[nodiscard]] static auto fits_in_context(size_t estimated_tokens, std::string_view model_id) -> bool {
        return estimated_tokens < get_model_limit(model_id);
    }
};

} // namespace cc::services
