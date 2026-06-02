module;
#include <string>
#include <string_view>
#include <functional>
#include <expected>
#include <cstdio>
#include <array>
#include <format>

export module cc.skills.bundled.loop;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// Configuration for a loop skill execution
struct LoopConfig {
    std::string task;
    int max_iterations;
    std::function<bool(std::string_view)> stop_condition;
};

namespace detail {

/// Execute a task as a shell command and return its output
inline std::expected<std::string, std::string> execute_task(const std::string& task) {
    FILE* pipe = ::popen(task.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to execute task: " + task);
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
    }

    int status = ::pclose(pipe);
    if (status != 0 && output.empty()) {
        return std::unexpected(std::format("Task exited with code {}: {}",
            WEXITSTATUS(status), task));
    }
    return output;
}

} // namespace detail

// Run a task in a loop until stop condition is met or max iterations reached
std::expected<std::string, std::string> run_loop(LoopConfig config) {
    if (config.task.empty()) {
        return std::unexpected("Loop task cannot be empty");
    }

    if (config.max_iterations <= 0) {
        return std::unexpected("max_iterations must be positive");
    }

    std::string accumulated_output;
    int iteration = 0;

    while (iteration < config.max_iterations) {
        ++iteration;

        // Execute the task
        auto result = detail::execute_task(config.task);
        std::string iteration_result;

        if (result) {
            iteration_result = *result;
        } else {
            iteration_result = std::format("[Error at iteration {}] {}",
                iteration, result.error());
        }

        accumulated_output += std::format("--- Iteration {} ---\n{}\n",
            iteration, iteration_result);

        // Check stop condition if provided
        if (config.stop_condition && config.stop_condition(iteration_result)) {
            accumulated_output += std::format(
                "[Stop condition met at iteration {}]\n", iteration);
            break;
        }

        // If command failed and no stop_condition provided, stop by default
        if (!result && !config.stop_condition) {
            accumulated_output += std::format(
                "[Stopping: task failed at iteration {}]\n", iteration);
            break;
        }
    }

    if (iteration >= config.max_iterations) {
        accumulated_output += std::format(
            "[Reached maximum iterations: {}]\n", config.max_iterations);
    }

    return accumulated_output;
}

// Get the skill manifest for the loop skill
cc::skills::SkillManifest get_loop_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "loop",
        .description = "Repeatedly execute a task until a condition is met",
        .version = "1.0.0",
        .triggers = {"loop", "repeat", "iterate", "keep trying", "run until"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
