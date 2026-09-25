// swarm_backends_shell.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). The injectable shell seams: the two override
// accessors (each owning its function-local std::function static, so the
// whole module — tests, tmux backend, registry — shares one instance),
// test install/reset, run_shell/command_available over std::system, and
// read_shell_output[_with_status] over cc::utils::bash popen_spawn.
//
// <sys/wait.h> is a global-module-fragment header because WIFEXITED /
// WEXITSTATUS are preprocessor macros that cannot arrive through
// `import std;`; <cstdio> declares FILE for popen_spawn, and <cstdlib>
// declares std::system.
module;

#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>

module cc.utils.swarm_backends;

import std;

import cc.utils.bash_execution;

namespace cc::utils::swarm_backends::detail {

ShellRunnerFn& shell_runner_override() {
    static ShellRunnerFn runner;
    return runner;
}

ShellCaptureFn& shell_capture_override() {
    static ShellCaptureFn capture;
    return capture;
}

void set_shell_runners_for_test(ShellRunnerFn runner, ShellCaptureFn capture) {
    shell_runner_override() = std::move(runner);
    shell_capture_override() = std::move(capture);
}

void reset_shell_runners_for_test() {
    shell_runner_override() = nullptr;
    shell_capture_override() = nullptr;
}

int run_shell(std::string_view command) {
    if (const auto& runner = shell_runner_override()) return runner(command);
    return std::system(std::string(command).c_str());
}

bool command_available(std::string_view command) {
    auto probe = "command -v " + std::string(command) + " >/dev/null 2>&1";
    return run_shell(probe) == 0;
}

std::string read_shell_output(std::string_view command) {
    if (const auto& capture = shell_capture_override()) return capture(command);
    FILE* pipe = cc::utils::bash::popen_spawn(std::string(command).c_str());
    if (!pipe) return {};
    std::array<char, 4096> buffer{};
    std::string output;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    (void)cc::utils::bash::pclose_spawn(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}

ShellOutput read_shell_output_with_status(std::string_view command) {
    FILE* pipe = cc::utils::bash::popen_spawn(std::string(command).c_str());
    if (!pipe) return {};
    std::array<char, 4096> buffer{};
    std::string output;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    const int raw = cc::utils::bash::pclose_spawn(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return ShellOutput{std::move(output), WIFEXITED(raw) ? WEXITSTATUS(raw) : -1};
}

} // namespace cc::utils::swarm_backends::detail
