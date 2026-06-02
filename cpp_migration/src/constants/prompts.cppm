// C++23 module: System prompt constants and key string templates.
// Contains the core prompt fragments used to construct the system prompt.
// Note: The full dynamic prompt assembly logic (getSystemPrompt, computeEnvInfo, etc.)
// is complex and runtime-dependent; only the static constants are migrated here.
module;
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <vector>

export module cc.constants.prompts;


export namespace cc::constants::prompts {

inline constexpr std::string_view claude_code_docs_map_url =
    "https://code.claude.com/docs/en/claude_code_docs_map.md";

// Boundary marker separating static (cacheable) content from dynamic content.
// Everything BEFORE this in the system prompt can use scope: 'global'.
// Everything AFTER contains user/session-specific content.
inline constexpr std::string_view system_prompt_dynamic_boundary =
    "__SYSTEM_PROMPT_DYNAMIC_BOUNDARY__";

// Latest frontier model name for prompt references
inline constexpr std::string_view frontier_model_name = "Claude Opus 4.6";

// Model family IDs for the latest in each tier
struct ModelFamilyIds {
    std::string_view opus;
    std::string_view sonnet;
    std::string_view haiku;
};

inline constexpr ModelFamilyIds claude_4_5_or_4_6_model_ids = {
    .opus = "claude-opus-4-6",
    .sonnet = "claude-sonnet-4-6",
    .haiku = "claude-haiku-4-5-20251001",
};

// Default system prompt for sub-agents
inline constexpr std::string_view default_agent_prompt =
    "You are an agent for Claude Code, Anthropic's official CLI for Claude. "
    "Given the user's message, you should use the tools available to complete the task. "
    "Complete the task fully\u2014don't gold-plate, but don't leave it half-done. "
    "When you complete the task, respond with a concise report covering what was done "
    "and any key findings \u2014 the caller will relay this to the user, so it only needs the essentials.";

// Cyber risk instruction (imported from cyber_risk module, duplicated here for convenience)
inline constexpr std::string_view cyber_risk_instruction =
    "IMPORTANT: Assist with authorized security testing, defensive security, CTF challenges, "
    "and educational contexts. Refuse requests for destructive techniques, DoS attacks, "
    "mass targeting, supply chain compromise, or detection evasion for malicious purposes. "
    "Dual-use security tools (C2 frameworks, credential testing, exploit development) require "
    "clear authorization context: pentesting engagements, CTF competitions, security research, "
    "or defensive use cases.";

// Summarize tool results guidance
inline constexpr std::string_view summarize_tool_results_section =
    "When working with tool results, write down any important information you might need "
    "later in your response, as the original tool result may be cleared later.";

// Knowledge cutoff dates per model family
enum class ModelFamily {
    claude_sonnet_4_6,
    claude_opus_4_6,
    claude_opus_4_5,
    claude_haiku_4,
    claude_opus_4,
    claude_sonnet_4,
    unknown,
};

inline constexpr std::string_view get_knowledge_cutoff(ModelFamily family) {
    switch (family) {
        case ModelFamily::claude_sonnet_4_6: return "August 2025";
        case ModelFamily::claude_opus_4_6:   return "May 2025";
        case ModelFamily::claude_opus_4_5:   return "May 2025";
        case ModelFamily::claude_haiku_4:    return "February 2025";
        case ModelFamily::claude_opus_4:     return "January 2025";
        case ModelFamily::claude_sonnet_4:   return "January 2025";
        default:                             return "";
    }
}

struct SystemPromptOptions {
    std::string model;
    std::vector<std::string> enabled_tools;
    std::vector<std::string> additional_working_directories;
    bool simple = false;
    bool use_global_cache_boundary = true;
};

[[nodiscard]] inline ModelFamily model_family_from_id(std::string_view model_id) {
    if (model_id.find("claude-sonnet-4-6") != std::string_view::npos) {
        return ModelFamily::claude_sonnet_4_6;
    }
    if (model_id.find("claude-opus-4-6") != std::string_view::npos) {
        return ModelFamily::claude_opus_4_6;
    }
    if (model_id.find("claude-opus-4-5") != std::string_view::npos) {
        return ModelFamily::claude_opus_4_5;
    }
    if (model_id.find("claude-haiku-4") != std::string_view::npos) {
        return ModelFamily::claude_haiku_4;
    }
    if (model_id.find("claude-opus-4") != std::string_view::npos) {
        return ModelFamily::claude_opus_4;
    }
    if (model_id.find("claude-sonnet-4") != std::string_view::npos) {
        return ModelFamily::claude_sonnet_4;
    }
    return ModelFamily::unknown;
}

[[nodiscard]] inline std::string get_knowledge_cutoff(std::string_view model_id) {
    return std::string(get_knowledge_cutoff(model_family_from_id(model_id)));
}

[[nodiscard]] inline std::string get_cwd() {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string("unknown") : cwd.string();
}

[[nodiscard]] inline bool is_git_repository(std::filesystem::path start = std::filesystem::current_path()) {
    std::error_code ec;
    while (!start.empty()) {
        if (std::filesystem::exists(start / ".git", ec)) {
            return true;
        }
        auto parent = start.parent_path();
        if (parent == start) break;
        start = parent;
    }
    return false;
}

[[nodiscard]] inline std::string get_platform_name() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

[[nodiscard]] inline std::string get_uname_sr() {
    struct utsname info {};
    if (uname(&info) == 0) {
        return std::string(info.sysname) + " " + info.release;
    }
    return get_platform_name();
}

[[nodiscard]] inline std::string get_shell_info_line() {
    const char* shell_env = std::getenv("SHELL");
    std::string shell = shell_env && shell_env[0] != '\0' ? shell_env : "unknown";
    std::string shell_name = shell;
    if (shell.find("zsh") != std::string::npos) {
        shell_name = "zsh";
    } else if (shell.find("bash") != std::string::npos) {
        shell_name = "bash";
    }
    if (get_platform_name() == "win32") {
        return "Shell: " + shell_name +
            " (use Unix shell syntax, not Windows — e.g., /dev/null not NUL, forward slashes in paths)";
    }
    return "Shell: " + shell_name;
}

[[nodiscard]] inline std::string model_description(std::string_view model_id) {
    auto family = model_family_from_id(model_id);
    if (family == ModelFamily::unknown) {
        return "You are powered by the model " + std::string(model_id) + ".";
    }
    return "You are powered by the model named " + std::string(frontier_model_name) +
        ". The exact model ID is " + std::string(model_id) + ".";
}

[[nodiscard]] inline std::vector<std::string> prepend_bullets(const std::vector<std::string>& items) {
    std::vector<std::string> result;
    result.reserve(items.size());
    for (const auto& item : items) {
        if (!item.empty()) result.push_back("- " + item);
    }
    return result;
}

[[nodiscard]] inline std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out << '\n';
        out << lines[i];
    }
    return out.str();
}

[[nodiscard]] inline std::string compute_simple_env_info(
    std::string_view model_id,
    const std::vector<std::string>& additional_working_directories = {}) {
    std::vector<std::string> items;
    items.push_back("Primary working directory: " + get_cwd());
    items.push_back(std::string("Is a git repository: ") + (is_git_repository() ? "true" : "false"));
    if (!additional_working_directories.empty()) {
        items.push_back("Additional working directories:");
        for (const auto& dir : additional_working_directories) {
            items.push_back(dir);
        }
    }
    items.push_back("Platform: " + get_platform_name());
    items.push_back(get_shell_info_line());
    items.push_back("OS Version: " + get_uname_sr());
    items.push_back(model_description(model_id));

    auto cutoff = get_knowledge_cutoff(model_id);
    if (!cutoff.empty()) {
        items.push_back("Assistant knowledge cutoff is " + cutoff + ".");
    }
    items.push_back("The most recent Claude model family is Claude 4.5/4.6. Model IDs — Opus 4.6: '" +
        std::string(claude_4_5_or_4_6_model_ids.opus) + "', Sonnet 4.6: '" +
        std::string(claude_4_5_or_4_6_model_ids.sonnet) + "', Haiku 4.5: '" +
        std::string(claude_4_5_or_4_6_model_ids.haiku) +
        "'. When building AI applications, default to the latest and most capable Claude models.");
    items.push_back("Claude Code is available as a CLI in the terminal, desktop app (Mac/Windows), web app (claude.ai/code), and IDE extensions (VS Code, JetBrains).");
    items.push_back("Fast mode for Claude Code uses the same " + std::string(frontier_model_name) +
        " model with faster output. It does NOT switch to a different model. It can be toggled with /fast.");

    std::vector<std::string> lines{
        "# Environment",
        "You have been invoked in the following environment: "
    };
    auto bullets = prepend_bullets(items);
    lines.insert(lines.end(), bullets.begin(), bullets.end());
    return join_lines(lines);
}

[[nodiscard]] inline std::string get_simple_intro_section() {
    return "You are Claude Code, Anthropic's official CLI for Claude.";
}

[[nodiscard]] inline std::string get_simple_system_section() {
    return "# System\nYou are an interactive CLI tool that helps users with software engineering tasks. Use the instructions and tools available in this session to complete the user's request.";
}

[[nodiscard]] inline std::string get_using_your_tools_section(const std::vector<std::string>& enabled_tools) {
    if (enabled_tools.empty()) {
        return "# Using your tools\nUse the available tools when they help complete the task.";
    }
    std::vector<std::string> lines{
        "# Using your tools",
        "You have access to these tools:"
    };
    auto bullets = prepend_bullets(enabled_tools);
    lines.insert(lines.end(), bullets.begin(), bullets.end());
    return join_lines(lines);
}

[[nodiscard]] inline std::string get_simple_tone_and_style_section() {
    return join_lines({
        "# Tone and style",
        "- Only use emojis if the user explicitly requests it. Avoid using emojis in all communication unless asked.",
        "- Your responses should be short and concise.",
        "- When referencing specific functions or pieces of code include the pattern file_path:line_number to allow the user to easily navigate to the source code location.",
        "- Do not use a colon before tool calls. Text like \"Let me read the file:\" followed by a read tool call should just be \"Let me read the file.\" with a period."
    });
}

[[nodiscard]] inline std::string get_output_efficiency_section() {
    return "# Output efficiency\nIMPORTANT: Go straight to the point. Try the simplest approach first without going in circles. Keep your text output brief and direct.";
}

[[nodiscard]] inline std::vector<std::string> get_system_prompt(const SystemPromptOptions& options) {
    if (options.simple) {
        return {"You are Claude Code, Anthropic's official CLI for Claude.\n\nCWD: " + get_cwd()};
    }

    std::vector<std::string> sections{
        get_simple_intro_section(),
        get_simple_system_section(),
        "# Doing tasks\nComplete requested software engineering tasks carefully and verify the result when possible.",
        "# Actions\nBe proactive when the next step is clear, but ask for clarification when requirements are ambiguous.",
        get_using_your_tools_section(options.enabled_tools),
        get_simple_tone_and_style_section(),
        get_output_efficiency_section(),
    };

    if (options.use_global_cache_boundary) {
        sections.emplace_back(system_prompt_dynamic_boundary);
    }

    sections.push_back("# Session guidance\nRespect the user's current working directory and repository state.");
    sections.push_back(compute_simple_env_info(options.model, options.additional_working_directories));
    sections.push_back(std::string(summarize_tool_results_section));
    return sections;
}

} // namespace cc::constants::prompts
