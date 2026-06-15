module;
#include <string>
#include <vector>
#include <expected>
#include <thread>
#include <mutex>
#include <cstdio>
#include <array>
#include <atomic>

export module cc.skills.bundled.batch;

import cc.skills.load_skills_dir;
import cc.utils.bash_execution;

export namespace cc::skills::bundled {

// Configuration for batch execution of multiple commands
struct BatchConfig {
    std::vector<std::string> commands;
    bool parallel;
    bool stop_on_error;
};

namespace detail {

/// Execute a shell command and capture its output
inline std::expected<std::string, std::string> exec_command(const std::string& cmd) {
    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) {
        return std::unexpected("Failed to execute: " + cmd);
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
    }

    int status = cc::utils::bash::pclose_spawn(pipe);
    if (status != 0) {
        if (output.empty()) {
            return std::unexpected("Command failed with exit code " +
                std::to_string(WEXITSTATUS(status)) + ": " + cmd);
        }
        // Return output even on failure for context
        return std::unexpected(output);
    }
    return output;
}

} // namespace detail

// Execute a batch of commands sequentially or in parallel
std::expected<std::vector<std::string>, std::string> execute_batch(BatchConfig config) {
    if (config.commands.empty()) {
        return std::unexpected("No commands specified in batch");
    }

    std::vector<std::string> results;
    results.resize(config.commands.size());

    if (config.parallel) {
        // Execute commands in parallel using threads
        std::vector<std::thread> threads;
        threads.reserve(config.commands.size());
        std::mutex results_mutex;
        std::atomic<bool> has_error{false};
        std::string first_error;

        for (size_t i = 0; i < config.commands.size(); ++i) {
            threads.emplace_back([&, i]() {
                if (config.stop_on_error && has_error.load(std::memory_order_relaxed)) {
                    std::lock_guard lock(results_mutex);
                    results[i] = "[Skipped due to earlier error]";
                    return;
                }

                auto result = detail::exec_command(config.commands[i]);
                std::lock_guard lock(results_mutex);
                if (result) {
                    results[i] = std::move(*result);
                } else {
                    results[i] = "[Error] " + result.error();
                    if (config.stop_on_error && !has_error.exchange(true)) {
                        first_error = result.error();
                    }
                }
            });
        }

        // Join all threads
        for (auto& t : threads) {
            t.join();
        }

        if (config.stop_on_error && has_error.load()) {
            return std::unexpected("Batch stopped: " + first_error);
        }
    } else {
        // Sequential execution
        for (size_t i = 0; i < config.commands.size(); ++i) {
            const auto& cmd = config.commands[i];

            if (cmd.empty()) {
                if (config.stop_on_error) {
                    return std::unexpected("Empty command encountered, stopping batch");
                }
                results[i] = "[Skipped: empty command]";
                continue;
            }

            auto result = detail::exec_command(cmd);
            if (result) {
                results[i] = std::move(*result);
            } else {
                if (config.stop_on_error) {
                    return std::unexpected("Command failed: " + result.error());
                }
                results[i] = "[Error] " + result.error();
            }
        }
    }

    return results;
}

// Get the skill manifest for the batch skill
cc::skills::SkillManifest get_batch_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "batch",
        .description = "Execute multiple commands in sequence or parallel",
        .version = "1.0.0",
        .triggers = {"run batch", "execute batch", "batch commands", "run all"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
