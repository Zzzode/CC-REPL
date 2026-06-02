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

// ─── Native Installer (原生安装器) ──────────────────────────

enum class InstallTarget { user_local, system_global, custom_path };

struct InstallConfig {
    InstallTarget target{InstallTarget::user_local};
    std::filesystem::path custom_path;
    bool add_to_path{true};
    bool create_symlink{true};
    bool install_completions{true};  // shell 补全脚本
};

struct InstallResult {
    bool success;
    std::filesystem::path installed_path;
    std::optional<std::string> error;
};

class NativeInstaller {
public:
    // 检测安装状态
    [[nodiscard]] static auto is_installed() -> bool {
        // 检查 cc-repl 是否在 PATH 中
        return std::system("which cc-repl > /dev/null 2>&1") == 0;
    }
    
    // 获取安装路径
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
    
    // 安装/更新
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
        if (!marker) return {false, installed, "无法写入安装路径"};
        marker << "";
        std::filesystem::permissions(installed,
            std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::add);
        if (config.install_completions) {
            if (auto completions = install_completions(); !completions) return {false, installed, completions.error()};
        }
        return {true, installed};
    }
    
    // 安装 shell 补全
    [[nodiscard]] static auto install_completions(std::string_view shell = "zsh") 
        -> std::expected<void, std::string> {
        auto dir = get_local_bin().parent_path() / "share" / "cc-repl" / "completions";
        std::filesystem::create_directories(dir);
        auto path = dir / ("cc-repl." + std::string(shell));
        std::ofstream out(path);
        if (!out) return std::unexpected("无法写入补全脚本");
        out << "#compdef cc-repl\n# generated completion shim\n";
        return {};
    }
    
    // 卸载
    [[nodiscard]] static auto uninstall() -> std::expected<void, std::string> {
        auto path = get_install_path();
        if (!path) return std::unexpected("未找到安装");
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

// ─── DXT (打包工具辅助) ─────────────────────────────────────

struct DxtManifest {
    std::string name;
    std::string version;
    std::string description;
    std::string entry_point;
    std::vector<std::string> files;
};

class DxtUtils {
public:
    // 解析 DXT 清单
    [[nodiscard]] static auto parse_manifest(const std::filesystem::path& path) 
        -> std::expected<DxtManifest, std::string> {
        if (!std::filesystem::exists(path)) return std::unexpected("清单文件不存在");
        std::ifstream in(path);
        if (!in) return std::unexpected("无法读取清单文件");
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
        return DxtManifest{.name = extract("name"), .version = extract("version"), .description = extract("description"), .entry_point = extract("entry_point")};
    }
    
    // 打包 DXT
    [[nodiscard]] static auto pack(const std::filesystem::path& dir, const std::filesystem::path& output)
        -> std::expected<void, std::string> {
        if (!std::filesystem::exists(dir)) return std::unexpected("目录不存在");
        std::ofstream out(output);
        if (!out) return std::unexpected("无法创建 DXT 包");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) out << std::filesystem::relative(entry.path(), dir).string() << "\n";
        }
        return {};
    }
    
    // 解包 DXT
    [[nodiscard]] static auto unpack(const std::filesystem::path& archive, const std::filesystem::path& dir)
        -> std::expected<void, std::string> {
        if (!std::filesystem::exists(archive)) return std::unexpected("归档文件不存在");
        std::filesystem::create_directories(dir);
        std::filesystem::copy_file(archive, dir / archive.filename(), std::filesystem::copy_options::overwrite_existing);
        return {};
    }
};

// ─── Background Utils (后台任务) ─────────────────────────────

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
    // 创建后台任务
    [[nodiscard]] auto create(std::string name) -> std::string {
        auto id = "bg_" + std::to_string(tasks_.size());
        tasks_.push_back({.id = id, .name = std::move(name), 
                         .created_at = std::chrono::system_clock::now()});
        return id;
    }
    
    // 更新进度
    void update_progress(std::string_view id, double progress) {
        if (auto* t = find(id)) t->progress = progress;
    }
    
    // 标记完成
    void complete(std::string_view id, std::string result) {
        if (auto* t = find(id)) {
            t->state = BackgroundTaskState::completed;
            t->completed_at = std::chrono::system_clock::now();
            t->result = std::move(result);
            t->progress = 1.0;
        }
    }
    
    // 标记失败
    void fail(std::string_view id, std::string error) {
        if (auto* t = find(id)) {
            t->state = BackgroundTaskState::failed;
            t->error = std::move(error);
        }
    }
    
    // 取消任务
    void cancel(std::string_view id) {
        if (auto* t = find(id)) t->state = BackgroundTaskState::cancelled;
    }
    
    // 获取所有任务
    [[nodiscard]] auto get_all() const -> const std::vector<BackgroundTask>& { return tasks_; }
    
    // 获取活跃任务
    [[nodiscard]] auto get_active() const -> std::vector<const BackgroundTask*> {
        std::vector<const BackgroundTask*> result;
        for (const auto& t : tasks_) {
            if (t.state == BackgroundTaskState::running || t.state == BackgroundTaskState::pending)
                result.push_back(&t);
        }
        return result;
    }
    
    // 清理已完成任务
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
