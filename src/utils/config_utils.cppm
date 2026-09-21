module;
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.config_utils;

export namespace cc::utils {

namespace fs = std::filesystem;

// Simple JSON value type for config management
using JsonValue = std::variant<std::string, int, double, bool, std::nullptr_t>;

// Load a JSON config file (simplified key-value parser)
std::expected<std::map<std::string, JsonValue>, std::string> load_config_file(fs::path filepath) {
    if (!fs::exists(filepath)) {
        return std::unexpected("Config file not found: " + filepath.string());
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
        return std::unexpected("Cannot open config file: " + filepath.string());
    }

    std::map<std::string, JsonValue> result;
    std::string line;

    // Simple line-by-line JSON parser for flat objects
    while (std::getline(file, line)) {
        // Trim whitespace
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip braces and comments
        if (line[0] == '{' || line[0] == '}' || line[0] == '/' || line[0] == '#') continue;

        // Find key
        auto key_start = line.find('"');
        if (key_start == std::string::npos) continue;
        auto key_end = line.find('"', key_start + 1);
        if (key_end == std::string::npos) continue;

        std::string key = line.substr(key_start + 1, key_end - key_start - 1);

        // Find colon
        auto colon = line.find(':', key_end);
        if (colon == std::string::npos) continue;

        // Parse value
        auto val_start = line.find_first_not_of(" \t", colon + 1);
        if (val_start == std::string::npos) continue;

        std::string_view val_sv(line.data() + val_start, line.size() - val_start);
        // Remove trailing comma
        if (val_sv.ends_with(",")) val_sv.remove_suffix(1);

        if (val_sv.starts_with("\"")) {
            // String value
            auto close = val_sv.find('"', 1);
            if (close != std::string_view::npos) {
                result[key] = std::string(val_sv.substr(1, close - 1));
            }
        } else if (val_sv == "true") {
            result[key] = true;
        } else if (val_sv == "false") {
            result[key] = false;
        } else if (val_sv == "null") {
            result[key] = nullptr;
        } else if (val_sv.find('.') != std::string_view::npos) {
            result[key] = std::stod(std::string(val_sv));
        } else {
            result[key] = std::stoi(std::string(val_sv));
        }
    }

    return result;
}

// Merge two config maps (override takes precedence)
std::map<std::string, JsonValue> merge_configs(
    const std::map<std::string, JsonValue>& base,
    const std::map<std::string, JsonValue>& override_) {
    auto result = base;
    for (auto& [k, v] : override_) {
        result[k] = v;
    }
    return result;
}

// Get a typed config value
template<typename T>
std::optional<T> get_config_value(const std::map<std::string, JsonValue>& config, std::string_view key) {
    auto it = config.find(std::string(key));
    if (it == config.end()) return std::nullopt;

    if (auto* val = std::get_if<T>(&it->second)) {
        return *val;
    }
    return std::nullopt;
}

// Set a single value in a config file (rewrite approach)
std::expected<void, std::string> set_config_value(fs::path filepath, std::string_view key, JsonValue value) {
    auto config_result = load_config_file(filepath);
    std::map<std::string, JsonValue> config;

    if (config_result) {
        config = *config_result;
    }

    config[std::string(key)] = value;

    // Write back
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return std::unexpected("Cannot write to config file: " + filepath.string());
    }

    file << "{\n";
    bool first = true;
    for (auto& [k, v] : config) {
        if (!first) file << ",\n";
        first = false;
        file << "  \"" << k << "\": ";

        std::visit([&file](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>) {
                file << "\"" << arg << "\"";
            } else if constexpr (std::is_same_v<T, bool>) {
                file << (arg ? "true" : "false");
            } else if constexpr (std::is_same_v<T, int>) {
                file << arg;
            } else if constexpr (std::is_same_v<T, double>) {
                file << arg;
            } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                file << "null";
            }
        }, v);
    }
    file << "\n}\n";

    return {};
}

} // namespace cc::utils
