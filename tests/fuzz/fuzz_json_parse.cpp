// Fuzz target: feeds random bytes into yyjson parse + message extraction
// Build with -fsanitize=fuzzer when ENABLE_FUZZING is ON.

import cc.utils.json;

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace cc::utils::json;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);

    // Attempt to parse as JSON.
    auto result = parse(input);
    if (!result.has_value()) {
        return 0;  // Invalid JSON is expected; just ensure no crash.
    }

    // Exercise message extraction patterns typical in SSE event handling:
    // Look for content_block_delta -> delta -> text (Anthropic streaming format).
    auto& doc = result.value();
    auto root = doc.root();

    if (root.is_obj()) {
        // Extract typical fields to exercise accessor paths.
        [[maybe_unused]] auto type_str = root.get_string("type");
        [[maybe_unused]] auto index_val = root.get_int("index");

        // Try nested object access: delta.text
        if (auto delta = root.get_object("delta"); delta.has_value()) {
            [[maybe_unused]] auto text = delta->get_string("text");
            [[maybe_unused]] auto stop = delta->get_string("stop_reason");
        }

        // Try nested object access: message.content[0].text
        if (auto message = root.get_object("message"); message.has_value()) {
            auto content = message->get("content");
            if (content.is_arr() && content.size() > 0) {
                auto first = content.at(0);
                if (first.is_obj()) {
                    [[maybe_unused]] auto text = first.get_string("text");
                }
            }
        }

        // Try usage object access
        if (auto usage = root.get_object("usage"); usage.has_value()) {
            [[maybe_unused]] auto input_tokens = usage->get_int("input_tokens");
            [[maybe_unused]] auto output_tokens = usage->get_int("output_tokens");
        }
    }

    return 0;
}
