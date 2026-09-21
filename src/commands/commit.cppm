/// @file commit.cppm
/// @brief CommitCommand implementing the /commit slash command.
/// Runs git status/diff/log, generates conventional commit messages via LLM,
/// validates format, and performs the commit with --amend/--all support.
module;

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <regex>
#include <array>
#include <span>

export module cc.commands.commit;

import cc.types.types;
import cc.commands.command;
import cc.utils.bash_execution;

export namespace cc::commands {

using namespace cc::core;

/// Conventional Commit type categories
enum class CommitType : std::uint8_t {
    Feat, Fix, Docs, Style, Refactor, Perf, Test, Build, Ci, Chore,
};

/// Map CommitType to its string prefix
[[nodiscard]] constexpr std::string_view commit_type_str(CommitType type) noexcept {
    switch (type) {
        case CommitType::Feat:     return "feat";
        case CommitType::Fix:      return "fix";
        case CommitType::Docs:     return "docs";
        case CommitType::Style:    return "style";
        case CommitType::Refactor: return "refactor";
        case CommitType::Perf:     return "perf";
        case CommitType::Test:     return "test";
        case CommitType::Build:    return "build";
        case CommitType::Ci:       return "ci";
        case CommitType::Chore:    return "chore";
    }
    return "chore";
}

/// Options parsed from command arguments
struct CommitOptions {
    bool amend = false;           // --amend: amend the last commit
    bool all = false;             // --all/-a: stage all tracked changes
    std::optional<std::string> message;  // -m: user-supplied message (skip LLM)
};

/// Git state collected before generating commit message
struct GitState {
    std::string status;           // Output of `git status --short`
    std::string diff;             // Output of `git diff --cached` (staged changes)
    std::string recent_log;       // Output of `git log --oneline -10`
};

/// CommitCommand implements the /commit slash command.
/// Workflow: collect git state -> generate commit message -> validate -> commit.
class CommitCommand {
public:
    /// Static command definition metadata
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "commit",
            .description = "Generate a conventional commit message and commit staged changes",
            .args = {
                CommandArg{.name = "--amend", .description = "Amend the previous commit",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--all", .description = "Stage all tracked file changes before committing",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "-m", .description = "Provide commit message directly (skip LLM generation)",
                           .type = ArgType::Text, .required = false},
            },
            .category = "git",
            .aliases = {"ci"},
            .hidden = false,
        };
    }

    /// Validate arguments before execution
    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        // If -m is used, ensure a message follows
        if (opts.message && opts.message->empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "The -m flag requires a non-empty commit message"
            ));
        }
        return {};
    }

    /// Execute the commit command
    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);

        // Step 1: Collect git state
        auto git_state = collect_git_state(opts);
        if (!git_state) return std::unexpected(git_state.error());

        // Step 2: Check if there are changes to commit
        if (git_state->status.empty() && !opts.amend) {
            return CommandResult::fail("No changes to commit. Stage changes with `git add` first.");
        }

        // Step 3: Generate or use provided commit message
        std::string commit_msg;
        if (opts.message) {
            commit_msg = *opts.message;
        } else {
            // Delegate to LLM for message generation (inject as user message)
            auto prompt = build_llm_prompt(*git_state);
            return CommandResult::inject(std::move(prompt));
        }

        // Step 4: Validate conventional commit format
        if (auto result = validate_commit_message(commit_msg); !result) {
            return std::unexpected(result.error());
        }

        // Step 5: Execute git commit
        auto commit_result = execute_commit(commit_msg, opts);
        if (!commit_result) return std::unexpected(commit_result.error());

        return CommandResult::success(
            std::format("Committed: {}", commit_msg.substr(0, 72))
        );
    }

    /// Provide completions for partial argument input
    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array flags = {"--amend", "--all", "-a", "-m"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }

private:
    /// Parse command-line options from argument span
    [[nodiscard]] static CommitOptions parse_options(std::span<const std::string> args) {
        CommitOptions opts;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--amend") {
                opts.amend = true;
            } else if (args[i] == "--all" || args[i] == "-a") {
                opts.all = true;
            } else if (args[i] == "-m" && i + 1 < args.size()) {
                opts.message = args[++i];
            }
        }
        return opts;
    }

    /// Collect current git state via shell commands
    [[nodiscard]] static Result<GitState> collect_git_state(const CommitOptions& opts) {
        GitState state;

        // Run git status
        state.status = run_git_command("status --short");
        if (state.status.starts_with("fatal:")) {
            return std::unexpected(Error::make(
                ErrorCode::ToolExecutionFailed, "Not a git repository"
            ));
        }

        // Stage all tracked changes if --all flag
        if (opts.all) {
            const auto add_output = run_git_command("add -u");
            if (add_output.starts_with("fatal:") || add_output.starts_with("error:")) {
                return std::unexpected(Error::make(
                    ErrorCode::ToolExecutionFailed,
                    std::format("Git add failed: {}", add_output)
                ));
            }
        }

        // Get staged diff (what will be committed)
        state.diff = run_git_command("diff --cached --stat");
        if (state.diff.empty()) {
            state.diff = run_git_command("diff --stat");
        }

        // Get recent log for style reference
        state.recent_log = run_git_command("log --oneline -5");

        return state;
    }

    /// Build the prompt sent to the LLM for commit message generation
    [[nodiscard]] static std::string build_llm_prompt(const GitState& state) {
        return std::format(
            "Generate a conventional commit message for these changes.\n\n"
            "## Git Status:\n```\n{}\n```\n\n"
            "## Diff Summary:\n```\n{}\n```\n\n"
            "## Recent Commits (for style reference):\n```\n{}\n```\n\n"
            "Requirements:\n"
            "- Use conventional commit format: type(scope): description\n"
            "- Types: feat, fix, docs, style, refactor, perf, test, build, ci, chore\n"
            "- Keep the first line under 72 characters\n"
            "- Add body if the change is complex\n"
            "- Reply ONLY with the commit message, no other text.\n\n"
            "Note: Git commands (git diff, git log, git status, etc.) are allowed via the Bash tool if you need more context.",
            state.status, state.diff, state.recent_log
        );
    }

    /// Validate that a commit message follows conventional commit format
    [[nodiscard]] static VoidResult validate_commit_message(std::string_view msg) {
        if (msg.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "Commit message cannot be empty"
            ));
        }

        // Pattern: type(optional scope): description
        static const std::regex pattern(
            R"(^(feat|fix|docs|style|refactor|perf|test|build|ci|chore)(\(.+\))?!?:\s.+)"
        );

        auto first_line = msg.substr(0, msg.find('\n'));
        if (!std::regex_match(std::string(first_line), pattern)) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidRequest,
                .severity = ErrorSeverity::Warning,
                .message = "Commit message does not match conventional format",
                .detail = std::format("Got: '{}'", first_line),
                .suggestion = "Expected format: type(scope): description",
            });
        }

        // Check first line length
        if (first_line.size() > 72) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidRequest,
                .severity = ErrorSeverity::Warning,
                .message = std::format("First line is {} chars (max 72)", first_line.size()),
            });
        }

        return {};
    }

    /// Execute the actual git commit with the given message and options
    [[nodiscard]] static VoidResult execute_commit(
        const std::string& message, const CommitOptions& opts
    ) {
        std::string cmd = "commit";
        if (opts.amend) cmd += " --amend";
        cmd += std::format(" -m \"{}\"", message);

        auto output = run_git_command(cmd);
        if (output.find("error") != std::string::npos ||
            output.find("fatal") != std::string::npos) {
            return std::unexpected(Error::make(
                ErrorCode::ToolExecutionFailed,
                std::format("Git commit failed: {}", output)
            ));
        }
        return {};
    }

    /// Execute a git command and return stdout via popen.
    [[nodiscard]] static auto run_git_command(const std::string& cmd) -> std::string {
        std::string full_cmd = "git " + cmd + " 2>&1";
        FILE* pipe = cc::utils::bash::popen_spawn(full_cmd.c_str());
        if (!pipe) return {};
        std::string result;
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) {
            result += buf;
        }
        cc::utils::bash::pclose_spawn(pipe);
        // Trim trailing newline
        while (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
        return result;
    }
};

} // namespace cc::commands
