module;

#include <cstddef>
#include <cstdint>
#include <string_view>

export module cc.services.token_estimation;


export namespace cc::services {


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


enum class ImageDetail { low, high, auto_detect };


class TokenEstimator {
public:

    [[nodiscard]] static auto estimate_text(std::string_view text) -> size_t {
        size_t count = 0;
        size_t i = 0;
        while (i < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0xE0) {

                count += 1;
                i += (c >= 0xF0) ? 4 : 3;
            } else if (c >= 0xC0) {
                count += 1;
                i += 2;
            } else {

                size_t ascii_run = 0;
                while (i < text.size() && static_cast<unsigned char>(text[i]) < 0x80) {
                    ++i; ++ascii_run;
                }
                count += (ascii_run + 3) / 4;
            }
        }
        return count == 0 ? 1 : count;
    }


    [[nodiscard]] static auto estimate_image(int width, int height, ImageDetail detail = ImageDetail::high) -> size_t {
        if (detail == ImageDetail::low) return 85;

        int tiles_w = (width + 511) / 512;
        int tiles_h = (height + 511) / 512;
        return static_cast<size_t>(170 * tiles_w * tiles_h + 85);
    }


    [[nodiscard]] static auto estimate_tool_use(std::string_view tool_name, std::string_view input_json) -> size_t {

        return 50 + estimate_text(tool_name) + estimate_text(input_json);
    }


    [[nodiscard]] static auto estimate_tool_result(std::string_view output) -> size_t {
        return 10 + estimate_text(output);
    }


    [[nodiscard]] static auto get_model_limit(std::string_view model_id) -> size_t {

        if (model_id.find("claude-4") != std::string_view::npos ||
            model_id.find("claude-sonnet-4") != std::string_view::npos ||
            model_id.find("claude-opus-4") != std::string_view::npos)
            return 200'000;

        if (model_id.find("claude-3-5") != std::string_view::npos) return 200'000;

        if (model_id.find("claude-3") != std::string_view::npos) return 200'000;

        return 100'000;
    }


    [[nodiscard]] static auto fits_in_context(size_t estimated_tokens, std::string_view model_id) -> bool {
        return estimated_tokens < get_model_limit(model_id);
    }
};

} // namespace cc::services
