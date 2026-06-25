/// @file review.cppm
/// @brief ReviewCommand implementing the /review slash command.
/// Gets git diffs of staged/unstaged changes or specific branches/files,
/// sends to LLM for code review, and outputs structured feedback with severity levels.
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
#include <span>
#include <array>

export module cc.commands.review;

import cc.types.types;
import cc.commands.command;
import cc.utils.bash_execution;

export namespace cc::commands {

using namespace cc::core;

/// Severity level for review findings
enum class ReviewSeverity : std::uint8_t {
    Critical,   // Must fix: security, data loss, crash
    High,       // Should fix: bugs, logic errors
    Medium,     // Recommended: performance, maintainability
    Low,        // Optional: style, naming, minor improvements
    Info,       // Informational: notes, explanations
};

/// Convert severity to display string with indicator
[[nodiscard]] constexpr std::string_view severity_label(ReviewSeverity sev) noexcept {
    switch (sev) {
        case ReviewSeverity::Critical: return "[CRITICAL]";
        case ReviewSeverity::High:     return "[HIGH]";
        case ReviewSeverity::Medium:   return "[MEDIUM]";
        case ReviewSeverity::Low:      return "[LOW]";
        case ReviewSeverity::Info:     return "[INFO]";
    }
    return "[UNKNOWN]";
}

/// A single review finding/comment
struct ReviewFinding {
    ReviewSeverity severity;
    std::string file;             // File path where issue was found
    std::uint32_t line = 0;       // Line number (0 if not applicable)
    std::string category;         // Category (security, performance, logic, style)
    std::string description;      // What the issue is
    std::string suggestion;       // How to fix it
};

/// Options controlling what to review
struct ReviewOptions {
    std::optional<std::string> branch;       // Compare against specific branch
    std::vector<std::string> files;          // Review only specific files
    bool staged_only = false;                // Only staged changes
    bool include_context = true;             // Include surrounding code context
};

/// ReviewCommand implements the /review slash command.
/// Gathers code diffs and delegates to LLM for structured code review.
class ReviewCommand {
public:
    /// Static command definition metadata
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "review",
            .description = "Review code changes with AI-powered analysis",
            .args = {
                CommandArg{.name = "--branch", .description = "Compare against a specific branch",
                           .type = ArgType::Text, .required = false},
                CommandArg{.name = "--staged", .description = "Review only staged changes",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "files", .description = "Specific files to review",
                           .type = ArgType::FilePath, .required = false},
            },
            .category = "git",
            .aliases = {"cr"},
            .hidden = false,
        };
    }

    /// Validate arguments
    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        // Validate branch name if provided
        if (opts.branch && opts.branch->empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "--branch requires a branch name"
            ));
        }
        return {};
    }

    /// Execute the review command
    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);

        // Step 1: Gather the diff to review
        auto diff = gather_diff(opts);
        if (!diff) return std::unexpected(diff.error());

        if (diff->empty()) {
            return CommandResult::fail(
                "No changes to review. Make some changes or specify a branch to compare against."
            );
        }

        // Step 2: Build the review prompt and inject to LLM
        auto prompt = build_review_prompt(*diff, opts);
        return CommandResult::inject(std::move(prompt));
    }

    /// Provide completions for partial input
    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array flags = {"--branch", "--staged", "--no-context"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }

private:
    /// Parse review options from arguments
    [[nodiscard]] static ReviewOptions parse_options(std::span<const std::string> args) {
        ReviewOptions opts;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--branch" && i + 1 < args.size()) {
                opts.branch = args[++i];
            } else if (args[i] == "--staged") {
                opts.staged_only = true;
            } else if (args[i] == "--no-context") {
                opts.include_context = false;
            } else if (!args[i].starts_with("-")) {
                // Treat as file path
                opts.files.emplace_back(args[i]);
            }
        }
        return opts;
    }

    /// Gather the diff content based on review options
    [[nodiscard]] static Result<std::string> gather_diff(const ReviewOptions& opts) {
        std::string cmd;

        if (opts.branch) {
            // Compare current branch against specified branch
            cmd = std::format("diff {}...HEAD", *opts.branch);
        } else if (opts.staged_only) {
            // Only staged changes
            cmd = "diff --cached";
        } else {
            // All uncommitted changes (staged + unstaged)
            cmd = "diff HEAD";
        }

        // Append file filter if specified
        if (!opts.files.empty()) {
            cmd += " --";
            for (const auto& file : opts.files) {
                cmd += std::format(" {}", file);
            }
        }

        auto output = run_git_command(cmd);
        if (output.starts_with("fatal:")) {
            return std::unexpected(Error::make(
                ErrorCode::ToolExecutionFailed,
                std::format("Git error: {}", output)
            ));
        }
        return output;
    }

    /// Build the prompt sent to LLM for code review
    [[nodiscard]] static std::string build_review_prompt(
        const std::string& diff, const ReviewOptions& opts
    ) {
        std::string context_note = opts.include_context
            ? "Include surrounding code context in your analysis."
            : "Focus only on the changed lines.";

        std::string file_note;
        if (!opts.files.empty()) {
            file_note = "Reviewing specific files: ";
            for (const auto& f : opts.files) {
                file_note += f + " ";
            }
            file_note += "\n";
        }

        return std::format(
            "Please review the following code changes and provide structured feedback.\n\n"
            "{}"
            "## Diff:\n```diff\n{}\n```\n\n"
            "## Review Guidelines:\n"
            "- {}\n"
            "- Categorize each finding by severity: CRITICAL, HIGH, MEDIUM, LOW, INFO\n"
            "- Categories: security, performance, logic, concurrency, robustness, style\n"
            "- For each finding provide: file, line, category, description, suggestion\n"
            "- Be specific: reference exact line numbers and variable names\n"
            "- Note positive patterns too (INFO level)\n\n"
            "## Output Format:\n"
            "For each finding:\n"
            "```\n"
            "[SEVERITY] file:line - category\n"
            "  Description: ...\n"
            "  Suggestion: ...\n"
            "```\n"
            "End with a brief summary of overall code quality.",
            file_note, diff, context_note
        );
    }

    /// Format review findings for display (post-processing of LLM output)
    [[nodiscard]] static std::string format_findings(const std::vector<ReviewFinding>& findings) {
        if (findings.empty()) return "No issues found. Code looks good!";

        std::string output;
        // Group by severity
        auto by_severity = [](const ReviewFinding& a, const ReviewFinding& b) {
            return static_cast<int>(a.severity) < static_cast<int>(b.severity);
        };

        auto sorted = findings;
        std::ranges::sort(sorted, by_severity);

        for (const auto& finding : sorted) {
            output += std::format(
                "{} {}:{} - {}\n  {}\n  Fix: {}\n\n",
                severity_label(finding.severity),
                finding.file,
                finding.line,
                finding.category,
                finding.description,
                finding.suggestion
            );
        }

        // Summary counts
        auto count_sev = [&](ReviewSeverity sev) {
            return std::ranges::count_if(findings, [sev](const auto& f) {
                return f.severity == sev;
            });
        };

        output += std::format(
            "---\nSummary: {} critical, {} high, {} medium, {} low, {} info\n",
            count_sev(ReviewSeverity::Critical),
            count_sev(ReviewSeverity::High),
            count_sev(ReviewSeverity::Medium),
            count_sev(ReviewSeverity::Low),
            count_sev(ReviewSeverity::Info)
        );

        return output;
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
