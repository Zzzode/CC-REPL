/// @file context.cppm
/// @brief Context module that provides runtime environment information,
/// git repository state, file system context, and cwd information.
/// Collects ambient context about the environment.
module;

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include <fstream>
#include <format>
#include <unordered_map>
#include <array>
#include <algorithm>
#include <sstream>

export module cc.context.context;
import cc.utils.bash_execution;

export namespace cc::core {

// ============================================================
// Git Status
// ============================================================

/// Git repository status
struct GitStatus {
    std::optional<std::string> current_branch;
    std::vector<std::string> modified_files;
    std::vector<std::string> staged_files;
    std::vector<std::string> untracked_files;
    bool is_git_repository = false;
    bool has_remote = false;
    std::optional<std::string> last_commit;
    int ahead{0};
    int behind{0};
};

// ============================================================
// Project Context
// ============================================================

/// Detected package manager
enum class PackageManager {
    None,
    Npm,
    Yarn,
    Pnpm,
    Cargo,
    CMake,
    Pip,
    Poetry,
    Go,
    Maven,
    Gradle
};

/// Information about the current project
struct ProjectContext {
    std::filesystem::path root_directory;
    std::string project_name;
    std::vector<std::string> detected_languages;
    PackageManager package_manager{PackageManager::None};
    std::vector<std::string> config_files;
    bool has_git = false;
    bool has_ci = false;
    std::optional<std::string> project_version;
};

// ============================================================
// Internal Helpers
// ============================================================

namespace detail {

inline std::string exec_git(const std::string& cmd, const std::filesystem::path& cwd) {
    std::string full_cmd = "cd " + cwd.string() + " && git " + cmd + " 2>/dev/null";
    auto pipe_cap = cc::utils::bash::exec_capture(full_cmd.c_str());
    if (!pipe_cap) return {};
    std::string output = std::move(pipe_cap->output);
    // Trim trailing newline
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

inline std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

inline PackageManager detect_package_manager(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    if (fs::exists(root / "pnpm-lock.yaml")) return PackageManager::Pnpm;
    if (fs::exists(root / "yarn.lock")) return PackageManager::Yarn;
    if (fs::exists(root / "package-lock.json")) return PackageManager::Npm;
    if (fs::exists(root / "package.json")) return PackageManager::Npm;
    if (fs::exists(root / "Cargo.toml")) return PackageManager::Cargo;
    if (fs::exists(root / "go.mod")) return PackageManager::Go;
    if (fs::exists(root / "CMakeLists.txt")) return PackageManager::CMake;
    if (fs::exists(root / "poetry.lock")) return PackageManager::Poetry;
    if (fs::exists(root / "requirements.txt") || fs::exists(root / "setup.py") ||
        fs::exists(root / "pyproject.toml")) return PackageManager::Pip;
    if (fs::exists(root / "pom.xml")) return PackageManager::Maven;
    if (fs::exists(root / "build.gradle") || fs::exists(root / "build.gradle.kts")) {
        return PackageManager::Gradle;
    }
    return PackageManager::None;
}

inline std::vector<std::string> detect_languages(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::vector<std::string> langs;

    if (fs::exists(root / "package.json") || fs::exists(root / "tsconfig.json")) {
        if (fs::exists(root / "tsconfig.json")) {
            langs.push_back("TypeScript");
        } else {
            langs.push_back("JavaScript");
        }
    }
    if (fs::exists(root / "Cargo.toml")) langs.push_back("Rust");
    if (fs::exists(root / "go.mod")) langs.push_back("Go");
    if (fs::exists(root / "CMakeLists.txt")) langs.push_back("C++");
    if (fs::exists(root / "setup.py") || fs::exists(root / "pyproject.toml") ||
        fs::exists(root / "requirements.txt")) {
        langs.push_back("Python");
    }
    if (fs::exists(root / "pom.xml") || fs::exists(root / "build.gradle") ||
        fs::exists(root / "build.gradle.kts")) {
        langs.push_back("Java");
    }
    if (fs::exists(root / "Package.swift")) langs.push_back("Swift");
    if (fs::exists(root / "Gemfile")) langs.push_back("Ruby");
    if (fs::exists(root / "mix.exs")) langs.push_back("Elixir");

    return langs;
}

inline std::vector<std::string> find_config_files(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::vector<std::string> configs;
    static const std::vector<std::string> known_configs = {
        "package.json", "tsconfig.json", "Cargo.toml", "go.mod",
        "CMakeLists.txt", "Makefile", ".eslintrc.json", ".eslintrc.js",
        ".prettierrc", "jest.config.js", "jest.config.ts",
        "webpack.config.js", "vite.config.ts", "rollup.config.js",
        ".github/workflows", ".gitlab-ci.yml", "Jenkinsfile",
        "docker-compose.yml", "Dockerfile", ".env.example",
        "pyproject.toml", "setup.py", "setup.cfg"
    };
    for (const auto& name : known_configs) {
        if (fs::exists(root / name)) {
            configs.push_back(name);
        }
    }
    return configs;
}

inline bool detect_ci(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    return fs::exists(root / ".github" / "workflows") ||
           fs::exists(root / ".gitlab-ci.yml") ||
           fs::exists(root / "Jenkinsfile") ||
           fs::exists(root / ".circleci") ||
           fs::exists(root / ".travis.yml");
}

inline std::optional<std::string> detect_version(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    // Try package.json
    if (fs::exists(root / "package.json")) {
        std::ifstream ifs(root / "package.json");
        std::string content((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
        auto pos = content.find("\"version\"");
        if (pos != std::string::npos) {
            auto colon = content.find(':', pos);
            auto quote1 = content.find('"', colon + 1);
            auto quote2 = content.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                return content.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }
    }
    // Try Cargo.toml
    if (fs::exists(root / "Cargo.toml")) {
        std::ifstream ifs(root / "Cargo.toml");
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.find("version") != std::string::npos && line.find('=') != std::string::npos) {
                auto eq = line.find('=');
                auto q1 = line.find('"', eq);
                auto q2 = line.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    return line.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace detail

// ============================================================
// Runtime Context
// ============================================================

/// Complete runtime environment context
class RuntimeContext {
    std::filesystem::path cwd_;
    std::unordered_map<std::string, std::string> env_;
    std::optional<GitStatus> git_status_;
    std::optional<ProjectContext> project_context_;
    std::string os_;
    std::string arch_;
    std::string username_;

public:
    RuntimeContext() {
        cwd_ = std::filesystem::current_path();
        os_ = "Unknown";
        arch_ = "Unknown";
        
        #ifdef __APPLE__
        os_ = "macOS";
        #elif __linux__
        os_ = "Linux";
        #elif _WIN32
        os_ = "Windows";
        #endif
        
        #ifdef __x86_64__
        arch_ = "x86_64";
        #elif __aarch64__
        arch_ = "arm64";
        #elif defined(__arm__)
        arch_ = "arm";
        #elif defined(__i386__)
        arch_ = "x86";
        #endif
        
        const char* user = std::getenv("USER");
        if (!user) user = std::getenv("USERNAME"); // Windows fallback
        username_ = user ? std::string(user) : "unknown";
    }

    /// Construct with explicit working directory
    explicit RuntimeContext(std::filesystem::path cwd)
        : RuntimeContext() {
        cwd_ = std::move(cwd);
    }

    /// Get current working directory
    [[nodiscard]] const std::filesystem::path& get_cwd() const { return cwd_; }

    /// Set working directory
    void set_cwd(std::filesystem::path path) { cwd_ = std::move(path); }

    /// Get operating system name
    [[nodiscard]] const std::string& get_os() const { return os_; }

    /// Get architecture
    [[nodiscard]] const std::string& get_arch() const { return arch_; }

    /// Get username
    [[nodiscard]] const std::string& get_username() const { return username_; }

    /// Get environment variable
    [[nodiscard]] std::optional<std::string> get_env(const std::string& key) const {
        auto it = env_.find(key);
        if (it != env_.end()) {
            return it->second;
        }
        const char* val = std::getenv(key.c_str());
        return val ? std::make_optional<std::string>(val) : std::nullopt;
    }

    /// Set environment variable (local override)
    void set_env(const std::string& key, std::string value) {
        env_[key] = std::move(value);
    }

    /// Get git status
    [[nodiscard]] const std::optional<GitStatus>& get_git_status() const {
        return git_status_;
    }

    /// Refresh git status by executing git commands
    void refresh_git_status() {
        namespace fs = std::filesystem;

        GitStatus status;

        // Check if we're in a git repository
        std::string git_dir = detail::exec_git("rev-parse --git-dir", cwd_);
        if (git_dir.empty()) {
            status.is_git_repository = false;
            git_status_ = std::move(status);
            return;
        }
        status.is_git_repository = true;

        // Get current branch
        std::string branch = detail::exec_git("rev-parse --abbrev-ref HEAD", cwd_);
        if (!branch.empty() && branch != "HEAD") {
            status.current_branch = branch;
        } else {
            // Detached HEAD — get short hash
            std::string hash = detail::exec_git("rev-parse --short HEAD", cwd_);
            status.current_branch = "detached:" + hash;
        }

        // Get last commit message
        std::string last_commit = detail::exec_git("log -1 --format=%s", cwd_);
        if (!last_commit.empty()) {
            status.last_commit = last_commit;
        }

        // Check for remote
        std::string remote = detail::exec_git("remote", cwd_);
        status.has_remote = !remote.empty();

        // Get ahead/behind counts
        if (status.has_remote && status.current_branch) {
            std::string upstream = detail::exec_git(
                "rev-list --left-right --count @{upstream}...HEAD", cwd_);
            if (!upstream.empty()) {
                // Format: "behind\tahead"
                auto tab = upstream.find('\t');
                if (tab != std::string::npos) {
                    try {
                        status.behind = std::stoi(upstream.substr(0, tab));
                        status.ahead = std::stoi(upstream.substr(tab + 1));
                    } catch (...) {}
                }
            }
        }

        // Parse porcelain status for file lists
        std::string porcelain = detail::exec_git("status --porcelain=v1", cwd_);
        for (const auto& line : detail::split_lines(porcelain)) {
            if (line.size() < 4) continue;
            char index_status = line[0];
            char work_status = line[1];
            std::string file = line.substr(3);

            // Staged files (index has changes)
            if (index_status == 'M' || index_status == 'A' || index_status == 'D' ||
                index_status == 'R' || index_status == 'C') {
                status.staged_files.push_back(file);
            }
            // Modified in working tree
            if (work_status == 'M' || work_status == 'D') {
                status.modified_files.push_back(file);
            }
            // Untracked
            if (index_status == '?' && work_status == '?') {
                status.untracked_files.push_back(file);
            }
        }

        git_status_ = std::move(status);
    }

    /// Get project context
    [[nodiscard]] const std::optional<ProjectContext>& get_project_context() const {
        return project_context_;
    }

    /// Refresh project context by scanning filesystem
    void refresh_project_context() {
        ProjectContext ctx;
        ctx.root_directory = find_project_root();
        ctx.project_name = ctx.root_directory.filename().string();
        ctx.has_git = std::filesystem::exists(ctx.root_directory / ".git");
        ctx.detected_languages = detail::detect_languages(ctx.root_directory);
        ctx.package_manager = detail::detect_package_manager(ctx.root_directory);
        ctx.config_files = detail::find_config_files(ctx.root_directory);
        ctx.has_ci = detail::detect_ci(ctx.root_directory);
        ctx.project_version = detail::detect_version(ctx.root_directory);

        project_context_ = std::move(ctx);
    }

    /// Refresh all context
    void refresh() {
        refresh_git_status();
        refresh_project_context();
    }

    /// Get a summary string for prompt context
    [[nodiscard]] std::string get_summary() const {
        std::string summary;
        summary += std::format("OS: {} ({}), User: {}\n", os_, arch_, username_);
        summary += std::format("CWD: {}\n", cwd_.string());

        if (git_status_ && git_status_->is_git_repository) {
            summary += std::format("Git: branch={}, modified={}, staged={}, untracked={}\n",
                git_status_->current_branch.value_or("unknown"),
                git_status_->modified_files.size(),
                git_status_->staged_files.size(),
                git_status_->untracked_files.size());
        }

        if (project_context_) {
            summary += std::format("Project: {}, Languages: [{}]\n",
                project_context_->project_name,
                [&]() {
                    std::string joined;
                    for (size_t i = 0; i < project_context_->detected_languages.size(); ++i) {
                        if (i > 0) joined += ", ";
                        joined += project_context_->detected_languages[i];
                    }
                    return joined;
                }());
        }

        return summary;
    }

private:
    /// Walk up from cwd to find the project root (directory containing .git, package.json, etc.)
    [[nodiscard]] std::filesystem::path find_project_root() const {
        namespace fs = std::filesystem;
        static const std::vector<std::string> root_markers = {
            ".git", "package.json", "Cargo.toml", "go.mod",
            "CMakeLists.txt", "pyproject.toml", "pom.xml"
        };

        fs::path dir = cwd_;
        while (true) {
            for (const auto& marker : root_markers) {
                if (fs::exists(dir / marker)) {
                    return dir;
                }
            }
            auto parent = dir.parent_path();
            if (parent == dir) break; // Reached filesystem root
            dir = parent;
        }
        return cwd_; // Fallback to cwd
    }
};

} // namespace cc::core
