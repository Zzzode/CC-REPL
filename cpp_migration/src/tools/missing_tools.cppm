module;

#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module cc.tools.missing_tools;


export namespace cc::tools {

namespace detail {
[[nodiscard]] inline auto shell_quote(std::string_view s) -> std::string {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += R"('\'')";
        else out += ch;
    }
    out += "'";
    return out;
}

[[nodiscard]] inline auto json_escape(std::string_view input) -> std::string {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '"': out += R"(\")"; break;
            case '\\': out += R"(\\)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out += ch; break;
        }
    }
    return out;
}
}


struct ToolError {
    std::string code;
    std::string message;
};


struct ToolResult {
    bool success{true};
    std::string output;
    std::optional<std::string> error;
    std::optional<std::string> metadata_json;
};

namespace detail {
[[nodiscard]] inline auto run_command(std::string_view command, size_t max_output_bytes) -> ToolResult {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(std::string(command).c_str(), "r");
    if (!pipe) return {.success = false, .error = "failed to spawn command"};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
        if (output.size() > max_output_bytes) {
            output.resize(max_output_bytes);
            output += "\n[output truncated]";
            break;
        }
    }
    const int status = pclose(pipe);
    return {.success = status == 0, .output = output, .error = status == 0 ? std::nullopt : std::optional<std::string>("command exited with non-zero status")};
}
}




enum class ReplLanguage { python, node, ruby, lua };

struct ReplConfig {
    ReplLanguage language{ReplLanguage::python};
    std::string working_dir;
    std::chrono::seconds timeout{30};
    size_t max_output_bytes{100'000};
    bool persist_state{true};
};

class REPLTool {
    struct ReplSession {
        std::string id;
        ReplLanguage language;
        bool active{false};
        std::vector<std::string> history;
    };
    std::vector<ReplSession> sessions_;

public:
    static constexpr auto name() -> std::string_view { return "repl"; }
    static constexpr auto description() -> std::string_view {
        return "在交互式 REPL 环境中执行代码 (Python/Node/Ruby)";
    }

    [[nodiscard]] auto execute(std::string_view code, ReplConfig config = {}) -> ToolResult {
        auto* session = get_or_create_session(config.language);
        session->history.push_back(std::string(code));

        namespace fs = std::filesystem;
        const auto base_dir = config.working_dir.empty()
            ? fs::temp_directory_path()
            : fs::path(config.working_dir);
        if (!fs::exists(base_dir)) {
            return {.success = false, .error = "working directory does not exist: " + base_dir.string()};
        }

        std::string ext;
        std::string interpreter;
        switch (config.language) {
            case ReplLanguage::python: interpreter = "python3"; ext = ".py"; break;
            case ReplLanguage::node: interpreter = "node"; ext = ".js"; break;
            case ReplLanguage::ruby: interpreter = "ruby"; ext = ".rb"; break;
            case ReplLanguage::lua: interpreter = "lua"; ext = ".lua"; break;
        }

        const auto script_path = base_dir / (session->id + ext);
        {
            std::ofstream script(script_path);
            if (!script) return {.success = false, .error = "failed to create REPL script"};
            if (config.persist_state) {
                for (const auto& entry : session->history) script << entry << '\n';
            } else {
                script << code << '\n';
            }
        }

        const auto cmd = "cd " + detail::shell_quote(base_dir.string()) + " && " + interpreter + " " + detail::shell_quote(script_path.string()) + " 2>&1";
        auto result = detail::run_command(cmd, config.max_output_bytes);
        result.metadata_json = R"({"session_id": ")" + session->id + R"(", "language": ")" + interpreter + R"("})";
        return result;
    }

    [[nodiscard]] auto get_history(ReplLanguage lang) const -> std::vector<std::string> {
        for (const auto& s : sessions_) if (s.language == lang) return s.history;
        return {};
    }
    
    void reset(ReplLanguage lang) {
        std::erase_if(sessions_, [&](const auto& s) { return s.language == lang; });
    }

private:
    auto get_or_create_session(ReplLanguage lang) -> ReplSession* {
        for (auto& s : sessions_) if (s.language == lang) return &s;
        sessions_.push_back({.id = "repl_" + std::to_string(sessions_.size()), 
                            .language = lang, .active = true});
        return &sessions_.back();
    }
};



struct RemoteTriggerConfig {
    std::string target_session_id;
    std::string bridge_url;
    std::chrono::seconds timeout{60};
    bool wait_for_result{true};
};

class RemoteTriggerTool {
public:
    static constexpr auto name() -> std::string_view { return "remote_trigger"; }
    static constexpr auto description() -> std::string_view {
        return "触发远程会话执行指定任务";
    }
    
    [[nodiscard]] auto execute(std::string_view task_description, RemoteTriggerConfig config) -> ToolResult {
        if (config.target_session_id.empty())
            return {.success = false, .error = "未指定目标会话 ID"};
        if (config.bridge_url.empty())
            return {.success = false, .error = "未指定 bridge URL"};

        const auto payload = R"({"jsonrpc":"2.0","id":"remote-trigger","method":"execute","params":{"session_id":")" +
            detail::json_escape(config.target_session_id) + R"(","task":")" + detail::json_escape(task_description) + R"(","wait":)" +
            (config.wait_for_result ? "true" : "false") + "}}";
        const auto cmd = "curl -fsS -m " + std::to_string(config.timeout.count()) + " -H 'Content-Type: application/json' --data " +
            detail::shell_quote(payload) + " " + detail::shell_quote(config.bridge_url) + " 2>&1";
        auto result = detail::run_command(cmd, 100'000);
        if (!result.success) return result;
        if (result.success && result.output.empty()) result.output = "远程任务已触发";
        return {.success = true, .output = result.output,
                .metadata_json = R"({"triggered_session": ")" + config.target_session_id + R"("})"};
    }
};



// Skill listing configuration
inline constexpr double SKILL_BUDGET_CONTEXT_PERCENT = 0.01;
inline constexpr size_t CHARS_PER_TOKEN = 4;
inline constexpr size_t DEFAULT_CHAR_BUDGET = 8000; // Fallback: 1% of 200k × 4
inline constexpr size_t MAX_LISTING_DESC_CHARS = 250;
inline constexpr size_t MIN_DESC_LENGTH = 20;

struct SkillInvocation {
    std::string skill_name;
    std::string input;
    std::optional<std::string> variant;
    std::chrono::seconds timeout{120};
};

struct SkillInfo {
    std::string name;
    std::string description;
    std::optional<std::string> when_to_use;
    std::string source;  // "bundled", "plugin", "local"
};

class SkillTool {
public:
    static constexpr auto name() -> std::string_view { return "skill"; }
    static constexpr auto description() -> std::string_view {
        return "调用已安装的技能模块执行复杂工作流";
    }
    
    // Get tool prompt for LLM
    static auto get_prompt() -> std::string {
        return R"(Execute a skill within the main conversation

When users ask you to perform tasks, check if any of the available skills match. Skills provide specialized capabilities and domain knowledge.

When users reference a "slash command" or "/<something>" (e.g., "/commit", "/review-pr"), they are referring to a skill. Use this tool to invoke it.

How to invoke:
- Use this tool with the skill name and optional arguments
- Examples:
  - `skill: "pdf"` - invoke the pdf skill
  - `skill: "commit", args: "-m 'Fix bug'"` - invoke with arguments
  - `skill: "review-pr", args: "123"` - invoke with arguments
  - `skill: "ms-office-suite:pdf"` - invoke using fully qualified name

Important:
- Available skills are listed in system-reminder messages in the conversation
- When a skill matches the user's request, this is a BLOCKING REQUIREMENT: invoke the relevant Skill tool BEFORE generating any other response about the task
- NEVER mention a skill without actually calling this tool
- Do not invoke a skill that is already running
- Do not use this tool for built-in CLI commands (like /help, /clear, etc.)
)";
    }
    
    // Calculate character budget for skill listing
    static auto get_char_budget(std::optional<size_t> context_window_tokens) -> size_t {
        if (const char* override_value = std::getenv("CC_SKILL_CHAR_BUDGET")) {
            char* end = nullptr;
            const auto parsed = std::strtoull(override_value, &end, 10);
            if (end != override_value && parsed > 0) return static_cast<size_t>(parsed);
        }
        if (context_window_tokens) {
            return static_cast<size_t>(
                *context_window_tokens * CHARS_PER_TOKEN * SKILL_BUDGET_CONTEXT_PERCENT
            );
        }
        return DEFAULT_CHAR_BUDGET;
    }
    
    // Format skill commands within budget
    static auto format_commands_within_budget(
        const std::vector<SkillInfo>& skills,
        std::optional<size_t> context_window_tokens
    ) -> std::string {
        if (skills.empty()) return "";

        const auto budget = get_char_budget(context_window_tokens);
        
        // First try full descriptions
        std::vector<std::string> full_entries;
        size_t full_total = 0;
        
        for (const auto& skill : skills) {
            const auto desc = skill.when_to_use 
                ? skill.description + " - " + *skill.when_to_use
                : skill.description;
            
            const auto truncated_desc = desc.length() > MAX_LISTING_DESC_CHARS
                ? desc.substr(0, MAX_LISTING_DESC_CHARS - 1) + "…"
                : desc;
                
            const auto entry = "- " + skill.name + ": " + truncated_desc;
            full_entries.push_back(entry);
            full_total += entry.length();
        }
        
        // Add newline separators
        full_total += skills.size() - 1;
        
        if (full_total <= budget) {
            std::string result;
            for (size_t i = 0; i < full_entries.size(); ++i) {
                if (i > 0) result += "\n";
                result += full_entries[i];
            }
            return result;
        }
        
        // If we need to truncate, separate bundled skills (keep full) and others
        std::vector<bool> is_bundled;
        std::vector<std::string> bundled_entries;
        std::vector<std::string> rest_entries;
        size_t bundled_chars = 0;
        
        for (size_t i = 0; i < skills.size(); ++i) {
            const auto& skill = skills[i];
            const auto& entry = full_entries[i];
            if (skill.source == "bundled") {
                is_bundled.push_back(true);
                bundled_entries.push_back(entry);
                bundled_chars += entry.length() + 1;
            } else {
                is_bundled.push_back(false);
                rest_entries.push_back("- " + skill.name);
            }
        }
        
        const size_t remaining_budget = budget - bundled_chars;
        
        if (rest_entries.empty()) {
            std::string result;
            for (size_t i = 0; i < full_entries.size(); ++i) {
                if (i > 0) result += "\n";
                result += full_entries[i];
            }
            return result;
        }
        
        // Calculate max description length for non-bundled
        size_t rest_name_overhead = 0;
        for (const auto& skill : skills) {
            if (skill.source != "bundled") {
                rest_name_overhead += skill.name.length() + 4;
            }
        }
        rest_name_overhead += rest_entries.size() - 1;
        
        const size_t available_for_descs = remaining_budget - rest_name_overhead;
        const size_t max_desc_len = available_for_descs / rest_entries.size();
        
        if (max_desc_len < MIN_DESC_LENGTH) {
            // Extreme case: show names only for non-bundled
            std::string result;
            for (size_t i = 0; i < skills.size(); ++i) {
                if (i > 0) result += "\n";
                if (is_bundled[i]) {
                    result += full_entries[i];
                } else {
                    result += "- " + skills[i].name;
                }
            }
            return result;
        }
        
        // Truncate descriptions
        std::string result;
        for (size_t i = 0; i < skills.size(); ++i) {
            if (i > 0) result += "\n";
            if (is_bundled[i]) {
                result += full_entries[i];
            } else {
                const auto& skill = skills[i];
                const auto desc = skill.when_to_use 
                    ? skill.description + " - " + *skill.when_to_use
                    : skill.description;
                
                const auto truncated = desc.length() > max_desc_len
                    ? desc.substr(0, max_desc_len)
                    : desc;
                
                result += "- " + skill.name + ": " + truncated;
            }
        }
        return result;
    }
    
    [[nodiscard]] auto execute(SkillInvocation invocation) -> ToolResult {
        if (invocation.skill_name.empty())
            return {.success = false, .error = "未指定技能名称"};
        

        auto skill_path = find_skill(invocation.skill_name);
        if (!skill_path)
            return {.success = false, .error = "技能未找到: " + invocation.skill_name};

        std::ifstream file(*skill_path);
        if (!file) return {.success = false, .error = "无法读取技能定义: " + *skill_path};
        std::stringstream buffer;
        buffer << file.rdbuf();
        auto definition = buffer.str();
        if (definition.empty()) return {.success = false, .error = "技能定义为空: " + invocation.skill_name};
        const auto max_chars = get_char_budget(std::nullopt);
        if (definition.size() > max_chars) definition.resize(max_chars);
        return {.success = true,
                .output = definition,
                .metadata_json = R"({"skill": ")" + detail::json_escape(invocation.skill_name) + R"(", "path": ")" + detail::json_escape(*skill_path) + R"("})"};
    }
    
    [[nodiscard]] auto list_available() const -> std::vector<std::string> {
        std::vector<std::string> skills;
        for (const auto& dir : skill_roots()) {
            if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) continue;
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_directory() || entry.path().extension() == ".md") {
                    skills.push_back(entry.path().stem().string());
                }
            }
        }
        return skills;
    }

private:
    [[nodiscard]] auto find_skill(std::string_view name) const -> std::optional<std::string> {
        for (const auto& root : skill_roots()) {
            const auto direct = root / std::string(name);
            if (std::filesystem::exists(direct)) return direct.string();
            const auto md = root / (std::string(name) + ".md");
            if (std::filesystem::exists(md)) return md.string();
        }
        return std::nullopt;
    }

    [[nodiscard]] static auto skill_roots() -> std::vector<std::filesystem::path> {
        std::vector<std::filesystem::path> roots;
        if (const char* home = std::getenv("HOME")) roots.emplace_back(std::filesystem::path(home) / ".cc-repl" / "skills");
        roots.emplace_back(std::filesystem::current_path() / "skills");
        return roots;
    }
};



struct SyntheticConfig {
    std::string content;
    std::chrono::milliseconds delay_per_chunk{50};
    size_t chunk_size{20};
    bool simulate_thinking{false};
};

class SyntheticOutputTool {
public:
    static constexpr auto name() -> std::string_view { return "synthetic_output"; }
    static constexpr auto description() -> std::string_view {
        return "生成合成输出（用于测试和流式输出模拟）";
    }
    
    [[nodiscard]] auto execute(SyntheticConfig config) -> ToolResult {

        std::string output;
        if (config.simulate_thinking) {
            output = "[thinking]\n" + config.content + "\n[/thinking]\n";
        } else {
            output = config.content;
        }
        return {.success = true, .output = output, 
                .metadata_json = R"({"synthetic": true, "chunks": )" + 
                    std::to_string((config.content.size() + config.chunk_size - 1) / config.chunk_size) + "}"};
    }
    

    void execute_streaming(SyntheticConfig config, std::function<void(std::string_view)> on_chunk) {
        for (size_t i = 0; i < config.content.size(); i += config.chunk_size) {
            auto chunk = std::string_view(config.content).substr(i, config.chunk_size);
            on_chunk(chunk);
            if (config.delay_per_chunk.count() > 0) {
                std::this_thread::sleep_for(config.delay_per_chunk);
            }
        }
    }
};

} // namespace cc::tools
