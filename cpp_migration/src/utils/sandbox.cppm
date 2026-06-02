module;

#include <chrono>
#include <array>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

export module cc.utils.sandbox;


export namespace cc::utils {

// 沙箱类型
enum class SandboxType { none, docker, nsjail, firejail, macos_sandbox };

// 沙箱能力
struct SandboxCapabilities {
    bool network_access{false};
    bool filesystem_read{true};
    bool filesystem_write{false};
    bool process_spawn{false};
    std::vector<std::string> allowed_paths;      // 读写白名单
    std::vector<std::string> readonly_paths;     // 只读挂载
    std::vector<std::string> blocked_syscalls;
    size_t memory_limit_mb{512};
    size_t cpu_limit_percent{50};
    std::chrono::seconds time_limit{60};
};

// 沙箱执行结果
struct SandboxResult {
    int exit_code{0};
    std::string stdout_output;
    std::string stderr_output;
    std::chrono::milliseconds execution_time{0};
    size_t memory_peak_mb{0};
    bool timed_out{false};
    bool oom_killed{false};
};

// 沙箱配置
struct SandboxConfig {
    SandboxType type{SandboxType::none};
    SandboxCapabilities capabilities;
    std::string working_dir;
    std::vector<std::pair<std::string, std::string>> env_vars;
    std::optional<std::string> container_image;  // Docker 镜像
};

// 沙箱适配器接口
class SandboxAdapter {
protected:
    SandboxConfig config_;
    bool active_{false};

public:
    explicit SandboxAdapter(SandboxConfig config) : config_(std::move(config)) {}
    virtual ~SandboxAdapter() = default;

    // 执行命令
    [[nodiscard]] virtual auto execute(std::string_view command) 
        -> std::expected<SandboxResult, std::string> = 0;
    
    // 写入文件 (受限于 allowed_paths)
    [[nodiscard]] virtual auto write_file(const std::filesystem::path& path, std::string_view content)
        -> std::expected<void, std::string> = 0;
    
    // 读取文件
    [[nodiscard]] virtual auto read_file(const std::filesystem::path& path)
        -> std::expected<std::string, std::string> = 0;
    
    // 启动/停止
    virtual auto start() -> std::expected<void, std::string> = 0;
    virtual void stop() = 0;
    
    [[nodiscard]] auto is_active() const -> bool { return active_; }
    [[nodiscard]] auto get_type() const -> SandboxType { return config_.type; }
};

// 无沙箱 (直接执行)
class NoSandbox : public SandboxAdapter {
public:
    NoSandbox() : SandboxAdapter({.type = SandboxType::none}) {}
    
    auto execute(std::string_view command) -> std::expected<SandboxResult, std::string> override {
        auto start = std::chrono::steady_clock::now();
        std::string cmd(command);
        cmd += " 2>&1";
        std::array<char, 4096> buffer{};
        std::string output;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return std::unexpected("无法启动命令");
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        int status = pclose(pipe);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return SandboxResult{.exit_code = status, .stdout_output = output, .execution_time = elapsed};
    }
    auto write_file(const std::filesystem::path& path, std::string_view content) 
        -> std::expected<void, std::string> override {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        if (!out) return std::unexpected("无法写入文件: " + path.string());
        out << content;
        return {};
    }
    auto read_file(const std::filesystem::path& path) -> std::expected<std::string, std::string> override {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::unexpected("无法读取文件: " + path.string());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    auto start() -> std::expected<void, std::string> override { active_ = true; return {}; }
    void stop() override { active_ = false; }
};

// Docker 沙箱
class DockerSandbox : public SandboxAdapter {
    std::string container_id_;
public:
    explicit DockerSandbox(SandboxConfig config) : SandboxAdapter(std::move(config)) {}
    
    auto execute(std::string_view command) -> std::expected<SandboxResult, std::string> override {
        if (!active_) return std::unexpected("沙箱未启动");
        NoSandbox runner;
        return runner.execute("docker exec " + container_id_ + " sh -lc '" + std::string(command) + "'");
    }
    auto write_file(const std::filesystem::path& path, std::string_view content)
        -> std::expected<void, std::string> override {
        auto host_path = std::filesystem::temp_directory_path() / ("cc-repl-sandbox-" + path.filename().string());
        NoSandbox local;
        auto wrote = local.write_file(host_path, content);
        if (!wrote) return wrote;
        auto copied = local.execute("docker cp " + host_path.string() + " " + container_id_ + ":" + path.string());
        std::filesystem::remove(host_path);
        if (!copied || copied->exit_code != 0) return std::unexpected("docker cp 写入失败");
        return {};
    }
    auto read_file(const std::filesystem::path& path) -> std::expected<std::string, std::string> override {
        NoSandbox runner;
        auto result = runner.execute("docker exec " + container_id_ + " cat " + path.string());
        if (!result || result->exit_code != 0) return std::unexpected("docker 读取失败");
        return result->stdout_output;
    }
    auto start() -> std::expected<void, std::string> override {
        auto image = config_.container_image.value_or("alpine:latest");
        auto command = "docker run -d --rm --memory " + std::to_string(config_.capabilities.memory_limit_mb) +
            "m --cpus " + std::to_string(std::max<std::size_t>(1, config_.capabilities.cpu_limit_percent) / 100.0) +
            " " + image + " sleep 86400";
        NoSandbox runner;
        auto result = runner.execute(command);
        if (!result || result->exit_code != 0) return std::unexpected("docker run 失败");
        container_id_ = result->stdout_output;
        if (auto pos = container_id_.find('\n'); pos != std::string::npos) container_id_.erase(pos);
        active_ = !container_id_.empty();
        return {};
    }
    void stop() override {
        if (active_ && !container_id_.empty()) {
            NoSandbox runner;
            (void)runner.execute("docker stop " + container_id_);
        }
        active_ = false;
    }
};

// 沙箱工厂
[[nodiscard]] inline auto create_sandbox(SandboxConfig config) -> std::unique_ptr<SandboxAdapter> {
    switch (config.type) {
        case SandboxType::docker: return std::make_unique<DockerSandbox>(std::move(config));
        default: return std::make_unique<NoSandbox>();
    }
}

// 检测可用沙箱类型
[[nodiscard]] inline auto detect_available_sandboxes() -> std::vector<SandboxType> {
    std::vector<SandboxType> available{SandboxType::none};
    auto has_cmd = [](std::string_view name) {
        return std::system(("command -v " + std::string(name) + " >/dev/null 2>&1").c_str()) == 0;
    };
    if (has_cmd("docker")) available.push_back(SandboxType::docker);
    if (has_cmd("nsjail")) available.push_back(SandboxType::nsjail);
    if (has_cmd("firejail")) available.push_back(SandboxType::firejail);
#ifdef __APPLE__
    if (has_cmd("sandbox-exec")) available.push_back(SandboxType::macos_sandbox);
#endif
    return available;
}

// 判断命令是否需要沙箱
[[nodiscard]] inline auto should_sandbox(std::string_view command) -> bool {
    // 危险模式: rm -rf, curl | sh, wget, 等
    static const std::vector<std::string_view> dangerous = {
        "rm -rf /", "mkfs", "dd if=", "curl | sh", "wget -O- |", ":(){ :|:& };:"
    };
    for (auto pattern : dangerous) {
        if (command.find(pattern) != std::string_view::npos) return true;
    }
    return false;
}

} // namespace cc::utils
