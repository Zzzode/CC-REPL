/// @file loop.cppm
/// @brief Loop skill - repeat a prompt task until stop condition or budget.
///
/// CORRECT IMPLEMENTATION (replaces previous popen-based version):
/// - Loop body delegates to query_engine via a caller-provided callback
/// - Stop conditions: explicit regex, implicit (identical output x2, LLM says stop)
/// - Guards: max_iterations, sleep_between_ms, wall-clock timeout
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <format>
#include <chrono>
#include <thread>
#include <regex>
#include <algorithm>
#include <cstdint>

export module cc.skills.bundled.loop;

import cc.skills.skill;
import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// ============================================================
// Types
// ============================================================

/// How the stop condition should be evaluated
enum class StopConditionType : std::uint8_t {
    Regex,           // Match output against a regex pattern (explicit)
    ContainsText,    // Output contains specific substring(s)
    Implicit,        // Consecutive identical output OR LLM self-signals stop
    RegexOrImplicit, // Match explicit regex OR trigger implicit (default)
};

/// Parsed loop configuration
struct LoopConfig {
    std::string loop_body;                    // Prompt to execute each iteration
    StopConditionType stop_type = StopConditionType::RegexOrImplicit;
    std::optional<std::string> stop_regex;    // For Regex / RegexOrImplicit
    std::vector<std::string> stop_keywords;   // For ContainsText (e.g. {"DONE", "SUCCESS"})
    int max_iterations = 10;                  // Hard cap on iterations (default 10)
    std::chrono::milliseconds sleep_between{0}; // Sleep between iterations
    std::chrono::milliseconds timeout{std::chrono::minutes(10)}; // Wall-clock timeout
    int implicit_stable_rounds = 2;           // Consecutive identical rounds = done (implicit)
    bool verbose = true;                      // Include per-iteration markers
};

/// Result of running a loop
struct LoopResult {
    std::vector<std::string> iteration_outputs;
    int iterations_run = 0;
    std::string stop_reason;                  // Why the loop exited
    bool timed_out = false;
    bool hit_max_iter = false;
    bool condition_met = false;
};

/// Callback type for executing one iteration.
/// The loop skill delegates LLM interaction to the caller (query_engine)
/// to avoid duplicating API/streaming logic.
using IterationExecutor = std::function<std::expected<std::string, std::string>(
    std::string_view prompt,      // The loop body (potentially modified per-iter)
    int iteration_number          // 1-based iteration counter
)>;

// ============================================================
// Parsing helpers
// ============================================================

namespace detail {

/// Parse "[interval] <prompt>" style argument (mirrors TS loop.ts parsing)
/// Returns {interval_ms, prompt} or error.
inline std::expected<std::pair<std::chrono::milliseconds, std::string>, std::string>
parse_interval_prompt(std::string_view args) {
    if (args.empty()) {
        return std::unexpected("Usage: /loop [interval] <prompt>\n"
            "  Intervals: Ns, Nm, Nh, Nd (e.g. 5m, 30m, 2h, 1d)\n"
            "  Default: 10m");
    }

    std::string input(args);

    // Rule 1: Leading token like ^\d+[smhd]$
    {
        auto first_space = input.find(' ');
        if (first_space != std::string::npos) {
            std::string token = input.substr(0, first_space);
            if (token.size() >= 2) {
                char unit = token.back();
                std::string num_str = token.substr(0, token.size() - 1);
                if ((unit == 's' || unit == 'm' || unit == 'h' || unit == 'd') &&
                    std::ranges::all_of(num_str, ::isdigit)) {
                    long n = std::stol(num_str);
                    std::chrono::milliseconds interval{0};
                    switch (unit) {
                        case 's': interval = std::chrono::seconds(n); break;
                        case 'm': interval = std::chrono::minutes(n); break;
                        case 'h': interval = std::chrono::hours(n);   break;
                        case 'd': interval = std::chrono::hours(n*24); break;
                    }
                    // Minimum interval: 1 minute (per TS spec)
                    if (interval < std::chrono::minutes(1))
                        interval = std::chrono::minutes(1);
                    std::string rest = input.substr(first_space + 1);
                    // Trim leading whitespace
                    auto pos = rest.find_first_not_of(" \t");
                    if (pos != std::string::npos) rest = rest.substr(pos);
                    if (rest.empty()) {
                        return std::unexpected("Empty prompt after interval. "
                            "Usage: /loop [interval] <prompt>");
                    }
                    return std::make_pair(interval, rest);
                }
            }
        }
    }

    // Rule 2: Trailing "every N<unit>" or "every N <unit-word>"
    // (Simplified: scan for "every " at the end, convert to ms)
    // Default: fall through.

    // Rule 3: Default interval 10m, entire input is prompt
    return std::make_pair(std::chrono::minutes(10), input);
}

/// Normalize output for implicit-stable comparison
/// (trim trailing whitespace, normalize newlines)
inline std::string normalize_for_compare(std::string_view s) {
    std::string result(s);
    // Normalize line endings
    size_t pos;
    while ((pos = result.find("\r\n")) != std::string::npos)
        result.replace(pos, 2, "\n");
    // Trim trailing whitespace
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == ' ' || result.back() == '\t'))
        result.pop_back();
    return result;
}

/// Check if LLM self-signals stop via explicit phrases
inline bool llm_self_stopped(std::string_view output) {
    static constexpr std::array<std::string_view, 8> stop_phrases = {
        "stopping the loop",
        "loop complete",
        "iteration complete and stopping",
        "stopping now",
        "condition met",
        "exit loop",
        "breaking the loop",
        "task complete, stopping",
    };
    std::string lower(output);
    std::ranges::transform(lower, lower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    for (auto phrase : stop_phrases) {
        std::string phrase_lower(phrase);
        std::ranges::transform(phrase_lower, phrase_lower.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (lower.find(phrase_lower) != std::string::npos) return true;
    }
    return false;
}

/// Match stop regex (if set) against output
inline bool regex_matches(std::string_view output, std::string_view pattern) {
    if (pattern.empty()) return false;
    try {
        std::regex re(std::string(pattern),
            std::regex::icase | std::regex::optimize);
        return std::regex_search(output.begin(), output.end(), re);
    } catch (const std::regex_error&) {
        // Fallback: plain substring match for invalid regex
        return output.find(pattern) != std::string::npos;
    }
}

/// Check keyword containment
inline bool keywords_match(std::string_view output,
                           const std::vector<std::string>& keywords) {
    if (keywords.empty()) return false;
    for (const auto& kw : keywords) {
        if (output.find(kw) != std::string::npos) return true;
    }
    return false;
}

} // namespace detail

// ============================================================
// Core loop executor
// ============================================================

/// Run a structured loop: call executor on each iteration, check stop
/// conditions, enforce max iterations, sleep, and timeout.
///
/// @param config   Parsed loop configuration
/// @param executor Caller-supplied iteration executor (delegates to query_engine)
/// @return LoopResult with per-iteration outputs and stop diagnostics
inline std::expected<LoopResult, std::string> run_loop(
    const LoopConfig& config,
    IterationExecutor executor
) {
    if (!executor) {
        return std::unexpected("Loop executor callback is required");
    }
    if (config.loop_body.empty()) {
        return std::unexpected("Loop body (prompt) cannot be empty");
    }
    if (config.max_iterations <= 0) {
        return std::unexpected("max_iterations must be positive");
    }

    LoopResult result;
    result.iteration_outputs.reserve(config.max_iterations);

    const auto start_time = std::chrono::steady_clock::now();
    std::vector<std::string> stable_history; // normalized outputs (last N)

    for (int iter = 1; iter <= config.max_iterations; ++iter) {
        // Timeout check (at start of each iteration)
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        if (elapsed >= config.timeout) {
            result.timed_out = true;
            result.stop_reason = std::format(
                "Loop timed out after {}ms (limit: {}ms)",
                elapsed.count(), config.timeout.count());
            break;
        }

        // Execute one iteration via caller-provided callback (query_engine)
        auto step = executor(config.loop_body, iter);
        std::string output;
        if (step) {
            output = std::move(*step);
        } else {
            output = std::format("[Error at iteration {}] {}", iter, step.error());
        }
        result.iteration_outputs.push_back(output);
        result.iterations_run = iter;

        // ---- Stop condition evaluation ----
        bool stop_now = false;

        // 1. Explicit regex match
        if (config.stop_type == StopConditionType::Regex ||
            config.stop_type == StopConditionType::RegexOrImplicit) {
            if (config.stop_regex &&
                detail::regex_matches(output, *config.stop_regex)) {
                stop_now = true;
                result.condition_met = true;
                result.stop_reason = std::format(
                    "Stop regex matched at iteration {}: /{}/",
                    iter, *config.stop_regex);
            }
        }

        // 2. Keyword match
        if (!stop_now &&
            config.stop_type == StopConditionType::ContainsText) {
            if (detail::keywords_match(output, config.stop_keywords)) {
                stop_now = true;
                result.condition_met = true;
                result.stop_reason = std::format(
                    "Stop keyword matched at iteration {}", iter);
            }
        }

        // 3. Implicit stop conditions
        if (!stop_now &&
            (config.stop_type == StopConditionType::Implicit ||
             config.stop_type == StopConditionType::RegexOrImplicit)) {
            // a) LLM self-signaled
            if (detail::llm_self_stopped(output)) {
                stop_now = true;
                result.condition_met = true;
                result.stop_reason = std::format(
                    "LLM self-signaled stop at iteration {}", iter);
            }

            // b) Consecutive identical normalized outputs
            if (!stop_now) {
                auto normalized = detail::normalize_for_compare(output);
                stable_history.push_back(normalized);
                if (static_cast<int>(stable_history.size()) >
                    config.implicit_stable_rounds) {
                    stable_history.erase(stable_history.begin());
                }
                if (static_cast<int>(stable_history.size()) >=
                    config.implicit_stable_rounds &&
                    config.implicit_stable_rounds >= 2) {
                    bool all_same = true;
                    const auto& ref = stable_history.back();
                    for (size_t i = 0; i + 1 < stable_history.size(); ++i) {
                        if (stable_history[i] != ref) { all_same = false; break; }
                    }
                    if (all_same && !ref.empty()) {
                        stop_now = true;
                        result.condition_met = true;
                        result.stop_reason = std::format(
                            "Consecutive identical outputs (x{}) at iteration {}",
                            config.implicit_stable_rounds, iter);
                    }
                }
            }
        }

        if (stop_now) break;

        // Sleep before next iteration (skip on final iteration)
        if (iter < config.max_iterations &&
            config.sleep_between.count() > 0) {
            std::this_thread::sleep_for(config.sleep_between);
        }
    }

    // Post-loop diagnostics
    if (result.stop_reason.empty()) {
        if (result.iterations_run >= config.max_iterations) {
            result.hit_max_iter = true;
            result.stop_reason = std::format(
                "Reached maximum iterations: {}", config.max_iterations);
        } else {
            result.stop_reason = "Loop completed";
        }
    }

    return result;
}

// ============================================================
// Skill manifest + SkillDefinition
// ============================================================

/// Get the SkillManifest for directory-based discovery (load_skills_dir)
[[nodiscard]] inline cc::skills::SkillManifest get_loop_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "loop",
        .description =
            "Run a prompt iteratively until a stop condition, max iterations, "
            "or timeout. Supports regex stop conditions, implicit convergence "
            "detection, sleep between rounds, and wall-clock timeout.",
        .version = "1.1.0",
        .triggers = {
            "loop", "repeat", "iterate", "keep trying", "run until",
            "keep doing", "until done", "while not", "loop until",
            "convergence loop", "auto-refine",
        },
        .directory = {}
    };
}

/// SkillDefinition for registration in SkillExecutor / bundled registry.
/// The skill prompt instructs the LLM how to structure a loop request;
/// the actual execution uses run_loop() above via query_engine.
[[nodiscard]] inline SkillDefinition make_bundled_loop_skill() {
    return SkillDefinition{
        .name = "loop",
        .description =
            "Iterative refinement loop: run a task repeatedly until a "
            "condition is met or budget exhausted. Use when user wants "
            "\"keep trying until X\", \"iterate until DONE\", "
            "\"refine until no more changes\".",
        .trigger_patterns = {
            R"(iterati(?:ve|on)\s+loop)",
            R"(loop\s+(?:until|while))",
            R"(keep\s+(?:going|trying|improving|refining|doing))",
            R"(not\s+(?:good|right|done)\s+yet)",
            R"(run\s+until\s+)",
            R"(repeat\s+until\s+)",
            R"(converge(?:nce)?\s+loop)",
            R"(auto\s*-?refin(?:e|ing))",
        },
        .content = R"(## Iterative Loop Skill

### When to use
- User says "keep trying until it works" or "loop until condition X"
- Tasks that converge over multiple rounds (e.g. "refine until output is stable")
- Polling patterns ("check until deploy is green")
- Budget-limited exploration (try N strategies)

### Loop Structure (parse from user request)
Extract:
1. **loop_body**: The task/prompt to run each iteration
2. **stop_condition**:
   - Explicit regex (e.g. output contains "DONE" or matches /PASS/)
   - Explicit keyword list (e.g. {"DONE", "all tests pass"})
   - Implicit: detect convergence (2+ identical outputs) or LLM says stop
   - Default: RegexOrImplicit
3. **max_iterations**: Hard cap (default 10, max 50)
4. **sleep_between**: Delay between iterations (default 0)
5. **timeout**: Wall-clock timeout (default 10 min)

### Execution Contract
- Run loop_body each iteration using query_engine (NOT subprocess/popen)
- After each iteration:
  a. Check explicit regex/keyword against output
  b. Check implicit: did LLM say "stopping loop" etc.?
  c. Check implicit: are last N outputs identical?
- Sleep before next iteration
- Report stop_reason when loop exits

### Exit Conditions (in priority order)
1. Wall-clock timeout (prevents runaway loops)
2. Explicit stop condition met (regex/keyword)
3. Implicit: LLM self-signaled stop
4. Implicit: N consecutive identical outputs (default N=2)
5. max_iterations reached
6. Error in executor (capture error text, continue if not stop_condition)

### Anti-patterns to Avoid
- Never use popen/system for the loop body — always delegate to query_engine
- Never run unbounded loops; always have max_iterations + timeout
- Never skip stop condition checks on error outputs (errors may be the "done" signal)
- Gold-plating: stop when explicit condition is met, don't keep iterating
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.1.0",
    };
}

} // namespace cc::skills::bundled
