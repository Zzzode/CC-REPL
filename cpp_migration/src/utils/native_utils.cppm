module;

#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.native_utils;


export namespace cc::utils {



enum class InstallTarget { user_local, system_global, custom_path };

struct InstallConfig {
    InstallTarget target{InstallTarget::user_local};
    std::filesystem::path custom_path;
    bool add_to_path{true};
    bool create_symlink{true};
    bool install_completions{true};
};

struct InstallResult {
    bool success;
    std::filesystem::path installed_path;
    std::optional<std::string> error;
};

class NativeInstaller {
public:

    [[nodiscard]] static auto is_installed() -> bool {

        return std::system("which cc-repl > /dev/null 2>&1") == 0;
    }
    

    [[nodiscard]] static auto get_install_path() -> std::optional<std::filesystem::path> {
        if (auto* path_env = std::getenv("PATH")) {
            std::stringstream paths(path_env);
            std::string dir;
            while (std::getline(paths, dir, ':')) {
                auto candidate = std::filesystem::path(dir) / "cc-repl";
                if (std::filesystem::exists(candidate)) return candidate;
            }
        }
        return std::nullopt;
    }
    

    [[nodiscard]] static auto install(InstallConfig config = {}) -> InstallResult {
        std::filesystem::path target_dir;
        switch (config.target) {
            case InstallTarget::user_local: target_dir = get_local_bin(); break;
            case InstallTarget::system_global: target_dir = "/usr/local/bin"; break;
            case InstallTarget::custom_path: target_dir = config.custom_path; break;
        }
        std::filesystem::create_directories(target_dir);
        auto installed = target_dir / "cc-repl";
        std::ofstream marker(installed, std::ios::app);
        if (!marker) return {false, installed, "failed to write to install path"};
        marker << "";
        std::filesystem::permissions(installed,
            std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::add);
        if (config.install_completions) {
            if (auto completions = install_completions(); !completions) return {false, installed, completions.error()};
        }
        return {true, installed, std::nullopt};
    }
    

    [[nodiscard]] static auto install_completions(std::string_view shell = "zsh") 
        -> std::expected<void, std::string> {
        auto dir = get_local_bin().parent_path() / "share" / "cc-repl" / "completions";
        std::filesystem::create_directories(dir);
        auto path = dir / ("cc-repl." + std::string(shell));
        std::ofstream out(path);
        if (!out) return std::unexpected("failed to write completion script");
        out << "#compdef cc-repl\n# generated completion shim\n";
        return {};
    }
    

    [[nodiscard]] static auto uninstall() -> std::expected<void, std::string> {
        auto path = get_install_path();
        if (!path) return std::unexpected("installation not found");
        std::filesystem::remove(*path);
        return {};
    }

private:
    static auto get_local_bin() -> std::filesystem::path {
        if (auto* home = std::getenv("HOME")) 
            return std::filesystem::path(home) / ".local" / "bin";
        return "/usr/local/bin";
    }
};



struct DxtManifest {
    std::string name;
    std::string version;
    std::string description;
    std::string entry_point;
    std::vector<std::string> files;
};

class DxtUtils {
public:

    [[nodiscard]] static auto parse_manifest(const std::filesystem::path& path) 
        -> std::expected<DxtManifest, std::string> {
        if (!std::filesystem::exists(path)) return std::unexpected("manifest file does not exist");
        std::ifstream in(path);
        if (!in) return std::unexpected("failed to read manifest file");
        std::ostringstream ss;
        ss << in.rdbuf();
        auto text = ss.str();
        auto extract = [&](std::string_view key) {
            auto marker = std::string("\"") + std::string(key) + "\":\"";
            auto pos = text.find(marker);
            if (pos == std::string::npos) return std::string{};
            pos += marker.size();
            auto end = text.find('"', pos);
            return end == std::string::npos ? std::string{} : text.substr(pos, end - pos);
        };
        return DxtManifest{
            .name = extract("name"),
            .version = extract("version"),
            .description = extract("description"),
            .entry_point = extract("entry_point"),
            .files = {},
        };
    }
    

    [[nodiscard]] static auto pack(const std::filesystem::path& dir, const std::filesystem::path& output)
        -> std::expected<void, std::string> {
        if (!std::filesystem::exists(dir)) return std::unexpected("directory does not exist");
        std::ofstream out(output);
        if (!out) return std::unexpected("failed to create DXT package");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) out << std::filesystem::relative(entry.path(), dir).string() << "\n";
        }
        return {};
    }
    

    [[nodiscard]] static auto unpack(const std::filesystem::path& archive, const std::filesystem::path& dir)
        -> std::expected<void, std::string> {
        if (!std::filesystem::exists(archive)) return std::unexpected("archive file does not exist");
        std::filesystem::create_directories(dir);
        std::filesystem::copy_file(archive, dir / archive.filename(), std::filesystem::copy_options::overwrite_existing);
        return {};
    }
};



enum class BackgroundTaskState { pending, running, completed, failed, cancelled };

struct BackgroundTask {
    std::string id;
    std::string name;
    BackgroundTaskState state{BackgroundTaskState::pending};
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
    double progress{0.0};       // 0.0 - 1.0
    std::optional<std::string> result;
    std::optional<std::string> error;
};

class BackgroundTaskManager {
    std::vector<BackgroundTask> tasks_;
public:

    [[nodiscard]] auto create(std::string name) -> std::string {
        auto id = "bg_" + std::to_string(tasks_.size());
        tasks_.push_back({.id = id, .name = std::move(name), 
                         .state = BackgroundTaskState::pending,
                         .created_at = std::chrono::system_clock::now(),
                         .completed_at = std::nullopt,
                         .progress = 0.0,
                         .result = std::nullopt,
                         .error = std::nullopt});
        return id;
    }
    

    void update_progress(std::string_view id, double progress) {
        if (auto* t = find(id)) t->progress = progress;
    }
    

    void complete(std::string_view id, std::string result) {
        if (auto* t = find(id)) {
            t->state = BackgroundTaskState::completed;
            t->completed_at = std::chrono::system_clock::now();
            t->result = std::move(result);
            t->progress = 1.0;
        }
    }
    

    void fail(std::string_view id, std::string error) {
        if (auto* t = find(id)) {
            t->state = BackgroundTaskState::failed;
            t->error = std::move(error);
        }
    }
    

    void cancel(std::string_view id) {
        if (auto* t = find(id)) t->state = BackgroundTaskState::cancelled;
    }
    

    [[nodiscard]] auto get_all() const -> const std::vector<BackgroundTask>& { return tasks_; }
    

    [[nodiscard]] auto get_active() const -> std::vector<const BackgroundTask*> {
        std::vector<const BackgroundTask*> result;
        for (const auto& t : tasks_) {
            if (t.state == BackgroundTaskState::running || t.state == BackgroundTaskState::pending)
                result.push_back(&t);
        }
        return result;
    }
    

    void cleanup(std::chrono::hours max_age = std::chrono::hours{24}) {
        auto cutoff = std::chrono::system_clock::now() - max_age;
        std::erase_if(tasks_, [&](const auto& t) {
            return (t.state == BackgroundTaskState::completed || t.state == BackgroundTaskState::failed)
                   && t.completed_at && *t.completed_at < cutoff;
        });
    }

private:
    auto find(std::string_view id) -> BackgroundTask* {
        for (auto& t : tasks_) if (t.id == id) return &t;
        return nullptr;
    }
};

} // namespace cc::utils
