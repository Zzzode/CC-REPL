// Implementation unit for cc.query.query_engine — system prompt assembly:
// SystemPromptBuilder::build/default, QueryEngine::build_and_add_system_prompt
// (project/user/auto memory + session summary injection), and the git
// context popen probe. <cstdio> is a global-module-fragment header because
// FILE / std::fgets / ::pclose are C names that cannot arrive through
// `import std;`.
module;

#include <cstdio>

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.memdir.paths;
// Genuinely used via global-namespace calls (::memdir::build_memory_lines,
// ::memdir::join_lines, ::memdir::truncate_entrypoint_content); the
// dead-import heuristic only attributes deep cc::-namespace paths, so the
// global ::memdir:: qualification is invisible to it.
import cc.memdir.memdir;  // arch-check: keep-import
import cc.constants.paths;
import cc.utils.bash_execution;

namespace cc::core {

[[nodiscard]] std::string SystemPromptBuilder::build(
    std::optional<std::string_view> custom_prompt,
    std::optional<std::string_view> append_prompt,
    const UserContext& user_ctx,
    const SystemContext& system_ctx) {
    std::string prompt;

    // Base prompt from custom or default
    if (custom_prompt) {
        prompt = std::string(*custom_prompt);
    } else {
        prompt = build_default_system_prompt();
    }

    // Append additional instructions
    if (append_prompt) {
        prompt += "\n\n";
        prompt += *append_prompt;
    }

    // Add context information
    prompt += "\n\n<context>";
    prompt += std::format("\n<cwd>{}</cwd>", user_ctx.cwd);
    prompt += std::format("\n<platform>{}</platform>", user_ctx.platform);

    // Add tool descriptions
    if (!system_ctx.tool_descriptions.empty()) {
        prompt += "\n<tools>";
        for (const auto& tool : system_ctx.tool_descriptions) {
            prompt += std::format("\n  - {}", tool);
        }
        prompt += "\n</tools>";
    }

    for (const auto& ctx : user_ctx.additional_contexts) {
        prompt += "\n";
        prompt += ctx;
    }
    prompt += "\n</context>";

    return prompt;
}

[[nodiscard]] std::string SystemPromptBuilder::build_default_system_prompt() {
    return R"(You are Loom, a helpful assistant working with code and files.
You have access to various tools to read, write, edit, and search files, execute commands, and more.
Always use the appropriate tools to accomplish your tasks rather than trying to do everything manually.
When you write or edit files, always make complete, functional changes.)";
}

void QueryEngine::build_and_add_system_prompt() {
    auto cwd = config_.cwd.value_or(std::filesystem::current_path().string());

    UserContext user_ctx{
        cwd,
        "unknown",
        "user",
        {}
    };

    // P1-12: Inject git context (branch + last commit)
    populate_git_context(user_ctx, cwd);

    // P1-13: Load the nearest project memory file (LOOM.md / AGENTS.md /
    // CLAUDE.md, whichever is nearest). The tag below names the file that
    // was actually read rather than a fixed "LOOM.md", so a legacy
    // CLAUDE.md is not mislabelled to the model as something it is not.
    if (auto memory_file =
            cc::constants::paths::find_memory_file(std::filesystem::path(cwd))) {
        std::ifstream mem_ifs(*memory_file);
        if (mem_ifs) {
            std::string loom_md((std::istreambuf_iterator<char>(mem_ifs)), {});
            if (!loom_md.empty()) {
                loaded_nested_memory_paths_.insert(memory_file->string());
                user_ctx.additional_contexts.push_back(std::format(
                    "<context name=\"{}\">\n{}\n</context>",
                    memory_file->filename().string(), loom_md));
            }
        }
    }

    // P1-13b: Load user-level memory via the memdir module so global user
    // preferences are injected alongside project memory. Tree/ancestor
    // memory is already covered by the walk above.
    auto user_mem = cc::memdir::get_user_memory_path();
    if (std::filesystem::exists(user_mem)) {
        std::ifstream um_ifs(user_mem);
        if (um_ifs) {
            std::string um_content((std::istreambuf_iterator<char>(um_ifs)), {});
            if (!um_content.empty()) {
                loaded_nested_memory_paths_.insert(user_mem.string());
                user_ctx.additional_contexts.push_back(
                    std::format("<context name=\"UserMemory\">\n{}\n</context>", um_content));
            }
        }
    }

    // Auto-memory: inject the file-based memory guidance (teaches the
    // model it can persist memories under the per-project memory dir) and
    // guarantee the directory exists so the model can write without setup.
    // TS REF: src/memdir/memdir.ts loadMemoryPrompt() (buildMemoryLines)
    // — it returns guidance only; MEMORY.md content is appended below.
    if (auto auto_mem_dir =
            cc::memdir::get_auto_mem_path(std::filesystem::path(cwd))) {
        std::error_code mkdir_ec;
        std::filesystem::create_directories(*auto_mem_dir, mkdir_ec);

        auto guidance_lines = ::memdir::build_memory_lines(
            "auto memory", auto_mem_dir->string());
        if (!guidance_lines.empty()) {
            user_ctx.additional_contexts.push_back(std::format(
                "<context name=\"memory\">\n{}\n</context>",
                ::memdir::join_lines(guidance_lines)));
        }

        // Append the MEMORY.md index contents (loommd.ts injects the
        // AutoMem entrypoint separately from the guidance prompt).
        std::filesystem::path mem_dir_path = *auto_mem_dir;
        std::filesystem::path mem_index_file{mem_dir_path / "MEMORY.md"};
        if (std::filesystem::exists(mem_index_file)) {
            std::ifstream mm_ifs{mem_index_file};
            if (mm_ifs) {
                std::string raw((std::istreambuf_iterator<char>(mm_ifs)), {});
                auto trunc = ::memdir::truncate_entrypoint_content(raw);
                if (!trunc.content.empty()) {
                    user_ctx.additional_contexts.push_back(std::format(
                        "<context name=\"MEMORY.md\">\n{}\n</context>",
                        trunc.content));
                }
            }
        }
    }

    // Inject this session's accumulated compaction summary, if a prior
    // run (or an earlier compaction in this run) wrote one. Prevents a
    // resumed/continuation session from starting blind after history was
    // dropped. TS REF: SessionMemory getSessionMemoryContent used by
    // sessionMemoryCompact.ts as the compact summary source.
    if (auto summary = read_session_summary(cwd); !summary.empty()) {
        constexpr std::size_t kMaxSummaryChars = 12000;
        if (summary.size() > kMaxSummaryChars) {
            summary.resize(kMaxSummaryChars);
            summary += "\n... [older session summary truncated]";
        }
        user_ctx.additional_contexts.push_back(std::format(
            "<context name=\"session-memory\">\n{}\n</context>",
            summary));
    }

    // P1-1: Add tool descriptions to system context
    SystemContext system_ctx{
        {},
        config_.model_params.model,
        "default"
    };
    for (const auto& tool : config_.tools) {
        system_ctx.tool_descriptions.push_back(tool.name);
    }

    auto prompt = SystemPromptBuilder::build(
        config_.custom_system_prompt,
        config_.append_system_prompt,
        user_ctx,
        system_ctx);

    SystemMessage sys_msg{};
    sys_msg.id.value = generate_id();
    sys_msg.timestamp = std::chrono::system_clock::now();
    sys_msg.content.push_back(TextBlock{std::move(prompt)});
    conversation_.push_back(Message{std::move(sys_msg)});
}

void QueryEngine::populate_git_context(UserContext& ctx, const std::string& cwd) {
    auto run_git_cmd = [&](const char* cmd) -> std::string {
        auto full_cmd = std::format("cd \"{}\" && ( {} ) 2>/dev/null", cwd, cmd);
        std::unique_ptr<FILE, decltype(&pclose)> pipe(cc::utils::bash::popen_spawn(full_cmd.c_str()), pclose);
        if (!pipe) return {};
        std::string result;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe.get())) {
            result += buf;
        }
        // Trim trailing newline
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        return result;
    };

    auto branch = run_git_cmd("git rev-parse --abbrev-ref HEAD");
    auto commit = run_git_cmd("git log --oneline -1");
    auto status_short = run_git_cmd("git status --short | head -20");

    if (!branch.empty()) {
        std::string git_ctx = std::format(
            "<context name=\"git\">\nBranch: {}\nLast commit: {}", branch, commit);
        if (!status_short.empty()) {
            git_ctx += std::format("\nStatus:\n{}", status_short);
        }
        git_ctx += "\n</context>";
        ctx.additional_contexts.push_back(std::move(git_ctx));
    }
}

} // namespace cc::core
