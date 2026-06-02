module;

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <array>
#include <system_error>

export module cc.utils.cwd;

namespace fs = std::filesystem;

export namespace cc::utils {

// 获取当前工作目录
inline fs::path get_cwd() {
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) {
        return fs::path{"."};
    }
    return cwd;
}

// 设置当前工作目录
inline bool set_cwd(const fs::path& new_dir) {
    std::error_code ec;
    fs::current_path(new_dir, ec);
    return !ec;
}

// RAII 守卫：在作用域结束时恢复原始工作目录
class ScopedCwd {
public:
    explicit ScopedCwd(const fs::path& new_dir)
        : original_dir_(get_cwd()), restored_(false) {
        set_cwd(new_dir);
    }

    ~ScopedCwd() {
        restore();
    }

    // 提前恢复
    void restore() {
        if (!restored_) {
            set_cwd(original_dir_);
            restored_ = true;
        }
    }

    // 获取保存的原始目录
    const fs::path& original() const { return original_dir_; }

    // 禁止拷贝
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

    // 允许移动
    ScopedCwd(ScopedCwd&& other) noexcept
        : original_dir_(std::move(other.original_dir_)), restored_(other.restored_) {
        other.restored_ = true;
    }

private:
    fs::path original_dir_;
    bool restored_;
};

// 在指定目录下执行回调，执行完后自动恢复
inline void with_cwd(const fs::path& dir, std::function<void()> fn) {
    ScopedCwd guard(dir);
    fn();
}

// 向上搜索项目根目录标记文件
inline std::optional<fs::path> find_project_root(fs::path start) {
    // 项目根目录的标记文件列表
    static constexpr std::array<const char*, 8> markers = {
        ".git",
        "package.json",
        "Cargo.toml",
        "CMakeLists.txt",
        "go.mod",
        "pyproject.toml",
        "Makefile",
        ".project-root"
    };

    fs::path current = fs::absolute(start);
    fs::path root = current.root_path();

    while (true) {
        for (const auto* marker : markers) {
            std::error_code ec;
            if (fs::exists(current / marker, ec)) {
                return current;
            }
        }

        // 到达文件系统根目录则停止
        if (current == root) {
            break;
        }
        auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return std::nullopt;
}

// 默认参数版本的重载
inline std::optional<fs::path> find_project_root() {
    return find_project_root(get_cwd());
}

} // namespace cc::utils
