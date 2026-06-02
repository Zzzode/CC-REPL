module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <array>
#include <cstdio>
#include <sstream>

export module cc.utils.exec_sync;

export namespace cc::utils {

// Execute a command synchronously and return stdout
inline std::expected<std::string, std::string>
exec_sync(std::string_view command) {
    std::string cmd(command);
    // Redirect stderr to /dev/null for clean stdout capture
    cmd += " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to execute command: " + std::string(command));
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = pclose(pipe);
    if (status != 0) {
        return std::unexpected("Command failed with exit code " +
            std::to_string(WEXITSTATUS(status)));
    }

    // Trim trailing newline
    while (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }

    return output;
}

// Execute a command and return only the exit code
inline int exec_sync_status(std::string_view command) {
    std::string cmd(command);
    cmd += " >/dev/null 2>&1";

    int status = system(cmd.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Execute a command and return stdout as a vector of lines
inline std::expected<std::vector<std::string>, std::string>
exec_sync_lines(std::string_view command) {
    auto result = exec_sync(command);
    if (!result) return std::unexpected(result.error());

    std::vector<std::string> lines;
    std::istringstream stream(*result);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace cc::utils
