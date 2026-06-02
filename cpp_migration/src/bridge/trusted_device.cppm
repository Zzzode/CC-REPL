module;
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <unistd.h>

export module cc.bridge.trusted_device;

export namespace cc::bridge {

// Information about a registered trusted device
struct DeviceInfo {
    std::string id;
    std::string name;
    std::chrono::system_clock::time_point registered;
    bool is_current;
};

// Internal helpers
inline std::string get_devices_directory();
inline std::string get_current_device_id();
inline std::expected<void, std::string> save_device_info(const DeviceInfo& info);
inline bool is_valid_device_id(std::string_view device_id);

// Register the current device as trusted
std::expected<DeviceInfo, std::string> register_device() {
    // Generate a unique device ID
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream id_oss;
    id_oss << std::hex << std::setfill('0') << std::setw(16) << dist(gen);
    std::string device_id = "dev-" + id_oss.str();

    // Get device name from hostname
    std::string device_name = "unknown";
#ifdef __unix__
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        device_name = hostname;
    }
#endif

    DeviceInfo info{
        .id = device_id,
        .name = device_name,
        .registered = std::chrono::system_clock::now(),
        .is_current = true
    };

    // Persist device registration
    auto result = save_device_info(info);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }

    return info;
}

// Get all trusted devices
std::vector<DeviceInfo> get_trusted_devices() {
    std::vector<DeviceInfo> devices;

    std::string devices_dir = get_devices_directory();
    if (!std::filesystem::exists(devices_dir)) {
        return devices;
    }

    std::string current_id = get_current_device_id();

    for (const auto& entry : std::filesystem::directory_iterator(devices_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) continue;

        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

        // Simple parsing of device info JSON
        DeviceInfo info;
        info.id = entry.path().stem().string();

        // Extract name from JSON content
        auto name_pos = content.find("\"name\"");
        if (name_pos != std::string::npos) {
            auto colon_pos = content.find(':', name_pos);
            auto quote_start = content.find('"', colon_pos + 1);
            auto quote_end = content.find('"', quote_start + 1);
            if (quote_start != std::string::npos && quote_end != std::string::npos) {
                info.name = content.substr(quote_start + 1, quote_end - quote_start - 1);
            }
        }

        const auto modified = std::filesystem::last_write_time(entry.path());
        info.registered = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            modified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        info.is_current = (info.id == current_id);

        devices.push_back(std::move(info));
    }

    return devices;
}

// Check if the current device is trusted
bool is_device_trusted() {
    std::string current_id = get_current_device_id();
    if (current_id.empty()) return false;

    std::string device_file = get_devices_directory() + "/" + current_id + ".json";
    return std::filesystem::exists(device_file);
}

// Revoke trust for a specific device
std::expected<void, std::string> revoke_device(std::string_view device_id) {
    if (device_id.empty()) {
        return std::unexpected("Device ID cannot be empty");
    }
    if (!is_valid_device_id(device_id)) {
        return std::unexpected("Invalid device ID: " + std::string(device_id));
    }

    const auto devices_dir = std::filesystem::path(get_devices_directory());
    const auto device_file = devices_dir / (std::string(device_id) + ".json");

    if (!std::filesystem::exists(device_file)) {
        return std::unexpected("Device not found: " + std::string(device_id));
    }

    std::error_code ec;
    std::filesystem::remove(device_file, ec);
    if (ec) {
        return std::unexpected("Failed to revoke device: " + ec.message());
    }

    return {};
}

// Internal helpers
inline bool is_valid_device_id(std::string_view device_id) {
    constexpr std::string_view prefix = "dev-";
    if (!device_id.starts_with(prefix) || device_id.size() != prefix.size() + 16) {
        return false;
    }
    return std::all_of(device_id.begin() + static_cast<std::ptrdiff_t>(prefix.size()), device_id.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
}

inline std::string get_devices_directory() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.config/claude-code/bridge/devices";
    }
    return "/tmp/claude-code-bridge-devices";
}

inline std::string get_current_device_id() {
    std::string id_file = get_devices_directory() + "/.current";
    if (!std::filesystem::exists(id_file)) return "";

    std::ifstream ifs(id_file);
    std::string id;
    std::getline(ifs, id);
    return id;
}

inline std::expected<void, std::string> save_device_info(const DeviceInfo& info) {
    std::string devices_dir = get_devices_directory();
    std::filesystem::create_directories(devices_dir);

    // Write device info
    std::string device_file = devices_dir + "/" + info.id + ".json";
    std::ofstream ofs(device_file);
    if (!ofs.is_open()) {
        return std::unexpected("Failed to write device file");
    }

    ofs << "{\"id\":\"" << info.id << "\",\"name\":\"" << info.name << "\"}";
    ofs.close();

    // Mark as current device
    std::string current_file = devices_dir + "/.current";
    std::ofstream curr_ofs(current_file);
    if (curr_ofs.is_open()) {
        curr_ofs << info.id;
    }

    return {};
}

} // namespace cc::bridge
