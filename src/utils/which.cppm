module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <unistd.h>

export module cc.utils.which;

export namespace cc::utils {

namespace fs = std::filesystem;

// which(1) equivalent — find a program in PATH
inline std::optional<fs::path> which(std::string_view program) {
    if (program.empty()) return std::nullopt;

    // Absolute or relative path given directly
    if (program.find('/') != std::string_view::npos) {
        fs::path p(program);
        if (fs::exists(p) && access(p.c_str(), X_OK) == 0) {
            return fs::canonical(p);
        }
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;

    std::istringstream stream(path_env);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
        if (dir.empty()) dir = ".";
        fs::path candidate = fs::path(dir) / std::string(program);
        if (fs::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
            return fs::canonical(candidate);
        }
    }
    return std::nullopt;
}

// Find all matching executables in PATH (like which -a)
inline std::vector<fs::path> which_all(std::string_view program) {
    std::vector<fs::path> results;
    if (program.empty()) return results;

    if (program.find('/') != std::string_view::npos) {
        fs::path p(program);
        if (fs::exists(p) && access(p.c_str(), X_OK) == 0) {
            results.push_back(fs::canonical(p));
        }
        return results;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return results;

    std::istringstream stream(path_env);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
        if (dir.empty()) dir = ".";
        fs::path candidate = fs::path(dir) / std::string(program);
        if (fs::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
            results.push_back(fs::canonical(candidate));
        }
    }
    return results;
}

// Check if a command exists in PATH
inline bool has_command(std::string_view program) {
    return which(program).has_value();
}

} // namespace cc::utils
