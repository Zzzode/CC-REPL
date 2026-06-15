module;
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <array>
#include <cstdio>
#include <filesystem>
#include <chrono>
#include <algorithm>

export module cc.skills.bundled.verify;

import cc.skills.load_skills_dir;
import cc.utils.bash_execution;

export namespace cc::skills::bundled {

// Configuration for verification checks
struct VerifyConfig {
    std::string target;
    std::vector<std::string> checks;
    bool auto_fix;
    std::string working_directory;  // cwd to run commands from
};

// Severity level for an issue
enum class IssueSeverity {
    Error,
    Warning,
    Info
};

// A single verification issue with details
struct VerifyIssue {
    std::string check_type;
    std::string message;
    std::string file;
    int line{0};
    IssueSeverity severity{IssueSeverity::Error};
};

// Result of a verification run
struct VerifyResult {
    bool passed;
    std::vector<VerifyIssue> issues;
    std::vector<std::string> checks_run;
    std::vector<std::string> fixes_applied;
    double duration_ms{0.0};
};

namespace detail {

// Execute a shell command and capture output
inline std::pair<int, std::string> exec_command(const std::string& cmd) {
    std::string output;
    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) {
        return {-1, "Failed to execute command: " + cmd};
    }
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int status = cc::utils::bash::pclose_spawn(pipe);
    #ifndef _WIN32
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    #else
    int exit_code = status;
    #endif
    return {exit_code, output};
}

// Parse compiler/linter output lines into issues
inline std::vector<VerifyIssue> parse_output_lines(
    const std::string& output, const std::string& check_type) {
    std::vector<VerifyIssue> issues;
    std::string_view sv(output);
    size_t pos = 0;
    while (pos < sv.size()) {
        auto nl = sv.find('\n', pos);
        if (nl == std::string_view::npos) nl = sv.size();
        auto line = sv.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;

        VerifyIssue issue;
        issue.check_type = check_type;
        issue.message = std::string(line);
        issue.severity = IssueSeverity::Error;

        // Try to parse "file:line: message" format
        auto first_colon = line.find(':');
        if (first_colon != std::string_view::npos && first_colon > 0) {
            auto second_colon = line.find(':', first_colon + 1);
            if (second_colon != std::string_view::npos) {
                auto line_num_str = line.substr(first_colon + 1, second_colon - first_colon - 1);
                bool all_digits = !line_num_str.empty();
                for (char c : line_num_str) {
                    if (c < '0' || c > '9') { all_digits = false; break; }
                }
                if (all_digits) {
                    issue.file = std::string(line.substr(0, first_colon));
                    issue.line = 0;
                    for (char c : line_num_str) {
                        issue.line = issue.line * 10 + (c - '0');
                    }
                    auto msg_start = second_colon + 1;
                    if (msg_start < line.size() && line[msg_start] == ' ') ++msg_start;
                    issue.message = std::string(line.substr(msg_start));
                }
            }
        }

        // Detect severity from message keywords
        if (issue.message.find("warning") != std::string::npos ||
            issue.message.find("Warning") != std::string::npos) {
            issue.severity = IssueSeverity::Warning;
        } else if (issue.message.find("note") != std::string::npos ||
                   issue.message.find("info") != std::string::npos) {
            issue.severity = IssueSeverity::Info;
        }

        issues.push_back(std::move(issue));
    }
    return issues;
}

// Detect project language for determining which tools to use
inline std::string detect_language(const std::string& target, const std::string& cwd) {
    namespace fs = std::filesystem;
    fs::path path(target.empty() ? cwd : target);

    if (target.ends_with(".ts") || target.ends_with(".tsx") || target.ends_with(".js")) {
        return "typescript";
    }
    if (target.ends_with(".py")) return "python";
    if (target.ends_with(".rs")) return "rust";
    if (target.ends_with(".go")) return "go";
    if (target.ends_with(".cpp") || target.ends_with(".cppm") ||
        target.ends_with(".cc") || target.ends_with(".h")) {
        return "cpp";
    }

    // Detect from project files
    if (fs::exists(fs::path(cwd) / "package.json")) return "typescript";
    if (fs::exists(fs::path(cwd) / "Cargo.toml")) return "rust";
    if (fs::exists(fs::path(cwd) / "go.mod")) return "go";
    if (fs::exists(fs::path(cwd) / "CMakeLists.txt")) return "cpp";
    if (fs::exists(fs::path(cwd) / "setup.py") || fs::exists(fs::path(cwd) / "pyproject.toml")) {
        return "python";
    }
    return "unknown";
}

// Get the appropriate command for each check type and language
inline std::string get_check_command(const std::string& check, const std::string& lang,
                                     const std::string& target) {
    if (check == "syntax") {
        if (lang == "typescript") return "npx tsc --noEmit " + target;
        if (lang == "python") return "python3 -m py_compile " + target;
        if (lang == "rust") return "cargo check 2>&1";
        if (lang == "go") return "go vet " + target + " 2>&1";
        if (lang == "cpp") return "g++ -std=c++23 -fsyntax-only " + target + " 2>&1";
        return "";
    }
    if (check == "types") {
        if (lang == "typescript") return "npx tsc --noEmit --strict " + target;
        if (lang == "python") return "mypy " + target + " 2>&1";
        if (lang == "rust") return "cargo check 2>&1";
        return "";
    }
    if (check == "tests") {
        if (lang == "typescript") return "npx jest --passWithNoTests 2>&1";
        if (lang == "python") return "python3 -m pytest " + target + " 2>&1";
        if (lang == "rust") return "cargo test 2>&1";
        if (lang == "go") return "go test ./... 2>&1";
        if (lang == "cpp") return "ctest --test-dir build 2>&1";
        return "";
    }
    if (check == "lint") {
        if (lang == "typescript") return "npx eslint " + target + " 2>&1";
        if (lang == "python") return "ruff check " + target + " 2>&1";
        if (lang == "rust") return "cargo clippy 2>&1";
        if (lang == "go") return "golangci-lint run " + target + " 2>&1";
        if (lang == "cpp") return "clang-tidy " + target + " 2>&1";
        return "";
    }
    if (check == "format") {
        if (lang == "typescript") return "npx prettier --check " + target + " 2>&1";
        if (lang == "python") return "ruff format --check " + target + " 2>&1";
        if (lang == "rust") return "cargo fmt --check 2>&1";
        if (lang == "go") return "gofmt -l " + target + " 2>&1";
        if (lang == "cpp") return "clang-format --dry-run --Werror " + target + " 2>&1";
        return "";
    }
    if (check == "security") {
        if (lang == "typescript") return "npx audit-ci --critical 2>&1";
        if (lang == "python") return "bandit -r " + target + " 2>&1";
        if (lang == "rust") return "cargo audit 2>&1";
        if (lang == "go") return "gosec ./... 2>&1";
        return "";
    }
    if (check == "build") {
        if (lang == "typescript") return "npx tsc --build 2>&1";
        if (lang == "rust") return "cargo build 2>&1";
        if (lang == "go") return "go build ./... 2>&1";
        if (lang == "cpp") return "cmake --build build 2>&1";
        return "";
    }
    return "";
}

// Get auto-fix command for a check type
inline std::string get_fix_command(const std::string& check, const std::string& lang,
                                   const std::string& target) {
    if (check == "lint") {
        if (lang == "typescript") return "npx eslint --fix " + target + " 2>&1";
        if (lang == "python") return "ruff check --fix " + target + " 2>&1";
        if (lang == "rust") return "cargo clippy --fix --allow-dirty 2>&1";
        if (lang == "go") return "golangci-lint run --fix " + target + " 2>&1";
        if (lang == "cpp") return "clang-tidy --fix " + target + " 2>&1";
    }
    if (check == "format") {
        if (lang == "typescript") return "npx prettier --write " + target + " 2>&1";
        if (lang == "python") return "ruff format " + target + " 2>&1";
        if (lang == "rust") return "cargo fmt 2>&1";
        if (lang == "go") return "gofmt -w " + target + " 2>&1";
        if (lang == "cpp") return "clang-format -i " + target + " 2>&1";
    }
    return "";
}

} // namespace detail

// Run verification checks on a target
std::expected<VerifyResult, std::string> run_verification(VerifyConfig config) {
    if (config.target.empty()) {
        return std::unexpected("Verification target cannot be empty");
    }
    if (config.checks.empty()) {
        return std::unexpected("No checks specified for verification");
    }

    auto start = std::chrono::steady_clock::now();

    VerifyResult result;
    result.passed = true;

    std::string cwd = config.working_directory.empty()
        ? std::filesystem::current_path().string()
        : config.working_directory;

    std::string lang = detail::detect_language(config.target, cwd);

    for (const auto& check : config.checks) {
        // Validate check type
        static const std::vector<std::string> valid_checks = {
            "syntax", "types", "tests", "lint", "format", "security", "build"
        };
        if (std::find(valid_checks.begin(), valid_checks.end(), check) == valid_checks.end()) {
            result.issues.push_back(VerifyIssue{
                .check_type = check,
                .message = "Unknown check type: " + check,
                .file = {},
                .line = 0,
                .severity = IssueSeverity::Error
            });
            result.passed = false;
            continue;
        }

        // Get and execute check command
        std::string cmd = detail::get_check_command(check, lang, config.target);
        if (cmd.empty()) {
            // No tool available for this language/check combo — skip silently
            result.checks_run.push_back(check + " (skipped: no tool for " + lang + ")");
            continue;
        }

        // Execute in the working directory
        std::string full_cmd = "cd " + cwd + " && " + cmd;
        auto [exit_code, output] = detail::exec_command(full_cmd);

        result.checks_run.push_back(check);

        if (exit_code != 0) {
            result.passed = false;
            auto issues = detail::parse_output_lines(output, check);
            if (issues.empty()) {
                // Couldn't parse individual issues, create a general one
                result.issues.push_back(VerifyIssue{
                    .check_type = check,
                    .message = output.empty() ? "Check failed with exit code " + std::to_string(exit_code) : output,
                    .file = config.target,
                    .line = 0,
                    .severity = IssueSeverity::Error
                });
            } else {
                for (auto& issue : issues) {
                    result.issues.push_back(std::move(issue));
                }
            }

            // Attempt auto-fix if enabled
            if (config.auto_fix) {
                std::string fix_cmd = detail::get_fix_command(check, lang, config.target);
                if (!fix_cmd.empty()) {
                    std::string full_fix_cmd = "cd " + cwd + " && " + fix_cmd;
                    auto [fix_exit, fix_output] = detail::exec_command(full_fix_cmd);
                    if (fix_exit == 0) {
                        result.fixes_applied.push_back(check + ": auto-fix applied");
                        // Re-run the check to see if it passes now
                        auto [recheck_exit, recheck_output] = detail::exec_command(full_cmd);
                        if (recheck_exit == 0) {
                            // Fix was successful — remove issues from this check
                            std::erase_if(result.issues, [&](const VerifyIssue& i) {
                                return i.check_type == check;
                            });
                        }
                    }
                }
            }
        }
    }

    // Re-evaluate pass status after auto-fixes
    result.passed = std::none_of(result.issues.begin(), result.issues.end(),
        [](const VerifyIssue& i) { return i.severity == IssueSeverity::Error; });

    auto end = std::chrono::steady_clock::now();
    result.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return result;
}

// Get the skill manifest for the verify skill
cc::skills::SkillManifest get_verify_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "verify",
        .description = "Run verification checks (syntax, types, tests, lint, format, security, build) on code",
        .version = "1.0.0",
        .triggers = {"verify", "check", "validate", "run checks", "ensure correct"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
