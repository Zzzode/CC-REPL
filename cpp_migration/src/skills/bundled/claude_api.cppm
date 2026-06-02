module;
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <functional>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <sstream>

export module cc.skills.bundled.claude_api;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

namespace detail {

/// Get API key from environment
inline std::string get_api_key() {
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    return key ? std::string(key) : std::string{};
}

/// Get base URL (allows overriding for testing/proxy)
inline std::string get_base_url() {
    const char* url = std::getenv("ANTHROPIC_BASE_URL");
    return url ? std::string(url) : "https://api.anthropic.com";
}

/// Execute a curl command and capture stdout
inline std::expected<std::string, std::string> exec_curl(const std::string& cmd) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to execute curl");
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
    }

    int status = ::pclose(pipe);
    if (status != 0 && output.empty()) {
        return std::unexpected("curl exited with non-zero status");
    }
    return output;
}

/// Escape a string for embedding in JSON
inline std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned>(c));
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/// Extract text content from API response JSON
inline std::string extract_content(const std::string& response) {
    // Look for "text":"..." in the content array
    auto text_key = response.find("\"text\"");
    if (text_key == std::string::npos) {
        // Check for error
        auto err_key = response.find("\"error\"");
        if (err_key != std::string::npos) {
            auto msg_key = response.find("\"message\"", err_key);
            if (msg_key != std::string::npos) {
                auto start = response.find('"', msg_key + 9);
                if (start != std::string::npos) {
                    auto end = response.find('"', start + 1);
                    if (end != std::string::npos) {
                        return "[API Error] " + response.substr(start + 1, end - start - 1);
                    }
                }
            }
        }
        return response; // Return raw if can't parse
    }

    // Find the value after "text":"
    auto colon = response.find(':', text_key);
    if (colon == std::string::npos) return response;
    auto quote_start = response.find('"', colon + 1);
    if (quote_start == std::string::npos) return response;

    // Parse escaped string
    std::string result;
    for (size_t i = quote_start + 1; i < response.size(); ++i) {
        if (response[i] == '\\' && i + 1 < response.size()) {
            ++i;
            switch (response[i]) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default: result += '\\'; result += response[i]; break;
            }
        } else if (response[i] == '"') {
            break;
        } else {
            result += response[i];
        }
    }
    return result;
}

} // namespace detail

// Make a direct synchronous API call to Claude
std::expected<std::string, std::string> direct_api_call(std::string_view prompt, std::optional<std::string> model) {
    if (prompt.empty()) {
        return std::unexpected("Prompt cannot be empty");
    }

    auto api_key = detail::get_api_key();
    if (api_key.empty()) {
        return std::unexpected("ANTHROPIC_API_KEY environment variable not set");
    }

    std::string selected_model = model.value_or("claude-sonnet-4-20250514");
    std::string base_url = detail::get_base_url();
    std::string escaped_prompt = detail::json_escape(prompt);

    std::string body = "{\"model\":\"" + selected_model + "\","
        "\"max_tokens\":4096,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"" + escaped_prompt + "\"}]}";

    // Build curl command
    std::string cmd = "curl -sS -X POST '" + base_url + "/v1/messages' "
        "-H 'Content-Type: application/json' "
        "-H 'x-api-key: " + api_key + "' "
        "-H 'anthropic-version: 2023-06-01' "
        "-d '" + body + "' 2>&1";

    auto response = detail::exec_curl(cmd);
    if (!response) {
        return std::unexpected(response.error());
    }

    return detail::extract_content(*response);
}

// Make a streaming API call, invoking callback for each received chunk
std::expected<void, std::string> stream_api_call(std::string_view prompt, std::function<void(std::string_view)> on_chunk) {
    if (prompt.empty()) {
        return std::unexpected("Prompt cannot be empty");
    }

    if (!on_chunk) {
        return std::unexpected("Chunk callback is required for streaming");
    }

    auto api_key = detail::get_api_key();
    if (api_key.empty()) {
        return std::unexpected("ANTHROPIC_API_KEY environment variable not set");
    }

    std::string base_url = detail::get_base_url();
    std::string escaped_prompt = detail::json_escape(prompt);

    std::string body = "{\"model\":\"claude-sonnet-4-20250514\","
        "\"max_tokens\":4096,\"stream\":true,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"" + escaped_prompt + "\"}]}";

    // Use curl with streaming (--no-buffer for real-time output)
    std::string cmd = "curl -sS -N -X POST '" + base_url + "/v1/messages' "
        "-H 'Content-Type: application/json' "
        "-H 'x-api-key: " + api_key + "' "
        "-H 'anthropic-version: 2023-06-01' "
        "-d '" + body + "' 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to start streaming request");
    }

    // Read SSE events line by line
    std::array<char, 4096> buffer{};
    std::string line_buffer;

    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        line_buffer += buffer.data();

        // Process complete lines
        size_t newline_pos;
        while ((newline_pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = line_buffer.substr(0, newline_pos);
            line_buffer.erase(0, newline_pos + 1);

            // SSE format: "data: {...}"
            if (line.starts_with("data: ")) {
                std::string_view data(line.data() + 6, line.size() - 6);

                // Look for content_block_delta with text
                auto text_pos = data.find("\"text\":\"");
                if (text_pos != std::string_view::npos) {
                    auto start = text_pos + 8;
                    std::string text;
                    for (size_t i = start; i < data.size(); ++i) {
                        if (data[i] == '\\' && i + 1 < data.size()) {
                            ++i;
                            switch (data[i]) {
                                case 'n': text += '\n'; break;
                                case 't': text += '\t'; break;
                                case '"': text += '"'; break;
                                case '\\': text += '\\'; break;
                                default: text += '\\'; text += data[i]; break;
                            }
                        } else if (data[i] == '"') {
                            break;
                        } else {
                            text += data[i];
                        }
                    }
                    if (!text.empty()) {
                        on_chunk(text);
                    }
                }
            }
        }
    }

    ::pclose(pipe);
    return {};
}

// Get the skill manifest for the Claude API skill
cc::skills::SkillManifest get_claude_api_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "claude_api",
        .description = "Direct API access to Claude for programmatic queries",
        .version = "1.0.0",
        .triggers = {"call api", "direct api", "ask claude", "api call"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
