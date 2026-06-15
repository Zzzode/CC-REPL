module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <expected>

export module cc.tools.script_sandbox;
import cc.utils.bash_execution;

export namespace cc::tools {

namespace detail {
[[nodiscard]] inline auto shell_quote(std::string_view s) -> std::string {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += R"('\'')";
        else out += ch;
    }
    out += "'";
    return out;
}

[[nodiscard]] inline auto is_within_root(const std::filesystem::path& root, const std::filesystem::path& path) -> bool {
    namespace fs = std::filesystem;
    const auto canonical_root = fs::weakly_canonical(root);
    const auto canonical_path = fs::weakly_canonical(path);
    auto root_it = canonical_root.begin();
    auto path_it = canonical_path.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++path_it) {
        if (path_it == canonical_path.end() || *root_it != *path_it) return false;
    }
    return true;
}
}


struct SandboxConfig {
    std::filesystem::path root_dir;
    std::vector<std::filesystem::path> allowed_reads;
    std::vector<std::filesystem::path> allowed_writes;
    bool network_access = false;
    std::chrono::seconds timeout{30};
};


inline auto create_sandbox(const SandboxConfig& config) -> std::expected<void, std::string> {
    namespace fs = std::filesystem;


    if (!fs::exists(config.root_dir)) {
        return std::unexpected("Sandbox root directory does not exist: " + config.root_dir.string());
    }


    if (!fs::is_directory(config.root_dir)) {
        return std::unexpected("Sandbox root is not a directory: " + config.root_dir.string());
    }


    for (const auto& read_path : config.allowed_reads) {
        auto resolved = read_path.is_relative() ? config.root_dir / read_path : read_path;
        if (!fs::exists(resolved)) {
            return std::unexpected("Allowed read path does not exist: " + resolved.string());
        }
        if (!detail::is_within_root(config.root_dir, resolved)) {
            return std::unexpected("Allowed read path escapes sandbox root: " + resolved.string());
        }
    }


    for (const auto& write_path : config.allowed_writes) {
        auto resolved = write_path.is_relative() ? config.root_dir / write_path : write_path;
        auto parent = resolved.parent_path();
        if (!fs::exists(parent)) {
            return std::unexpected("Parent of allowed write path does not exist: " + parent.string());
        }
        if (!detail::is_within_root(config.root_dir, parent)) {
            return std::unexpected("Allowed write path escapes sandbox root: " + resolved.string());
        }
    }

    return {};
}


inline auto execute_in_sandbox(
    std::string_view script,
    const SandboxConfig& config
) -> std::expected<std::string, std::string> {

    auto validation = create_sandbox(config);
    if (!validation) {
        return std::unexpected(validation.error());
    }


    if (script.empty()) {
        return std::unexpected("Empty script provided");
    }


    static const std::vector<std::string_view> escape_patterns = {
        "chroot", "unshare", "nsenter",
        "/proc/self", "/proc/1",
        "ptrace", "LD_PRELOAD",
        "mount ", "umount "
    };

    for (const auto& pattern : escape_patterns) {
        if (script.find(pattern) != std::string_view::npos) {
            return std::unexpected(
                "Script contains potentially dangerous pattern: " + std::string(pattern)
            );
        }
    }

    namespace fs = std::filesystem;
    const auto script_path = config.root_dir / ".cc_sandbox_script.sh";
    {
        std::ofstream script_file(script_path);
        if (!script_file) {
            return std::unexpected("Failed to create sandbox script: " + script_path.string());
        }
        script_file << "#!/bin/sh\nset -eu\n" << script << '\n';
    }

    const auto command = "cd " + detail::shell_quote(config.root_dir.string()) +
        " && /bin/sh " + detail::shell_quote(script_path.string()) + " 2>&1";
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = cc::utils::bash::popen_spawn(command.c_str());
    if (!pipe) {
        fs::remove(script_path);
        return std::unexpected("Failed to spawn sandboxed shell");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int status = cc::utils::bash::pclose_spawn(pipe);
    fs::remove(script_path);
    if (status != 0) {
        return std::unexpected("Sandboxed script exited with non-zero status:\n" + output);
    }
    return output;
}


inline auto is_sandboxed() -> bool {
    return std::getenv("APP_SANDBOX_CONTAINER_ID") != nullptr ||
           std::getenv("CC_REPL_SANDBOX") != nullptr;
}

} // namespace cc::tools
