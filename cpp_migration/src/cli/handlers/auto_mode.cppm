module;
#include <string>
#include <string_view>
#include <map>
#include <expected>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <format>

export module cc.cli.handlers.auto_mode;
import cc.utils.bash_execution;

export namespace cc::cli::handlers {

// Configuration for auto mode execution
struct AutoModeConfig {
    int max_turns;
    bool allow_destructive;
    std::string model;
};

bool is_auto_mode_available();
AutoModeConfig get_auto_mode_config();

namespace detail {

/// Execute a single turn: send prompt to API, return response text
inline std::expected<std::string, std::string> execute_turn(
    const std::string& prompt, const std::string& model, const std::string& api_key) {

    // JSON-escape the prompt
    std::string escaped;
    escaped.reserve(prompt.size() + 32);
    for (char c : prompt) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped += c; break;
        }
    }

    std::string base_url = "https://api.anthropic.com";
    if (const char* url_env = std::getenv("ANTHROPIC_BASE_URL"); url_env && url_env[0] != '\0') {
        base_url = url_env;
    }

    std::string body = std::format(
        R"({{"model":"{}","max_tokens":4096,"messages":[{{"role":"user","content":"{}"}}]}})",
        model, escaped);

    std::string cmd = std::format(
        "curl -sS -X POST '{}/v1/messages' "
        "-H 'Content-Type: application/json' "
        "-H 'x-api-key: {}' "
        "-H 'anthropic-version: 2023-06-01' "
        "-d '{}' 2>&1",
        base_url, api_key, body);

    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) {
        return std::unexpected("Failed to execute API request");
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
    }
    cc::utils::bash::pclose_spawn(pipe);

    if (output.empty()) {
        return std::unexpected("Empty response from API");
    }

    // Check for stop_reason to determine if we should continue
    // "stop_reason":"end_turn" means the model is done
    // "stop_reason":"tool_use" means there are tool calls to execute
    return output;
}

/// Check if response indicates tool use (requires another turn)
inline bool has_tool_calls(const std::string& response) {
    return response.find("\"tool_use\"") != std::string::npos ||
           response.find("\"stop_reason\":\"tool_use\"") != std::string::npos;
}

/// Check if response indicates completion
inline bool is_complete(const std::string& response) {
    return response.find("\"stop_reason\":\"end_turn\"") != std::string::npos;
}

/// Extract text content from response
inline std::string extract_text(const std::string& response) {
    auto pos = response.find("\"text\":\"");
    if (pos == std::string::npos) return response;

    auto start = pos + 8;
    std::string result;
    for (size_t i = start; i < response.size(); ++i) {
        if (response[i] == '\\' && i + 1 < response.size()) {
            ++i;
            switch (response[i]) {
                case 'n': result += '\n'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default: result += response[i]; break;
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

// Handle auto mode execution — runs Claude non-interactively
std::expected<void, std::string> handle_auto_mode(std::string_view prompt, std::map<std::string, std::string> options) {
    if (prompt.empty()) {
        return std::unexpected("Prompt cannot be empty for auto mode");
    }

    // Validate auto mode availability
    if (!is_auto_mode_available()) {
        return std::unexpected("Auto mode is not available. Ensure API key is configured.");
    }

    auto config = get_auto_mode_config();

    // Apply any option overrides
    if (auto it = options.find("max_turns"); it != options.end()) {
        try {
            config.max_turns = std::stoi(it->second);
        } catch (...) {
            return std::unexpected("Invalid max_turns value: " + it->second);
        }
    }
    if (auto it = options.find("model"); it != options.end()) {
        config.model = it->second;
    }
    if (auto it = options.find("allow_destructive"); it != options.end()) {
        config.allow_destructive = (it->second == "true" || it->second == "1");
    }

    const char* api_key_env = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key_env || api_key_env[0] == '\0') {
        return std::unexpected("ANTHROPIC_API_KEY not set");
    }
    std::string api_key(api_key_env);

    // Execute the auto mode loop
    int turn_count = 0;
    std::string current_prompt(prompt);

    while (turn_count < config.max_turns) {
        ++turn_count;

        auto response = detail::execute_turn(current_prompt, config.model, api_key);
        if (!response) {
            return std::unexpected(std::format("Turn {} failed: {}",
                turn_count, response.error()));
        }

        // Check if the model is done (end_turn) or needs tool execution
        if (detail::is_complete(*response)) {
            // Model completed its response — we're done
            break;
        }

        if (detail::has_tool_calls(*response)) {
            // In a full implementation: parse tool_use blocks, execute them,
            // build tool_result messages, and continue the conversation.
            // For now, stop if we can't process tool calls without the full tool system.
            break;
        }

        // If neither end_turn nor tool_use, something unexpected happened
        break;
    }

    return {};
}

// Check if auto mode prerequisites are met
bool is_auto_mode_available() {
    // Auto mode requires valid API credentials
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    return key != nullptr && key[0] != '\0';
}

// Get the current auto mode configuration
AutoModeConfig get_auto_mode_config() {
    AutoModeConfig config{
        .max_turns = 10,
        .allow_destructive = false,
        .model = "claude-sonnet-4-20250514"
    };

    // Check environment for overrides
    if (const char* turns = std::getenv("CLAUDE_AUTO_MAX_TURNS"); turns && turns[0] != '\0') {
        try { config.max_turns = std::stoi(turns); } catch (...) {}
    }
    if (const char* model = std::getenv("CLAUDE_MODEL"); model && model[0] != '\0') {
        config.model = model;
    }

    return config;
}

} // namespace cc::cli::handlers
