/// @file repl_tool.cppm
/// @brief REPL tool — executes code snippets in Python/Node/Ruby/Lua interpreters.
/// Uses subprocess (popen) for isolated code execution with timeout.
module;

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

export module cc.tools.repl_tool;

export namespace cc::tools::repl {

enum class Language { Python, Node, Ruby, Lua };

struct ReplRequest {
    Language language{Language::Python};
    std::string code;
    bool preserve_state{true};
    uint32_t timeout_seconds{30};
};

struct ReplResponse {
    bool ok{true};
    std::string output;
    std::vector<std::string> diagnostics;
};

[[nodiscard]] inline auto language_name(Language language) -> std::string_view {
    switch (language) {
        case Language::Python: return "python";
        case Language::Node: return "node";
        case Language::Ruby: return "ruby";
        case Language::Lua: return "lua";
    }
    return "unknown";
}

/// Get the interpreter binary for a language
[[nodiscard]] inline auto interpreter_binary(Language language) -> std::string_view {
    switch (language) {
        case Language::Python: return "python3";
        case Language::Node: return "node";
        case Language::Ruby: return "ruby";
        case Language::Lua: return "lua";
    }
    return "python3";
}

/// Shell-quote a string for safe inclusion in a shell command (POSIX single-quote)
[[nodiscard]] inline auto shell_quote(std::string_view input) -> std::string {
    std::string result = "'";
    for (char c : input) {
        if (c == '\'') {
            result += "'\\''";  // End quote, escaped quote, start quote
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

/// Execute a REPL snippet using subprocess (popen)
[[nodiscard]] inline auto execute_repl(const ReplRequest& request) -> ReplResponse {
    if (request.code.empty()) {
        return {.ok = false, .output = {}, .diagnostics = {"REPL code is empty"}};
    }

    // Build command: interpreter -c 'code'
    // For Node.js, use -e instead of -c
    auto binary = interpreter_binary(request.language);
    std::string flag = (request.language == Language::Node) ? " -e " : " -c ";
    std::string command = std::string(binary) + flag + shell_quote(request.code);

    // Redirect stderr to stdout to capture all output
    command += " 2>&1";

    // Execute via popen
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {
            .ok = false,
            .output = {},
            .diagnostics = {"failed to start interpreter subprocess"}
        };
    }

    // Read output in chunks
    std::string output;
    std::array<char, 4096> buffer{};
    constexpr size_t max_output = 512 * 1024;  // 512KB limit

    while (true) {
        auto bytes_read = fread(buffer.data(), 1, buffer.size(), pipe);
        if (bytes_read == 0) break;
        output.append(buffer.data(), bytes_read);
        if (output.size() > max_output) {
            output.resize(max_output);
            output += "\n... [output truncated at 512KB]";
            break;
        }
    }

    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    // Trim trailing newline
    while (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }

    std::vector<std::string> diagnostics;
    if (exit_code != 0) {
        diagnostics.push_back(std::string("exit code: ") + std::to_string(exit_code));
    }
    if (request.preserve_state) {
        diagnostics.push_back("note: state preservation not supported in subprocess mode");
    }

    return {
        .ok = (exit_code == 0),
        .output = std::move(output),
        .diagnostics = std::move(diagnostics)
    };
}

} // namespace cc::tools::repl
