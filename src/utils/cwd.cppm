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


inline fs::path get_cwd() {
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) {
        return fs::path{"."};
    }
    return cwd;
}


inline bool set_cwd(const fs::path& new_dir) {
    std::error_code ec;
    fs::current_path(new_dir, ec);
    return !ec;
}


class ScopedCwd {
public:
    explicit ScopedCwd(const fs::path& new_dir)
        : original_dir_(get_cwd()), restored_(false) {
        set_cwd(new_dir);
    }

    ~ScopedCwd() {
        restore();
    }


    void restore() {
        if (!restored_) {
            set_cwd(original_dir_);
            restored_ = true;
        }
    }


    const fs::path& original() const { return original_dir_; }


    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;


    ScopedCwd(ScopedCwd&& other) noexcept
        : original_dir_(std::move(other.original_dir_)), restored_(other.restored_) {
        other.restored_ = true;
    }

private:
    fs::path original_dir_;
    bool restored_;
};


inline void with_cwd(const fs::path& dir, std::function<void()> fn) {
    ScopedCwd guard(dir);
    fn();
}


inline std::optional<fs::path> find_project_root(fs::path start) {

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


inline std::optional<fs::path> find_project_root() {
    return find_project_root(get_cwd());
}

} // namespace cc::utils
