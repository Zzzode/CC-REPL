module;

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <random>
#include <chrono>
#include <system_error>

export module cc.utils.tempfile;

namespace fs = std::filesystem;

export namespace cc::utils {

namespace detail {


inline std::string generate_unique_name(std::string_view prefix) {

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<uint64_t>(now));
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t random_part = dist(rng);

    std::string name{prefix};

    constexpr char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 12; ++i) {
        name.push_back(hex_chars[random_part & 0xF]);
        random_part >>= 4;
    }
    return name;
}

} // namespace detail


inline fs::path get_temp_dir() {
    std::error_code ec;
    auto tmp = fs::temp_directory_path(ec);
    if (ec) {
        return fs::path{"/tmp"};
    }
    return tmp;
}


class TempFile {
public:
    explicit TempFile(std::string_view prefix = "cc-") {
        auto name = detail::generate_unique_name(prefix);
        path_ = get_temp_dir() / name;

        std::ofstream ofs(path_, std::ios::binary);
        ofs.close();
    }

    ~TempFile() {
        cleanup();
    }


    TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    TempFile& operator=(TempFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }


    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;


    const fs::path& path() const { return path_; }


    void write(std::string_view content) {
        std::ofstream ofs(path_, std::ios::binary | std::ios::trunc);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    }


    std::string read() const {
        std::ifstream ifs(path_, std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) {
            return {};
        }
        auto size = ifs.tellg();
        if (size <= 0) return {};
        std::string content(static_cast<size_t>(size), '\0');
        ifs.seekg(0);
        ifs.read(content.data(), size);
        return content;
    }

private:
    void cleanup() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }

    fs::path path_;
};


class TempDir {
public:
    explicit TempDir(std::string_view prefix = "cc-") {
        auto name = detail::generate_unique_name(prefix);
        path_ = get_temp_dir() / name;
        std::error_code ec;
        fs::create_directories(path_, ec);
    }

    ~TempDir() {
        cleanup();
    }


    TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    TempDir& operator=(TempDir&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }


    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;


    const fs::path& path() const { return path_; }

private:
    void cleanup() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }

    fs::path path_;
};

} // namespace cc::utils
