module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

export module cc.utils.find_executable;

export namespace cc::utils {

namespace fs = std::filesystem;

// Get all directories in the PATH environment variable
inline std::vector<fs::path> get_path_dirs() {
    std::vector<fs::path> dirs;
    const char* path_env = std::getenv("PATH");
    if (!path_env) return dirs;

    std::istringstream stream(path_env);
    std::string segment;
    while (std::getline(stream, segment, ':')) {
        if (!segment.empty()) {
            dirs.emplace_back(segment);
        }
    }
    return dirs;
}

// Check if a file is executable
inline bool is_executable(const fs::path& p) {
    if (!fs::exists(p) || !fs::is_regular_file(p)) return false;
    return access(p.c_str(), X_OK) == 0;
}

// Find the first executable with the given name in PATH
inline std::optional<fs::path> find_executable(std::string_view name) {
    // If the name contains a path separator, check directly
    if (name.find('/') != std::string_view::npos) {
        fs::path p(name);
        if (is_executable(p)) return p;
        return std::nullopt;
    }

    for (const auto& dir : get_path_dirs()) {
        fs::path candidate = dir / std::string(name);
        if (is_executable(candidate)) {
            return fs::canonical(candidate);
        }
    }
    return std::nullopt;
}

// Find all executables with the given name in PATH
inline std::vector<fs::path> find_all_executables(std::string_view name) {
    std::vector<fs::path> results;

    if (name.find('/') != std::string_view::npos) {
        fs::path p(name);
        if (is_executable(p)) results.push_back(p);
        return results;
    }

    for (const auto& dir : get_path_dirs()) {
        fs::path candidate = dir / std::string(name);
        if (is_executable(candidate)) {
            results.push_back(fs::canonical(candidate));
        }
    }
    return results;
}

} // namespace cc::utils
