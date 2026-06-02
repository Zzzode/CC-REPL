/// @file dump_prompts.cppm
/// @brief Prompt debugging: dump current prompt state to file and load back.
/// Supports redaction of secrets and configurable output directories.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <format>
#include <regex>
#include <algorithm>

export module cc.services.api.dump_prompts;

export namespace cc::services::api {

namespace fs = std::filesystem;

/// Prompt dump configuration
struct PromptDumpConfig {
    std::string output_dir;
    bool include_system_prompt{true};
    bool include_messages{true};
    bool redact_secrets{true};
};

/// Redact potential secrets from text content
[[nodiscard]] inline std::string redact_content(std::string_view text) {
    std::string result(text);

    // Redact API keys (sk-..., key-..., etc.)
    static const std::regex api_key_pattern(
        R"((sk-|key-|token-|pat-|ghp_|gho_|github_pat_)[A-Za-z0-9_\-]{10,})",
        std::regex::optimize);
    result = std::regex_replace(result, api_key_pattern, "$1[REDACTED]");

    // Redact bearer tokens in headers
    static const std::regex bearer_pattern(
        R"((Bearer\s+)[A-Za-z0-9_\-\.]{10,})",
        std::regex::optimize | std::regex::icase);
    result = std::regex_replace(result, bearer_pattern, "$1[REDACTED]");

    // Redact environment variable values that look like secrets
    static const std::regex env_secret_pattern(
        R"(([A-Z_]*(?:SECRET|TOKEN|KEY|PASSWORD|PASSWD|CREDENTIALS)[A-Z_]*\s*=\s*)[^\s\n]+)",
        std::regex::optimize);
    result = std::regex_replace(result, env_secret_pattern, "$1[REDACTED]");

    return result;
}

/// Generate a timestamped filename for prompt dumps
[[nodiscard]] inline std::string generate_dump_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

    std::tm tm_buf{};
    localtime_r(&time_t, &tm_buf);

    return std::format("prompt_dump_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}_{:03d}.json",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms));
}

/// Dump the current prompt state for debugging
/// Returns the path of the dumped file on success
[[nodiscard]] inline std::optional<std::string> dump_prompts(
    const PromptDumpConfig& config,
    const std::vector<std::string>& messages) {

    // Determine output directory
    fs::path out_dir = config.output_dir.empty()
        ? fs::temp_directory_path() / "cc-repl-dumps"
        : fs::path(config.output_dir);

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) return std::nullopt;

    auto filename = generate_dump_filename();
    auto filepath = out_dir / filename;

    std::ofstream file(filepath);
    if (!file.is_open()) return std::nullopt;

    // Write as JSON array
    file << "{\n";
    file << "  \"timestamp\": \"" << generate_dump_filename().substr(12, 15) << "\",\n";
    file << "  \"message_count\": " << messages.size() << ",\n";
    file << "  \"config\": {\n";
    file << "    \"include_system_prompt\": " << (config.include_system_prompt ? "true" : "false") << ",\n";
    file << "    \"include_messages\": " << (config.include_messages ? "true" : "false") << ",\n";
    file << "    \"redact_secrets\": " << (config.redact_secrets ? "true" : "false") << "\n";
    file << "  },\n";
    file << "  \"messages\": [\n";

    for (std::size_t i = 0; i < messages.size(); ++i) {
        auto content = config.redact_secrets
            ? redact_content(messages[i])
            : messages[i];

        // Escape JSON string
        std::string escaped;
        escaped.reserve(content.size() + 16);
        for (char ch : content) {
            switch (ch) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:   escaped += ch; break;
            }
        }

        file << "    \"" << escaped << "\"";
        if (i + 1 < messages.size()) file << ",";
        file << "\n";
    }

    file << "  ]\n";
    file << "}\n";

    file.flush();
    if (!file.good()) return std::nullopt;

    return filepath.string();
}

/// Load a previously dumped prompt
/// Returns the messages from the dump file
[[nodiscard]] inline std::optional<std::vector<std::string>> load_dumped_prompts(
    std::string_view dump_path) {

    fs::path path(dump_path);
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Simple JSON array extraction: find "messages": [...] section
    auto messages_pos = content.find("\"messages\":");
    if (messages_pos == std::string::npos) return std::nullopt;

    auto array_start = content.find('[', messages_pos);
    if (array_start == std::string::npos) return std::nullopt;

    // Parse the JSON string array manually
    std::vector<std::string> messages;
    std::size_t pos = array_start + 1;

    while (pos < content.size()) {
        // Skip whitespace
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t' || content[pos] == ',')) {
            ++pos;
        }

        if (pos >= content.size() || content[pos] == ']') break;

        if (content[pos] != '"') {
            ++pos;
            continue;
        }

        // Parse JSON string
        ++pos; // skip opening quote
        std::string msg;
        while (pos < content.size() && content[pos] != '"') {
            if (content[pos] == '\\' && pos + 1 < content.size()) {
                ++pos;
                switch (content[pos]) {
                    case 'n':  msg += '\n'; break;
                    case 'r':  msg += '\r'; break;
                    case 't':  msg += '\t'; break;
                    case '"':  msg += '"'; break;
                    case '\\': msg += '\\'; break;
                    default:   msg += content[pos]; break;
                }
            } else {
                msg += content[pos];
            }
            ++pos;
        }
        if (pos < content.size()) ++pos; // skip closing quote

        messages.push_back(std::move(msg));
    }

    return messages;
}

/// List all dump files in the output directory
[[nodiscard]] inline std::vector<std::string> list_dump_files(
    std::string_view output_dir = "") {

    fs::path dir = output_dir.empty()
        ? fs::temp_directory_path() / "cc-repl-dumps"
        : fs::path(output_dir);

    std::vector<std::string> files;
    if (!fs::exists(dir)) return files;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".json" &&
            entry.path().filename().string().starts_with("prompt_dump_")) {
            files.push_back(entry.path().string());
        }
    }

    std::ranges::sort(files);
    return files;
}

} // namespace cc::services::api
