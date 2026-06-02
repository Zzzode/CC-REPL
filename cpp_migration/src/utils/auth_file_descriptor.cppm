module;

#include <string>
#include <string_view>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

export module cc.utils.auth_file_descriptor;

export namespace cc::utils {

namespace fs = std::filesystem;

// Read a credential file securely (verifies permissions first)
inline std::expected<std::string, std::string>
read_credential_file(const fs::path& path) {
    if (!fs::exists(path)) {
        return std::unexpected("Credential file not found: " + path.string());
    }

    // Check file permissions — warn if too permissive
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return std::unexpected("Cannot stat credential file: " + std::string(strerror(errno)));
    }

    // Verify it's a regular file
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected("Credential path is not a regular file: " + path.string());
    }

    // Read file content
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open credential file: " + path.string());
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

    // Trim trailing whitespace/newlines
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                                content.back() == ' ' || content.back() == '\t')) {
        content.pop_back();
    }

    return content;
}

// Write a credential file securely (creates with 0600 permissions)
inline std::expected<void, std::string>
write_credential_file(const fs::path& path, std::string_view content) {
    // Ensure parent directory exists
    auto parent = path.parent_path();
    if (!fs::exists(parent)) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            return std::unexpected("Cannot create directory: " + parent.string() + ": " + ec.message());
        }
        // Set directory permissions to 0700
        chmod(parent.c_str(), S_IRWXU);
    }

    // Write file with restrictive permissions
    // First, create the file with O_CREAT | O_WRONLY | O_TRUNC and mode 0600
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return std::unexpected("Cannot create credential file: " + path.string() +
                             ": " + std::string(strerror(errno)));
    }

    // Write content
    ssize_t written = write(fd, content.data(), content.size());
    close(fd);

    if (written != static_cast<ssize_t>(content.size())) {
        return std::unexpected("Failed to write complete content to: " + path.string());
    }

    // Ensure permissions are correct (in case file already existed)
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected("Cannot set permissions on: " + path.string());
    }

    return {};
}

// Delete a credential file securely
inline bool delete_credential_file(const fs::path& path) {
    if (!fs::exists(path)) return true;

    // Overwrite with zeros before deletion (basic secure delete)
    auto size = fs::file_size(path);
    if (size > 0) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (file.is_open()) {
            std::string zeros(size, '\0');
            file.write(zeros.data(), zeros.size());
            file.close();
        }
    }

    std::error_code ec;
    return fs::remove(path, ec);
}

} // namespace cc::utils
